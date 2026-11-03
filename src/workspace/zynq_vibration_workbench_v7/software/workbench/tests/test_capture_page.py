#!/usr/bin/env python3
"""Independent CapturePage host integration. No legacy GUI is loaded."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--service", required=True)
    parser.add_argument("--evidence")
    args = parser.parse_args()
    if Path("/dev/ax7z020-capture").exists():
        raise RuntimeError("Host QA refuses physical capture hardware")
    evidence = Path(args.evidence) if args.evidence else None
    before = hashlib.sha256(Path(args.probe).read_bytes()).hexdigest()
    sizes = []
    with tempfile.TemporaryDirectory(prefix="new-capture-page-qa-") as temp:
        root = Path(temp)
        if evidence is None:
            evidence = root / "evidence"
        runtime = root / "qt-runtime"
        runtime.mkdir(mode=0o700)
        for size in ("1024x600", "1280x720"):
            output = evidence / size
            output.mkdir(parents=True, exist_ok=True)
            work = root / size
            work.mkdir()
            result = subprocess.run([args.probe, args.service, sys.executable,
                                     str(Path(__file__).with_name("fake_capture_peer.py")), str(output), str(work), size],
                                    env=dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"), XDG_RUNTIME_DIR=str(runtime)),
                                    capture_output=True, text=True, encoding="utf-8", timeout=90)
            (output / "probe.json").write_text(result.stdout, encoding="utf-8")
            (output / "stderr.log").write_text(result.stderr, encoding="utf-8")
            report = json.loads(result.stdout)
            assert result.returncode == 0, (size, report.get("error"), result.stderr)
            assert report["result"] == "passed" and all(x["passed"] for x in report["checks"])
            assert [x["rate_hz"] for x in report["captures"]] == [50000,25000,10000,5000]
            assert all(x["synthetic"] for x in report["captures"])
            sizes.append(dict(size=size, checks=len(report["checks"]), passed=True))
    assert hashlib.sha256(Path(args.probe).read_bytes()).hexdigest() == before, "Probe changed during QA"
    summary = dict(schema="new-capture-page-qa-summary-v1", passed=True, sizes=sizes,
                   probe_sha256=before, service_sha256=hashlib.sha256(Path(args.service).read_bytes()).hexdigest(),
                   scope="Independent new CapturePage transport/actions only; no old GUI, final shell or physical hardware acceptance")
    if args.evidence:
        (evidence / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
