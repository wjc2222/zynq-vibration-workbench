#!/usr/bin/env python3
"""Black-box experimental detection interface, parser and protection QA."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--report")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("generator", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    cases = []
    with tempfile.TemporaryDirectory(prefix="vibration_detection_cli_") as temp:
        root = Path(temp)
        synthetic = root / "synthetic.BIN"
        generator.generate(synthetic, seconds=.2, header_ppr=1, noise_std_g=0)
        capture = root / "软件QA条件声明.BIN"
        capture.write_bytes(synthetic.read_bytes())
        profile = root / "example-experimental.profile"
        result_path = root / "detection.json"
        common = ["--signal", "vibration", "--calibrated", "--setup-id", "software-qa-bench"]

        def run(name, source=capture, options=(), success=True):
            process = subprocess.run([args.executable, str(source), *map(str, options)],
                                     capture_output=True, text=True, encoding="utf-8", timeout=30)
            assert (process.returncode == 0) == success, (name, process.returncode, process.stderr)
            cases.append({"case": name, "exit_code": process.returncode,
                          "stderr": process.stderr.strip(), "expected": "pass" if success else "reject"})
            return process

        original_stdout = run("default legacy JSON remains unconfigured").stdout
        assert "detection" not in json.loads(original_stdout)
        run("baseline requires explicit setup", options=["--write-baseline", profile], success=False)
        run("observed baseline draft", options=[*common, "--write-baseline", profile,
                                                 "--baseline-id", "example_experimental_qa"])
        original_profile = profile.read_text(encoding="utf-8")
        assert "status=draft\n" in original_profile and "rule_count=0\n" in original_profile
        assert "source_kind=vibration\n" in original_profile
        assert "calibration_confirmed=true\n" in original_profile

        def with_rules(text, labels=("normal",), condition="1,ac_rms,absolute,0,1000000"):
            text = re.sub(r"(?m)^rule_count=0$", "rule_count=" + str(len(labels)), text)
            for index, label in enumerate(labels):
                text += f"rule.{index}.id=example_{label}_{index}\nrule.{index}.label={label}\n"
                text += f"rule.{index}.condition_count=1\nrule.{index}.condition.0={condition}\n"
            return text

        valid_profile = with_rules(original_profile)
        profile.write_text(valid_profile, encoding="utf-8")
        profile_options = [*common, "--detection-profile", profile, "--detection-output", result_path]
        run("explicit profile requires separate structured output", options=[*common, "--detection-profile", profile], success=False)
        for label in ("normal", "imbalance", "misalignment", "looseness"):
            profile.write_text(with_rules(original_profile, (label,)), encoding="utf-8")
            process = run("experimental single candidate " + label, options=profile_options)
            detection = json.loads(result_path.read_text(encoding="utf-8"))
            assert detection["status"] == "candidate" and detection["candidate"] == label, detection
            assert detection["classification"] == "not_classified" and detection["validated"] is False
            assert "probability" not in detection and "confidence" not in detection
            assert detection["profile_snapshot"] == profile.read_text(encoding="utf-8"), detection
            assert "detection" not in json.loads(process.stdout), "Legacy JSON contract changed"
        profile.write_text(with_rules(original_profile, ("normal", "imbalance")), encoding="utf-8")
        run("multiple rules conflict", options=profile_options)
        detection = json.loads(result_path.read_text(encoding="utf-8"))
        assert detection["status"] == "conflict" and detection["candidate"] is None
        profile.write_text(with_rules(original_profile, condition="1,ac_rms,absolute,100,200"), encoding="utf-8")
        run("no rule match is inconclusive", options=profile_options)
        detection = json.loads(result_path.read_text(encoding="utf-8"))
        assert detection["status"] == "inconclusive" and detection["candidate"] is None
        zero_ratio = with_rules(original_profile, ("normal", "imbalance"))
        zero_ratio, replaced = re.subn(r"(?m)^(baseline\.\d+=1,ac_rms,)[^\n]+$", r"\g<1>0", zero_ratio)
        assert replaced == 1
        zero_ratio = zero_ratio.replace("rule.1.condition.0=1,ac_rms,absolute,0,1000000",
                                        "rule.1.condition.0=1,ac_rms,ratio,0,1000000")
        profile.write_text(zero_ratio, encoding="utf-8")
        run("zero ratio cannot let normal absolute rule win", options=profile_options)
        detection = json.loads(result_path.read_text(encoding="utf-8"))
        assert detection["status"] == "quality_gated" and detection["candidate"] is None, detection
        profile.write_text(valid_profile, encoding="utf-8")
        for name, source, options in [
            ("synthetic cannot be promoted", synthetic, profile_options),
            ("uncalibrated blocked", capture, [o for o in profile_options if o != "--calibrated"]),
            ("incompatible setup blocked", capture, ["other-setup" if o == "software-qa-bench" else o for o in profile_options]),
        ]:
            run(name, source=source, options=options)
            detection = json.loads(result_path.read_text(encoding="utf-8"))
            assert detection["status"] == "quality_gated" and detection["candidate"] is None, (name, detection)
        malformed = {
            "unknown key": valid_profile + "unexpected=1\n",
            "duplicate key": valid_profile + "id=second\n",
            "NaN": valid_profile.replace("analog_gain=-1\n", "analog_gain=nan\n"),
            "infinity": valid_profile.replace("analog_gain=-1\n", "analog_gain=inf\n"),
            "zero sensitivity": re.sub(r"(?m)^sensor_sensitivity_v_per_g=.*$", "sensor_sensitivity_v_per_g=0", valid_profile),
            "reversed interval": valid_profile.replace("1,ac_rms,absolute,0,1000000", "1,ac_rms,absolute,2,1"),
            "channel outside bounds": valid_profile.replace("1,ac_rms,absolute,0,1000000", "4,ac_rms,absolute,0,1"),
            "too many rules": re.sub(r"(?m)^rule_count=1$", "rule_count=33", valid_profile),
            "too many conditions": valid_profile.replace("condition_count=1", "condition_count=17"),
            "oversized": "#" + "x" * 65536,
            "fake validated status": valid_profile.replace("status=draft", "status=validated"),
            "missing required setup": re.sub(r"(?m)^setup_id=.*\n", "", valid_profile),
        }
        for name, contents in malformed.items():
            profile.write_text(contents, encoding="utf-8")
            result_path.write_text('{"previous":"preserve"}', encoding="utf-8")
            process = run("strict parser: " + name, options=profile_options, success=False)
            assert not process.stdout.strip(), (name, process.stdout)
            assert result_path.read_text(encoding="utf-8") == '{"previous":"preserve"}', name
        profile.write_bytes(valid_profile.encode("utf-8") + b"\xc0\xaf")
        run("invalid UTF8 profile", options=profile_options, success=False)
        profile.write_text(valid_profile, encoding="utf-8")
        protected = {capture: capture.read_bytes(), synthetic: synthetic.read_bytes(),
                     synthetic.with_suffix(".meta.json"): synthetic.with_suffix(".meta.json").read_bytes(),
                     profile: profile.read_bytes()}
        for name, target in (("capture", capture), ("reserved absent sidecar", capture.with_suffix(".meta.json")),
                             ("loaded profile", profile)):
            for option in ("--detection-output", "--write-baseline", "--output"):
                options = [*profile_options, option, target] if option != "--detection-output" else [*common, "--detection-profile", profile, option, target]
                run(f"protected {name}: {option}", options=options, success=False)
                assert all(path.read_bytes() == data for path, data in protected.items())
                assert not capture.with_suffix(".meta.json").exists()
        alias = root / "profile-hardlink.profile"
        os.link(profile, alias)
        run("detection hardlink to profile rejected", options=[*common, "--detection-profile", profile,
                                                               "--detection-output", alias], success=False)
        assert profile.read_bytes() == protected[profile]
        if os.name != "nt":
            symlink = root / "missing-meta-alias.json"
            symlink.symlink_to(capture.with_suffix(".meta.json"))
            run("dangling metadata symlink rejected", options=[*common, "--detection-profile", profile,
                                                               "--detection-output", symlink], success=False)
            assert not capture.with_suffix(".meta.json").exists()
        assert run("default CLI still unchanged after feature use").stdout == original_stdout
    report = {"result": "passed", "scope": "software QA only, no physical detection accuracy", "cases": cases,
              "binary_sha256": hashlib.sha256(Path(args.executable).read_bytes()).hexdigest()}
    if args.report:
        Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(cases)} strict-profile, gates, candidate, conflict, inconclusive and protected-output CLI checks; no accuracy claim")


if __name__ == "__main__":
    main()
