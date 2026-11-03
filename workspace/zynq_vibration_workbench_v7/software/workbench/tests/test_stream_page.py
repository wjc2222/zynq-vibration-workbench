#!/usr/bin/env python3
"""Exercise the real Qt stream page against an explicit synthetic service."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--service", required=True)
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    results = []
    for worker in ("0", "1"):
        with tempfile.TemporaryDirectory(prefix="stream-page-") as temporary:
            root = Path(temporary)
            work = root / "work"; work.mkdir()
            runtime = root / "runtime"; runtime.mkdir(mode=0o700)
            evidence = args.evidence / ("worker-" + worker)
            evidence.mkdir(exist_ok=True)
            process = subprocess.run([args.probe, args.service, str(work), str(evidence)],
                env=dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"), XDG_RUNTIME_DIR=str(runtime),
                         AX7_CAPTURE_WORKER=worker), capture_output=True, text=True,
                encoding="utf-8", timeout=100)
            (evidence / "probe.json").write_text(process.stdout, encoding="utf-8")
            (evidence / "stderr.log").write_text(process.stderr, encoding="utf-8")
            report = json.loads(process.stdout)
            assert process.returncode == 0 and report["result"] == "passed", (worker, report.get("error"), process.stderr)
            results.append(dict(worker=worker, checks=len(report["checks"]), passed=True))
    summary = dict(passed=True, modes=results)
    (args.evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
