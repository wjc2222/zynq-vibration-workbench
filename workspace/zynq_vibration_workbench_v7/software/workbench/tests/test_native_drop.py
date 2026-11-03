#!/usr/bin/env python3
"""Native Qt event routing, not a claim of physical drag/touch acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--evidence")
    args = parser.parse_args()
    before = hashlib.sha256(Path(args.probe).read_bytes()).hexdigest()
    results = []
    with tempfile.TemporaryDirectory(prefix="native-wave-drop-qa-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        for size in ("1024x600", "1280x720"):
            output = Path(args.evidence) / size if args.evidence else root / size
            output.mkdir(parents=True, exist_ok=True)
            process = subprocess.run([args.probe, str(output), size],
                                     env=dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"), XDG_RUNTIME_DIR=str(runtime)),
                                     capture_output=True, text=True, encoding="utf-8", timeout=45)
            (output / "probe.json").write_text(process.stdout, encoding="utf-8")
            (output / "stderr.log").write_text(process.stderr, encoding="utf-8")
            report = json.loads(process.stdout)
            assert process.returncode == 0, (size, report.get("error"), process.stderr)
            assert report["result"] == "passed" and all(x["passed"] for x in report["checks"])
            assert report["events"] and all(x["action"] == 2 for x in report["events"] if x["drop_accepted"])
            results.append(dict(size=size, checks=len(report["checks"]), native_events=len(report["events"]), passed=True))
    assert hashlib.sha256(Path(args.probe).read_bytes()).hexdigest() == before, "Probe rebuilt during QA"
    summary = dict(schema="native-wave-drop-qa-summary-v1", passed=True, sizes=results, probe_sha256=before,
                   boundary="Real Qt DragEnter/Drop/Mouse/Wheel dispatch with actual DragButton MIME. Generated curves only; physical mouse/touch and hardware not accepted.")
    if args.evidence:
        (Path(args.evidence) / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
