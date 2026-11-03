#!/usr/bin/env python3
"""Qt acquisition-count boundaries using only the explicit host fixture."""
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
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="capture-parameters-") as temporary:
        root = Path(temporary)
        work = root / "work"; work.mkdir()
        runtime = root / "runtime"; runtime.mkdir(mode=0o700)
        evidence = args.evidence or root / "evidence"; evidence.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([args.probe, args.service, str(work), str(evidence)],
            env=dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"), XDG_RUNTIME_DIR=str(runtime)),
            capture_output=True, text=True, encoding="utf-8", timeout=40)
        (evidence / "probe.json").write_text(result.stdout, encoding="utf-8")
        (evidence / "stderr.log").write_text(result.stderr, encoding="utf-8")
        report = json.loads(result.stdout)
        assert result.returncode == 0 and report["result"] == "passed", (report.get("error"), result.stderr)
        print(json.dumps(dict(passed=True, checks=len(report["checks"]), scope=report["scope"])))


if __name__ == "__main__":
    main()
