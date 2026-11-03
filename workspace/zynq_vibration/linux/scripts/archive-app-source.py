#!/usr/bin/env python3
"""Create the local application source evidence from an explicit allowlist."""
import argparse
import gzip
import hashlib
import io
import json
import pathlib
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("project", type=pathlib.Path)
parser.add_argument("destination", type=pathlib.Path)
args = parser.parse_args()
project = args.project.resolve(strict=True)
destination = args.destination.resolve()
destination.mkdir(parents=True, exist_ok=True)
source_paths = [project / "CMakeLists.txt"] + sorted((project / "software").rglob("*"))
files = {}
payloads = []
for path in source_paths:
    if path.is_symlink():
        raise RuntimeError(f"Application source snapshot refuses symbolic links: {path}")
    if path.is_dir():
        continue
    if not path.is_file():
        raise RuntimeError(f"Unexpected source entry: {path}")
    name = path.relative_to(project).as_posix()
    if name != "CMakeLists.txt" and not name.startswith("software/"):
        raise RuntimeError(f"Source escaped the allowlist: {name}")
    data = path.read_bytes()
    files[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    payloads.append((name, data))
archive = destination / "vibration-offline-0.1.0-source.tar.gz"
with archive.open("wb") as output, gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=1792627200) as zipped:
    with tarfile.open(fileobj=zipped, mode="w") as tar:
        for name, data in payloads:
            item = tarfile.TarInfo("vibration-offline-0.1.0/" + name)
            item.size, item.mode, item.mtime = len(data), 0o644, 1792627200
            tar.addfile(item, io.BytesIO(data))
manifest = {"scope": "Project-owned CMakeLists.txt and software/** only; no project-root traversal or VHD",
            "archive": archive.name, "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "files": files}
(destination / "vibration-offline-source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps({"archive": str(archive), "files": len(files), "sha256": manifest["archive_sha256"]}))
