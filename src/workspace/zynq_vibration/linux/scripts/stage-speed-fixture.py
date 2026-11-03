#!/usr/bin/env python3
"""Copy the approved real speed-only capture unchanged into the RAM rootfs."""
import hashlib
import pathlib
import shutil
import sys

project = pathlib.Path(sys.argv[1]).resolve()
destination = pathlib.Path(sys.argv[2]).resolve()
source_dir = project / "data/validation/speed-only"
source = source_dir / "CAP00003.BIN"
expected = "941b1320495af0f1af7b38a5c70bd999e658c8583539c21d4c8ba402bae2f1cb"
if source.stat().st_size != 16004096 or hashlib.sha256(source.read_bytes()).hexdigest() != expected:
    raise RuntimeError("Approved speed-only capture identity mismatch")
if source.with_suffix(".meta.json").exists():
    raise RuntimeError("Unexpected capture sidecar; review provenance before staging it")
destination.mkdir(parents=True, exist_ok=True)
def copy_readonly_input(input_path):
    output = destination / input_path.name
    if output.is_symlink():
        raise RuntimeError(f"Unexpected symlink in rootfs staging: {output}")
    if output.exists():
        output.chmod(0o644)
    shutil.copyfile(input_path, output)
    output.chmod(0o444)

copy_readonly_input(source)
for document in source_dir.iterdir():
    if document.is_file() and document.suffix.lower() in (".md", ".txt", ".json"):
        copy_readonly_input(document)
for path in destination.iterdir():
    if path.is_file():
        path.chmod(0o444)
print(f"Approved real speed-only fixture copied unchanged: {expected}")
