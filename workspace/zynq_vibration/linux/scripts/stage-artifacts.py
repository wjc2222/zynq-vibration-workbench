#!/usr/bin/env python3
"""Archive an already verified RAM-boot payload without opening any device."""
import argparse
import datetime
import hashlib
import json
import pathlib
import shutil
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=pathlib.Path, help="Buildroot output directory")
parser.add_argument("profile", choices=("serial", "qt", "display"))
parser.add_argument("--candidate", action="store_true", help="Use a separate candidate archive and pointer")
parser.add_argument("--with-source", action="store_true", help="Snapshot the application allowlist and small QPA/integration directories")
args = parser.parse_args()
platform = pathlib.Path(__file__).resolve().parents[1]
out = args.output.resolve()
subprocess.run(["python3", str(platform / "scripts/verify-artifacts.py"), str(out), args.profile], check=True)
images = out / "images"
verification = json.loads((images / "artifact-verification.json").read_text())
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
channel = args.profile + ("-candidate" if args.candidate else "")
archive = platform / "artifacts" / channel / stamp
archive.mkdir(parents=True, exist_ok=False)
for name, expected in verification["files"].items():
    shutil.copyfile(images / name, archive / name)
    actual = hashlib.sha256((archive / name).read_bytes()).hexdigest()
    if actual != expected["sha256"]:
        raise RuntimeError(f"Archived image hash mismatch: {name}")
for name in ("artifact-verification.json", "ramboot-current.txt", "ramboot.txt", "sd-initramfs.cmd"):
    shutil.copyfile(images / name, archive / name)
configs = archive / "configs"
configs.mkdir()
shutil.copyfile(out / ".config", configs / "buildroot.config")
for component in ("linux", "uboot", "busybox"):
    source = next((out / "build").glob(component + "-*/.config"))
    shutil.copyfile(source, configs / (component + ".config"))
shutil.copyfile(platform / "versions.lock.json", archive / "versions.lock.json")
if args.profile == "display":
    shutil.copyfile(platform / "display-platform.lock.json", archive / "display-platform.lock.json")
if args.with_source:
    source = archive / "source"
    source.mkdir()
    subprocess.run(["python3", str(platform / "scripts/archive-app-source.py"), str(platform.parent), str(source)], check=True)
    app_manifest = json.loads((source / "vibration-offline-source-manifest.json").read_text())
    if {name: record["sha256"] for name, record in app_manifest["files"].items()} != verification["application_source_sha256"]:
        raise RuntimeError("Application source changed between build verification and snapshot")
    # Explicit small source roots only: never walk/archive the project root.
    roots = [platform / "scripts", platform / "br2-external"]
    if args.profile == "display":
        roots.append(platform / "qt-ax7zfb")
    integration_files = [path for root in roots for path in root.rglob("*") if path.is_file()]
    integration_files += [platform / "versions.lock.json", platform / "display-platform.lock.json", platform / "README.md"]
    with tarfile.open(source / "linux-integration-source.tar.gz", "w:gz") as tar:
        for path in sorted(integration_files):
            if path.is_symlink():
                raise RuntimeError(f"Source snapshot refuses symlinks: {path}")
            tar.add(path, arcname=path.relative_to(platform).as_posix(), recursive=False)
    (source / "linux-integration-source-manifest.json").write_text(json.dumps({
        "scope": "linux/scripts, linux/br2-external, linux/qt-ax7zfb, README and lock files only",
        "files": {path.relative_to(platform).as_posix(): {"bytes": path.stat().st_size,
                   "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in sorted(integration_files)},
    }, indent=2) + "\n")
    if args.profile == "display":
        prepared = next((out / "build").glob("qt-ax7zfb-*/prepared"))
        # Include the exact generated Qt integration and copied license texts,
        # not qmake objects, Makefiles or the complete Qt build directory.
        prepared_files = [path for path in prepared.rglob("*") if path.is_file() and
                          (path.suffix in (".cpp", ".h", ".pro", ".json") or
                           path.name.startswith("LICENSE.") or path.name == "LGPL_EXCEPTION.txt")]
        with tarfile.open(source / "qt-ax7zfb-prepared-source.tar.gz", "w:gz") as tar:
            for path in sorted(prepared_files):
                tar.add(path, arcname=path.relative_to(prepared).as_posix(), recursive=False)
ramdisk_bytes = (archive / "rootfs.cpio.gz").stat().st_size
manifest = {
    "profile": args.profile, "channel": channel, "created_utc": stamp, "buildroot_output": str(out),
    "scope": "Verified build payload archived for RAM-only JTAG boot; no board, SD or Flash access performed",
    "board_boot_verified_by_this_step": False,
    "application_elf_sha256": verification["application_elf_sha256"],
    "qt_ax7zfb_sha256": (verification.get("qt_ax7zfb") or {}).get("sha256"),
    "root_login": "root", "root_password": "empty", "uart": "ttyPS0 115200 8N1",
    "ramdisk_bytes": ramdisk_bytes, "ramdisk_hex": hex(ramdisk_bytes),
    "files": {},
}
for path in sorted(archive.rglob("*")):
    if path.is_file():
        manifest["files"][path.relative_to(archive).as_posix()] = {
            "bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        }
(archive / "stage-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(archive.parent / "current.json").write_text(json.dumps({"archive": str(archive), "stage_manifest": str(archive / "stage-manifest.json"), "ramdisk_hex": hex(ramdisk_bytes)}, indent=2) + "\n")
print(json.dumps({"archive": str(archive), "ramdisk_bytes": ramdisk_bytes, "ramdisk_hex": hex(ramdisk_bytes)}, indent=2))
