#!/usr/bin/env python3
"""Black-box CLI output protection, including Unicode paths and file aliases."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("generator", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    cases, skipped = [], []
    with tempfile.TemporaryDirectory(prefix="vibration_cli_output_") as temp:
        root = Path(temp)
        chinese = root / "中文 路径"
        chinese.mkdir()
        source = chinese / "原始振动.BIN"
        generator.generate(source, seconds=0.2, header_ppr=1)
        metadata = source.with_suffix(".meta.json")
        original = {source: source.read_bytes(), metadata: metadata.read_bytes()}

        def run(name, capture, output, reject=True):
            process = subprocess.run([args.executable, str(capture), "--output", str(output)],
                                     cwd=root, capture_output=True, text=True, encoding="utf-8")
            assert (process.returncode != 0) == reject, (name, process.returncode, process.stderr)
            if reject:
                assert "must not overwrite" in process.stderr, (name, process.stderr)
            assert all(path.read_bytes() == before for path, before in original.items()), name
            cases.append({"case": name, "exit_code": process.returncode,
                          "expected": "rejected" if reject else "JSON saved",
                          "stderr": process.stderr.strip(), "originals_unchanged": True})

        run("original BIN, Unicode path", source, source)
        run("original metadata, Unicode path", source, metadata)
        run("relative metadata path", source, metadata.relative_to(root))
        for name, target in (("BIN hard link", source), ("metadata hard link", metadata)):
            link = chinese / ("硬链接" + (".BIN" if target == source else ".json"))
            os.link(target, link)
            run(name, source, link)
        no_metadata = chinese / "没有来源说明.BIN"
        no_metadata.write_bytes(original[source])
        reserved = no_metadata.with_suffix(".meta.json")
        run("reserved metadata filename when absent", no_metadata, reserved)
        assert not reserved.exists()
        if os.name == "nt":
            run("case variant of absent metadata filename", no_metadata, reserved.with_suffix(".JSON"))
            assert not reserved.exists()
        try:
            meta_link = chinese / "来源软链接.json"
            os.symlink(metadata.name, meta_link)
            run("metadata symbolic link", source, meta_link)
            bin_link = chinese / "采集软链接.BIN"
            os.symlink(source.name, bin_link)
            run("BIN symbolic link", source, bin_link)
            alias_meta = bin_link.with_suffix(".meta.json")
            alias_meta.write_bytes(original[metadata])
            original[alias_meta] = alias_meta.read_bytes()
            run("input symlink uses its own same-stem metadata", bin_link, alias_meta)
            dangling = chinese / "悬空来源链接.json"
            os.symlink(reserved.name, dangling)
            run("dangling output alias to absent metadata", no_metadata, dangling)
            assert not reserved.exists()
        except OSError as error:
            skipped.append({"feature": "symbolic links", "reason": str(error)})
        output = chinese / "普通分析结果.json"
        output.write_text('{"previous":true}', encoding="utf-8")
        run("ordinary Unicode JSON output", source, output, reject=False)
        result = json.loads(output.read_text(encoding="utf-8"))
        assert result["source_metadata"]["synthetic"] is True
        assert result["configuration"]["signal_kind"] == "test_signal"
        hashes = [{"filename": path.name, "before_sha256": hashlib.sha256(before).hexdigest(),
                   "after_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                  for path, before in original.items()]
    report = {"result": "passed", "platform": platform.platform(), "cases": cases, "skipped": skipped,
              "preserved_original_hashes": hashes,
              "binary_sha256": hashlib.sha256(Path(args.executable).read_bytes()).hexdigest()}
    Path(args.report).write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"PASS: {len(cases)} CLI output protection cases; {len(skipped)} optional features unavailable")


if __name__ == "__main__":
    main()
