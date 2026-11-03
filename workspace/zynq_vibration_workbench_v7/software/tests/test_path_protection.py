#!/usr/bin/env python3
"""Relative missing-sidecar regression, C++ API and CLI, using software QA files."""
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
    parser.add_argument("--probe", required=True)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--report")
    parser.add_argument("--legacy-executable")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("generator", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    cases, skipped = [], []
    with tempfile.TemporaryDirectory(prefix="vibration_path_protection_") as temp:
        root = Path(temp)
        cpp = subprocess.run([args.probe, "cpp-workspace"], cwd=root, capture_output=True, text=True, encoding="utf-8", timeout=30)
        assert cpp.returncode == 0, cpp.stderr
        match = re.search(r"PASS: (\d+) relative/absolute path-protection cases; (\d+) optional Windows alias cases skipped", cpp.stdout)
        assert match, cpp.stdout
        if os.name != "nt":
            assert tuple(map(int, match.groups())) == (26, 0), cpp.stdout
        cli = root / "cli"
        cli.mkdir(); (cli / "sub").mkdir()
        reference = cli / "synthetic-reference.BIN"
        generator.generate(reference, seconds=.2, header_ppr=1, noise_std_g=0)
        capture = cli / "example-software-path-qa.BIN"
        capture.write_bytes(reference.read_bytes())
        baseline_source = cli / "example-baseline-source.BIN"
        baseline_source.write_bytes(reference.read_bytes())
        profile = cli / "example-experimental.profile"
        common = ["--signal", "vibration", "--calibrated", "--setup-id", "software-path-qa"]
        created = subprocess.run([args.executable, baseline_source.name, *common, "--write-baseline", profile.name,
                                  "--baseline-id", "example_path_qa"], cwd=cli, capture_output=True, text=True, timeout=30)
        assert created.returncode == 0, created.stderr
        originals = {path: path.read_bytes() for path in (capture, baseline_source, profile, reference, reference.with_suffix(".meta.json"))}
        absent = {capture.with_suffix(".meta.json"), baseline_source.with_suffix(".meta.json")}

        def preserved():
            assert all(path.read_bytes() == value for path, value in originals.items()), "protected source/profile changed"
            assert all(not path.exists() for path in absent), "reserved absent metadata created"

        def run(name, input_name, options, reject=True, untouched=()):
            before = {path: path.read_bytes() for path in untouched if path.exists()}
            missing = {path for path in untouched if not path.exists()}
            process = subprocess.run([args.executable, str(input_name), *map(str, options)], cwd=cli,
                                     capture_output=True, text=True, encoding="utf-8", timeout=30)
            assert (process.returncode != 0) == reject, (name, process.returncode, process.stderr)
            if reject:
                assert "must not overwrite" in process.stderr, (name, process.stderr)
                assert not process.stdout.strip(), (name, "failure printed success JSON")
            assert all(path.read_bytes() == value for path, value in before.items()), name
            assert all(not path.exists() for path in missing), name
            preserved()
            cases.append({"case": name, "input_argument": str(input_name), "options": list(map(str, options)),
                          "exit_code": process.returncode, "expected": "rejected" if reject else "allowed", "protected_bytes_unchanged": True})
            return process

        inputs = [capture.name, str(capture), "./" + capture.name, "sub/../" + capture.name]
        metadata = capture.with_suffix(".meta.json")
        outputs = [metadata.name, str(metadata), "./" + metadata.name, "sub/../" + metadata.name,
                   str(cli / "sub" / ".." / metadata.name)]
        for input_index, input_name in enumerate(inputs):
            for output_index, output in enumerate(outputs):
                for mode in ("legacy", "baseline", "detection"):
                    options = ["--output", output] if mode == "legacy" else [*common, "--write-baseline", output] if mode == "baseline" else [*common, "--detection-profile", profile.name, "--detection-output", output]
                    run(f"missing sidecar namespace input{input_index}/output{output_index}/{mode}", input_name, options)
        ordinary = cli / "batch-first.json"
        ordinary.write_text('{"previous":"keep"}', encoding="utf-8")
        run("batch preflight preserves existing first output", capture.name,
            [*common, "--output", ordinary.name, "--write-baseline", metadata], untouched=[ordinary])
        run("batch relative/absolute same missing output rejected", capture.name,
            [*common, "--output", "duplicate.json", "--write-baseline", cli / "duplicate.json"], untouched=[cli / "duplicate.json"])
        for target in (capture, profile):
            link = cli / (target.stem + "-hardlink.json")
            os.link(target, link)
            if target == capture:
                run("relative BIN hardlink output", capture.name, ["--output", link.name])
            else:
                run("relative loaded-profile hardlink output", capture.name, [*common, "--detection-profile", profile.name, "--detection-output", link.name])
        try:
            dangling = cli / "missing-meta-link.json"
            dangling.symlink_to(metadata.name)
            chain = cli / "missing-meta-chain.json"
            chain.symlink_to(dangling.name)
            directory = cli / "directory-alias"
            directory.symlink_to(cli, target_is_directory=True)
            for output in (dangling.name, str(dangling), chain.name, "directory-alias/" + metadata.name):
                for mode in ("legacy", "baseline", "detection"):
                    options = ["--output", output] if mode == "legacy" else [*common, "--write-baseline", output] if mode == "baseline" else [*common, "--detection-profile", profile.name, "--detection-output", output]
                    run("dangling/directory alias " + mode + ": " + output, capture.name, options)
            alias = cli / "input-alias.BIN"
            alias.symlink_to(capture.name)
            alias_meta = alias.with_suffix(".meta.json")
            absent.add(alias_meta)
            for output in (alias_meta.name, str(alias_meta), "sub/../" + alias_meta.name):
                run("relative input spelling owns absent sidecar", alias.name, ["--output", output])
                run("detection input alias absent sidecar", alias.name, [*common, "--detection-profile", profile.name, "--detection-output", output])
        except OSError as error:
            if os.name != "nt":
                raise
            skipped.append({"feature": "Windows symbolic links", "reason": str(error)})
        for mode in ("legacy", "baseline", "detection"):
            output = cli / ("allowed-" + mode + ".json")
            output.write_text("previous ordinary QA output", encoding="utf-8")
            options = ["--output", output.name] if mode == "legacy" else [*common, "--write-baseline", "sub/../" + output.name] if mode == "baseline" else [*common, "--detection-profile", profile.name, "--detection-output", output]
            run("ordinary output remains allowed: " + mode, capture.name, options, reject=False)
            contents = output.read_text(encoding="utf-8")
            assert "schema=zynq_vibration.detection_profile.v1" in contents if mode == "baseline" else isinstance(json.loads(contents), dict)
        legacy_negative = None
        if args.legacy_executable:
            old = root / "legacy-negative-control"; old.mkdir()
            old_capture = old / capture.name; old_capture.write_bytes(originals[capture])
            old_meta = old_capture.with_suffix(".meta.json")
            # The failing namespace direction is intentionally relative input,
            # absolute missing output. Both-relative alone masked the old bug.
            process = subprocess.run([args.legacy_executable, old_capture.name, "--output", str(old_meta)], cwd=old,
                                     capture_output=True, text=True, encoding="utf-8", timeout=30)
            assert process.returncode == 0 and old_meta.exists(), ("frozen pre-fix CLI did not reproduce known bug", process.stderr)
            legacy_negative = {"expected_old_failure_reproduced": True, "relative_input_argument": old_capture.name,
                               "absolute_reserved_output_created": True, "exit_code": process.returncode,
                               "binary_sha256": hashlib.sha256(Path(args.legacy_executable).read_bytes()).hexdigest()}
        preserved()
        report = {"result": "passed", "scope": "Software path-protection regression only; all capture bytes are generated QA fixtures",
                  "cpp": {"exit_code": cpp.returncode, "cases": int(match[1]), "optional_skipped": int(match[2]), "stdout": cpp.stdout},
                  "cli_cases": cases, "optional_skipped": skipped, "pre_fix_cli_negative_control": legacy_negative,
                  "source_hashes": {path.name: hashlib.sha256(value).hexdigest() for path, value in originals.items()}}
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"PASS: C++ {report['cpp']['cases']} path cases and {len(cases)} CLI path-protection cases; original files preserved")


if __name__ == "__main__":
    main()
