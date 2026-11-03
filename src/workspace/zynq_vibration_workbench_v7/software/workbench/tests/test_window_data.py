#!/usr/bin/env python3
"""New Window source protection and declaration QA; generated inputs only."""
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
    parser.add_argument("--probe", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--evidence")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("qa_generator", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    probe_digest = hashlib.sha256(Path(args.probe).read_bytes()).hexdigest()
    results = []
    with tempfile.TemporaryDirectory(prefix="new-window-data-qa-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        original = root / "SYNTHETIC_SOFTWARE_QA_ORIGINAL.BIN"
        generator.generate(original, seconds=65536 / 50000, header_ppr=0, noise_std_g=0)
        current = root / "SOFTWARE_QA_UNMARKED_CONTROL_ONLY.BIN"
        current.write_bytes(original.read_bytes())
        # This unmarked copy is only a source-declaration path control. Its
        # mathematical origin remains explicit in reports and filenames.
        invalid = root / "SOFTWARE_QA_INVALID_METADATA.BIN"
        invalid.write_bytes(original.read_bytes())
        invalid.with_suffix(".meta.json").write_text('{"synthetic": true, broken}', encoding="utf-8")
        baseline = root / "SYNTHETIC_未打开基线.BIN"
        generator.generate(baseline, seconds=65536 / 50000, header_ppr=1, noise_std_g=0)
        known = root / "SYNTHETIC_KNOWN_PPR.BIN"
        generator.generate(known, seconds=65536 / 50000, header_ppr=1, noise_std_g=0)
        alias = root / "baseline-hardlink.json"
        os.link(baseline, alias)
        originals = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir() if p.is_file()}
        for size in ("1024x600", "1280x720"):
            output = Path(args.evidence) / size if args.evidence else root / (size + "-evidence")
            output.mkdir(parents=True, exist_ok=True)
            process = subprocess.run([args.probe, str(current), str(baseline), str(invalid), str(known), str(alias), str(output), size],
                                     env=dict(os.environ, QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"), XDG_RUNTIME_DIR=str(runtime)),
                                     capture_output=True, text=True, encoding="utf-8", timeout=120)
            (output / "probe.json").write_text(process.stdout, encoding="utf-8")
            (output / "stderr.log").write_text(process.stderr, encoding="utf-8")
            report = json.loads(process.stdout)
            assert process.returncode == 0, (size, report.get("error"), process.stderr)
            assert all(row["passed"] for row in report["checks"])
            assert report["baseline_sha256_before"] == report["baseline_sha256_after"]
            assert report["metadata_sha256_before"] == report["metadata_sha256_after"]
            results.append(dict(size=size, checks=len(report["checks"]), passed=True))
        assert all(hashlib.sha256(path.read_bytes()).hexdigest() == old for path, old in originals.items()), "Original QA inputs were modified"
    assert hashlib.sha256(Path(args.probe).read_bytes()).hexdigest() == probe_digest, "Probe rebuilt during QA"
    summary = dict(schema="new-window-data-qa-summary-v1", passed=True, sizes=results,
                   origin="Generated synthetic fixtures only. The explicitly unmarked control exercises user declarations, never physical evidence.",
                   probe_sha256=probe_digest)
    if args.evidence:
        (Path(args.evidence) / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
