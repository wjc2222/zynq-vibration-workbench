#!/usr/bin/env python3
"""Inject the explicitly pinned built-in display driver into a fresh kernel tree."""
import hashlib
import json
import pathlib
import shutil
import sys

platform = pathlib.Path(__file__).resolve().parents[1]
project = platform.parent
lock = json.loads((platform / "display-platform.lock.json").read_text())
kernel = pathlib.Path(sys.argv[1]).resolve()
destination = kernel / "drivers/video/fbdev/codex_ax7z020"
for relative, expected in {**lock["source_sha256"], **lock["hardware_sha256"]}.items():
    source = project / relative
    actual = hashlib.sha256(source.read_bytes()).hexdigest()
    if actual != expected:
        raise RuntimeError(f"Display source changed: {relative}; review and refresh the display lock before building")
destination.mkdir(parents=True, exist_ok=True)
for name in ("codex_ax7z020_fb.c", "Kconfig", "Makefile"):
    shutil.copyfile(project / "hardware/display/driver" / name, destination / name)
for filename, line in (
    ("Kconfig", 'source "drivers/video/fbdev/codex_ax7z020/Kconfig"'),
    ("Makefile", 'obj-$(CONFIG_FB_CODEX_AX7Z020) += codex_ax7z020/'),
):
    path = destination.parent / filename
    text = path.read_text()
    if line not in text.splitlines():
        path.write_text(text + "\n" + line + "\n")
(kernel / "ax7z020-display-source-lock.json").write_text(json.dumps(lock, indent=2) + "\n")
print("Pinned display driver injected; compilation and board acceptance remain separate checks")
