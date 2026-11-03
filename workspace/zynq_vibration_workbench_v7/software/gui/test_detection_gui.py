#!/usr/bin/env python3
"""Independent real-widget detection QA. Fixtures establish no physical accuracy."""
import argparse
import csv
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--evidence")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("generator", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    with tempfile.TemporaryDirectory(prefix="vibration_detection_gui_") as temp:
        root = Path(temp)
        synthetic = root / "明确合成.BIN"
        generator.generate(synthetic, seconds=0.2, header_ppr=1, noise_std_g=0)
        source = root / "软件QA条件路径.BIN"
        source.write_bytes(synthetic.read_bytes())
        bad = root / "invalid.BIN"
        bad.write_bytes(b"invalid software QA fixture")
        originals = {p: p.read_bytes() for p in root.iterdir() if p.is_file()}
        for size in ("1024x600", "1280x720"):
            output = Path(args.evidence) / size if args.evidence else root / size
            output.mkdir(parents=True, exist_ok=True)
            run = subprocess.run([args.probe, str(source), str(synthetic), str(bad), str(output), size],
                                 capture_output=True, text=True, encoding="utf-8", timeout=150,
                                 env=dict(os.environ, QT_QPA_PLATFORM="offscreen"))
            (output / "probe.stderr.log").write_text(run.stderr, encoding="utf-8")
            (output / "probe.json").write_text(run.stdout, encoding="utf-8")
            assert run.returncode == 0, (size, run.stdout, run.stderr)
            report = json.loads(run.stdout)
            assert report["result"] == "passed" and all(c["passed"] for c in report["checks"]), report
            for name in ("no_profile", "invalid_profile", "cleared_profile"):
                detection = report[name]["detection"]
                assert detection["status"] == ("invalid_profile" if name == "invalid_profile" else "not_configured"), (name, detection)
                assert detection["candidate"] is None and detection["classification"] == "not_classified", detection
                assert not detection["rules"], detection
            for name in ("uncalibrated", "incompatible_setup", "synthetic", "zero_baseline_conflict"):
                detection = report[name]["detection"]
                assert detection["status"] == "quality_gated" and detection["candidate"] is None, (name, detection)
                assert any(not gate["passed"] for gate in detection["gates"]), detection
                assert not any(rule["matched"] for rule in detection["rules"]), detection
            candidate = report["candidate"]["detection"]
            assert candidate["status"] == "candidate" and candidate["candidate"] == "normal", candidate
            csv_rows = list(csv.reader(io.StringIO(report["candidate_csv"])))
            assert csv_rows[0] == ["field", "channel", "value", "unit"] and all(len(row) == 4 for row in csv_rows)
            structured = [row[2] for row in csv_rows if row[0] == "detection.json"]
            assert len(structured) == 1 and json.loads(structured[0]) == candidate, structured
            assert any(row[:3] == ["detection.candidate", "", "normal"] for row in csv_rows), csv_rows
            for name, exported in report.items():
                if not isinstance(exported, dict) or "detection" not in exported:
                    continue
                detection = exported["detection"]
                assert detection["validated"] is False and detection["classification"] == "not_classified", (name, detection)
                assert "probability" not in detection and "confidence" not in detection, detection
            assert report["synthetic"]["source_metadata"]["synthetic"] is True
            expected_shots = {"01-no-profile.png", "02-quality-gated.png", "03-candidate.png", "04-features.png",
                              "05-invalid-profile.png", "06-synthetic-gated.png", "07-invalid-capture.png"}
            actual_shots = {path.name for path in output.glob("*.png")}
            assert expected_shots <= actual_shots <= expected_shots | {"08-four-waveform.png"}, (output, actual_shots)
        assert all(path.read_bytes() == content for path, content in originals.items()), "GUI changed source fixtures"
    print("PASS: detection real-widget state, 2 sizes, profile failure reset, source/calibration/setup gates, single candidate, reanalysis, busy guards, protected exports and observed-only baseline. No accuracy claim.")


if __name__ == "__main__":
    main()
