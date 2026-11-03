#!/usr/bin/env python3
"""Real modern-workbench UI actions; no physical measurement or accuracy claim."""
import argparse
import csv
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
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
    with tempfile.TemporaryDirectory(prefix="vibration_workbench_") as temp:
        root = Path(temp)
        synthetic = root / "synthetic_software_QA.BIN"
        generator.generate(synthetic, seconds=65536 / 50000, header_ppr=1, noise_std_g=0)
        special = "<>&" if os.name != "nt" else "&"
        source = root / ("软件QA" + special + "显式条件声明_超长文件名用于验证记录栏省略且保留来源状态.BIN")
        source.write_bytes(synthetic.read_bytes())
        invalid = root / "invalid-experimental.profile"
        invalid.write_text("unexpected=1\n", encoding="utf-8")
        originals = {path: path.read_bytes() for path in root.iterdir() if path.is_file()}
        for size in ("1024x600", "1280x720"):
            output = Path(args.evidence) / size if args.evidence else root / size
            output.mkdir(parents=True, exist_ok=True)
            process = subprocess.run([args.probe, str(source), str(output), size, str(invalid)],
                                     env=dict(os.environ, QT_QPA_PLATFORM="offscreen"),
                                     capture_output=True, text=True, encoding="utf-8", timeout=120)
            (output / "probe.json").write_text(process.stdout, encoding="utf-8")
            (output / "probe.stderr.log").write_text(process.stderr, encoding="utf-8")
            report = json.loads(process.stdout)
            assert process.returncode == 0, (size, report.get("error"), report.get("checks", [])[-1:], str(output / "probe.json"), process.stderr)
            assert report["result"] == "passed" and report["checks"] and all(item["passed"] for item in report["checks"])
            assert report["range_before_sha256"] == report["range_after_sha256"]
            assert report["loaded_profile"]["status"] == "quality_gated" and report["loaded_profile"]["candidate"] is None
            assert len(report["loaded_profile"]["gates"]) == 11
            candidate = report["actual_json"]
            assert candidate["status"] == "candidate" and candidate["candidate"] == "normal", candidate
            assert candidate["classification"] == "not_classified" and candidate["validated"] is False
            assert candidate["profile_snapshot"] and "probability" not in candidate and "confidence" not in candidate
            failed = report["invalid_profile"]
            assert failed["status"] == "invalid_profile" and failed["candidate"] is None and not failed["rules"]
            rows = list(csv.reader(io.StringIO(report["actual_csv"])))
            assert rows[0] == ["field", "channel", "value", "unit"] and all(len(row) == 4 for row in rows)
            full = [json.loads(row[2]) for row in rows if row[0] == "detection.json"]
            assert full == [candidate], "Actual CSV and JSON chosen through dialogs disagree"
            expected = {"09-records.png", "10-spectrum.png", "11-speed.png", "12-range-dialog.png", "13-config-dialog.png",
                        "14-conditions.png", "15-export-options.png", "16-parameters.png", "17-long-profile-candidate.png", "18-numeric-keypad.png", "19-parameters-source-calibration.png"}
            assert {path.name for path in output.glob("*.png")} == expected
            width, height = map(int, size.split("x"))
            for path in output.glob("*.png"):
                w, h = struct.unpack_from(">II", path.read_bytes(), 16)
                assert 0 < w <= width and 0 < h <= height, (path, w, h)
                if path.name in {"09-records.png", "10-spectrum.png", "11-speed.png", "14-conditions.png", "17-long-profile-candidate.png"}:
                    assert (w, h) == (width, height), (path, w, h)
        assert all(path.read_bytes() == before for path, before in originals.items()), "Workbench changed original input fixtures"
    print("PASS: real icon navigation, actual range/export/config/file dialogs, 3 detection tabs, 44px controls, native bounds, range preserves complete analysis, structured JSON/CSV, observed baseline and failed-load reset at both sizes")


if __name__ == "__main__":
    main()
