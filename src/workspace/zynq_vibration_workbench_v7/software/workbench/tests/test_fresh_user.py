#!/usr/bin/env python3
"""Independent Qt user journey; synthetic input and isolated temporary storage."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--service", required=True, type=Path)
    parser.add_argument("--generator", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    args.evidence.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("fresh_user_fixture", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    with tempfile.TemporaryDirectory(prefix="fresh-user-v55-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        fixture = root / "source.BIN"
        generator.generate(fixture, seconds=0.5, header_ppr=1)
        process = subprocess.run(
            [str(args.probe.resolve()), str(fixture), str(args.evidence.resolve()), str(args.service.resolve())],
            env=dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"), XDG_RUNTIME_DIR=str(runtime)),
            capture_output=True, text=True, encoding="utf-8", timeout=90)
        report = json.loads((args.evidence / "report.json").read_text(encoding="utf-8"))
        summary = dict(passed=process.returncode == 0 and report["passed"],
                       checks=len(report["checks"]),
                       probe_sha256=hashlib.sha256(args.probe.read_bytes()).hexdigest(),
                       service_sha256=hashlib.sha256(args.service.read_bytes()).hexdigest(),
                       synthetic_fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                       scope=report["scope"])
        (args.evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(summary))
        assert summary["passed"], (report.get("error"), process.stderr[-2000:])


if __name__ == "__main__":
    main()
