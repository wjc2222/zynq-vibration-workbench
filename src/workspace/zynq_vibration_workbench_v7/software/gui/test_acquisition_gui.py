#!/usr/bin/env python3
"""Independent real Qt acquisition-page + real service host acceptance."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--service", required=True)
    parser.add_argument("--evidence")
    parser.add_argument("--flow-only", action="store_true", help="No screenshots or layout acceptance; protocol/action flow only")
    args = parser.parse_args()
    if Path("/dev/ax7z020-capture").exists():
        raise RuntimeError("Host QA refuses access to any real capture device")
    probe_digest = hashlib.sha256(Path(args.probe).read_bytes()).hexdigest()
    service_digest = hashlib.sha256(Path(args.service).read_bytes()).hexdigest()
    summaries = []
    with tempfile.TemporaryDirectory(prefix="capture-gui-qa-") as temporary:
        root = Path(temporary)
        for size in ("1024x600", "1280x720"):
            output = Path(args.evidence) / size if args.evidence else root / (size + "-evidence")
            work = root / (size + "-work")
            output.mkdir(parents=True, exist_ok=True)
            work.mkdir()
            command = [args.probe, args.service, str(output), size, str(work)]
            if args.flow_only:
                command.append("--flow-only")
            process = subprocess.run(command,
                                     env=dict(os.environ, QT_QPA_PLATFORM="offscreen"),
                                     capture_output=True, text=True, encoding="utf-8", timeout=90)
            (output / "probe.json").write_text(process.stdout, encoding="utf-8")
            (output / "probe.stderr.log").write_text(process.stderr, encoding="utf-8")
            report = json.loads(process.stdout)
            assert process.returncode == 0, (size, report.get("error"), str(output / "probe.json"), process.stderr)
            assert report["result"] == "passed" and report["checks"]
            assert all(check["passed"] for check in report["checks"])
            assert [row["rate_hz"] for row in report["captures"]] == [50000,25000,10000,5000]
            assert all(row["synthetic"] for row in report["captures"])
            assert report["actual_analysis"]["source_metadata"]["synthetic"] is True
            expected = {"00-device-unavailable.png", "01-fixture-ready.png", "02-running.png",
                        "03-cancelled.png", "04-completed.png", "05-view-record-analysis.png"}
            if args.flow_only:
                expected = set()
            assert {file.name for file in output.glob("*.png")} == expected
            width, height = map(int, size.split("x"))
            for image in output.glob("*.png"):
                assert struct.unpack_from(">II", image.read_bytes(), 16) == (width, height), str(image)
            summaries.append(dict(size=size, passed=True, checks=len(report["checks"]),
                                  screenshots=sorted(expected), rates_hz=[50000,25000,10000,5000]))
    assert hashlib.sha256(Path(args.probe).read_bytes()).hexdigest() == probe_digest, "Probe rebuilt during two-size QA; rerun against one frozen binary"
    assert hashlib.sha256(Path(args.service).read_bytes()).hexdigest() == service_digest, "Service rebuilt during QA"
    summary = dict(schema="acquisition-gui-qa-summary-v1", flow_only=args.flow_only, created_utc=datetime.now(timezone.utc).isoformat(),
                   passed=True, sizes=summaries, probe_sha256=probe_digest,
                   service_sha256=service_digest,
                   boundary="Real Qt widget interactions and explicit synthetic service fixture on host only; no physical board, sensor, SD or touch validation")
    if args.evidence:
        (Path(args.evidence) / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == "__main__":
    main()
