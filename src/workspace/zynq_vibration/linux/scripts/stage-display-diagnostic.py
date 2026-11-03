#!/usr/bin/env python3
"""Stage a verified display kernel with frozen serial userspace for early diagnosis."""
import datetime
import hashlib
import json
import pathlib
import shutil
import struct
import sys

platform = pathlib.Path(__file__).resolve().parents[1]
out = pathlib.Path(sys.argv[1]).resolve()
serial = pathlib.Path(sys.argv[2]).resolve()
kernel = out / "build/linux-custom"
lock = json.loads((platform / "display-platform.lock.json").read_text())
assert json.loads((kernel / "ax7z020-display-source-lock.json").read_text()) == lock
config = (kernel / ".config").read_text().splitlines()
required = ["CONFIG_SMP=y", "CONFIG_NR_CPUS=2", "CONFIG_FB=y", "CONFIG_FB_CODEX_AX7Z020=y", "CONFIG_CMA_SIZE_MBYTES=64", "CONFIG_DMA_CMA=y", "CONFIG_FB_SYS_FILLRECT=y", "CONFIG_FB_SYS_COPYAREA=y", "CONFIG_FB_SYS_IMAGEBLIT=y", "CONFIG_FB_SYS_FOPS=y"]
for item in required:
    assert item in config, item
for item in ("CONFIG_OF_OVERLAY=y", "CONFIG_SUSPEND=y", "CONFIG_HIBERNATION=y"):
    assert item not in config, item
driver = kernel / "drivers/video/fbdev/codex_ax7z020/codex_ax7z020_fb.c"
assert hashlib.sha256(driver.read_bytes()).hexdigest() == lock["source_sha256"]["hardware/display/driver/codex_ax7z020_fb.c"]
assert b"ax7fb_probe" in (kernel / "vmlinux").read_bytes()
assert (driver.parent / "built-in.a").is_file()
dtb = (out / "images/zynq-ax7z020.dtb").read_bytes()
assert struct.unpack_from(">I", dtb)[0] == 0xd00dfeed
assert b"codex,ax7z020-vdma-fb\0" in dtb
assert struct.pack(">IIIIII", 0x43000000, 0x10000, 0x43c00000, 0x10000, 0x41200000, 0x10000) in dtb
assert struct.pack(">III", 0, 29, 4) in dtb
stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
archive = platform / "artifacts/display-diagnostic" / stamp
archive.mkdir(parents=True, exist_ok=False)
source_verification = json.loads((serial / "artifact-verification.json").read_text())
for name in ("u-boot.elf", "u-boot.bin", "rootfs.cpio.gz"):
    assert hashlib.sha256((serial / name).read_bytes()).hexdigest() == source_verification["files"][name]["sha256"]
    shutil.copyfile(serial / name, archive / name)
for name in ("zImage", "zynq-ax7z020.dtb"):
    shutil.copyfile(out / "images" / name, archive / name)
shutil.copyfile(kernel / ".config", archive / "display-kernel.config")
shutil.copyfile(platform / "display-platform.lock.json", archive / "display-platform.lock.json")
size = (archive / "rootfs.cpio.gz").stat().st_size
(archive / "ramboot-current.txt").write_text(f"setenv ramdisk_size {hex(size)}\nrun ramboot\n")
files = {name: {"bytes": (archive / name).stat().st_size, "sha256": hashlib.sha256((archive / name).read_bytes()).hexdigest()} for name in ("u-boot.elf", "u-boot.bin", "rootfs.cpio.gz", "zImage", "zynq-ax7z020.dtb")}
(archive / "SHA256SUMS").write_text("".join(f"{record['sha256']}  {name}\n" for name, record in files.items()))
record = {
    "profile": "display-kernel-with-frozen-serial-userspace-diagnostic",
    "build_checks_passed": True, "board_boot_verified": False, "display_verified": False,
    "scope": "Diagnostic combination only: linked built-in display kernel and matching DTB plus byte-identical proven serial userspace. No Qt or final display rootfs claim.",
    "serial_userspace_origin": str(serial), "display_output_origin": str(out),
    "contains_qt": False, "contains_fb_scanout_test": False,
    "known_early_devnull_issue_retained_from_serial": True,
    "ramdisk_bytes": size, "ramdisk_hex": hex(size), "files": files,
    "display_platform_lock": lock,
}
(archive / "artifact-verification.json").write_text(json.dumps(record, indent=2) + "\n")
(archive / "README.txt").write_text("DIAGNOSTIC COMBINATION, NOT THE FINAL DISPLAY/Qt IMAGE.\nUse finalC ax7z020_display_720p60.bit and its matching ps7_init.tcl.\nFrozen serial rootfs preserves its known initial /dev/null warning.\nInspect /dev/fb0 and platform driver status only; no screen/tearing/touch acceptance is implied.\n")
print(json.dumps({"archive": str(archive), "ramdisk_bytes": size, "ramdisk_hex": hex(size), "files": files}, indent=2))
