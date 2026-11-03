#!/usr/bin/env python3
"""Validate generated payload structure and configuration; never opens a device."""
import gzip
import hashlib
import json
import pathlib
import posixpath
import re
import struct
import sys

out = pathlib.Path(sys.argv[1]).resolve()
profile = sys.argv[2]
images = out / "images"

def check(condition, message):
    if not condition:
        raise RuntimeError(message)

def dt_properties(blob):
    check(struct.unpack_from(">I", blob)[0] == 0xD00DFEED, "Invalid FDT magic")
    structure_offset, strings_offset = struct.unpack_from(">II", blob, 8)
    cursor, stack, props = structure_offset, [], {}
    while True:
        token = struct.unpack_from(">I", blob, cursor)[0]
        cursor += 4
        if token == 1:
            end = blob.index(b"\0", cursor)
            stack.append(blob[cursor:end].decode())
            cursor = (end + 4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name_offset = struct.unpack_from(">II", blob, cursor)
            cursor += 8
            name_start = strings_offset + name_offset
            name_end = blob.index(b"\0", name_start)
            name = blob[name_start:name_end].decode()
            props[("/" + "/".join(stack[1:]), name)] = blob[cursor:cursor + length]
            cursor = (cursor + length + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            return props
        else:
            raise RuntimeError(f"Unknown FDT token {token}")

def cpio_members(blob):
    entries, metadata, cursor = {}, {}, 0
    while cursor + 110 <= len(blob):
        header = blob[cursor:cursor + 110]
        check(header[:6] in (b"070701", b"070702"), "Unsupported initramfs CPIO format")
        fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        size, name_size = fields[6], fields[11]
        name = blob[cursor + 110:cursor + 110 + name_size - 1].decode()
        cursor = (cursor + 110 + name_size + 3) & ~3
        if name == "TRAILER!!!":
            break
        clean_name = name.removeprefix("./")
        entries[clean_name] = blob[cursor:cursor + size]
        metadata[clean_name] = {"mode": fields[1], "rdevmajor": fields[9], "rdevminor": fields[10]}
        cursor = (cursor + size + 3) & ~3
    return entries, metadata

def check_arm_hardfloat(blob, name):
    check(blob[:6] == b"\x7fELF\x01\x01", f"{name} must be little-endian ELF32")
    check(struct.unpack_from("<H", blob, 18)[0] == 40, f"{name} must target ARM")
    check(struct.unpack_from("<I", blob, 36)[0] & 0x400, f"{name} must use the hard-float ABI")

def elf_needed(blob):
    """Read ELF32 SHT_DYNAMIC without requiring a host-specific readelf."""
    section_offset = struct.unpack_from("<I", blob, 32)[0]
    section_size, section_count = struct.unpack_from("<HH", blob, 46)
    sections = [struct.unpack_from("<10I", blob, section_offset + index * section_size)
                for index in range(section_count)]
    needed = []
    for section in sections:
        if section[1] != 6:
            continue
        strings = sections[section[6]]
        table = blob[strings[4]:strings[4] + strings[5]]
        for offset in range(section[4], section[4] + section[5], section[9] or 8):
            tag, value = struct.unpack_from("<II", blob, offset)
            if tag == 1:
                needed.append(table[value:table.index(b"\0", value)].decode())
    return needed

def resolve_member(name):
    """Resolve only CPIO-contained symlinks, never a path on the host."""
    visited = set()
    while name in members:
        check(name not in visited, f"CPIO symlink loop: {name}")
        visited.add(name)
        if cpio_metadata[name]["mode"] & 0o170000 != 0o120000:
            return name
        link = members[name].decode()
        name = posixpath.normpath(link.lstrip("/") if link.startswith("/") else
                                  posixpath.join(posixpath.dirname(name), link))
        check(not name.startswith("../"), "CPIO symlink escaped the rootfs")
    raise RuntimeError(f"CPIO symlink target is missing: {name}")

required = ["zImage", "zynq-ax7z020.dtb", "rootfs.cpio.gz", "u-boot.elf", "u-boot.bin", "SHA256SUMS"]
for name in required:
    check((images / name).is_file(), f"Missing artifact: {name}")
for line in (images / "SHA256SUMS").read_text().splitlines():
    expected, name = line.split(maxsplit=1)
    path = images / name.lstrip(" *")
    check(path.parent.resolve() == images.resolve(), "Unsafe path in SHA256SUMS")
    check(hashlib.sha256(path.read_bytes()).hexdigest() == expected, f"SHA256SUMS mismatch: {path.name}")
kernel = (images / "zImage").read_bytes()
check(struct.unpack_from("<I", kernel, 0x24)[0] == 0x016F2818, "Invalid ARM zImage magic")
bootloader = (images / "u-boot.elf").read_bytes()
check(bootloader[:5] == b"\x7fELF\x01", "Expected ELF32 U-Boot")
check(struct.unpack_from("<H", bootloader, 18)[0] == 40, "U-Boot ELF is not ARM")
check(struct.unpack_from("<I", bootloader, 24)[0] == 0x04000000, "Unexpected U-Boot entry address")
check(b"ALINX AX7Z020 offline vibration PS platform" in bootloader, "Remade U-Boot ELF is missing the appended board DTB")
for entry in (b"kernel_addr_r=0x02000000\0", b"fdt_addr_r=0x01f00000\0", b"ramdisk_addr_r=0x10000000\0", b"ramboot=if test -n"):
    check(entry in bootloader, f"U-Boot RAM environment is missing {entry!r}")
props = dt_properties((images / "zynq-ax7z020.dtb").read_bytes())
check(props[("/", "model")].startswith(b"ALINX AX7Z020"), "Incorrect board model")
check(props[("/memory@0", "reg")] == struct.pack(">II", 0, 0x40000000), "DDR range must be exactly 1 GiB")
check(props[("/chosen", "stdout-path")] == b"serial0:115200n8\0", "Incorrect console")
check(props[("/aliases", "serial0")] == b"/axi/serial@e0001000\0", "Console must alias UART1")
check(props[("/axi/ethernet@e000b000/ethernet-phy@3", "reg")] == struct.pack(">I", 3), "Incorrect PHY address")
initramfs_path = images / "rootfs.cpio.gz"
check(initramfs_path.stat().st_size < 0x10000000, "Initramfs exceeds the documented RAM-load allocation")
members, cpio_metadata = cpio_members(gzip.decompress(initramfs_path.read_bytes()))
check(not any(name.startswith("usr/lib/") and re.fullmatch(r"st[A-Za-z0-9]{6}", name.rsplit("/", 1)[-1])
              for name in members), "Rootfs contains an interrupted GNU strip temporary file")
bootstrap_nodes = {}
for name, major, minor in (("null", 1, 3), ("zero", 1, 5), ("tty", 5, 0), ("random", 1, 8), ("urandom", 1, 9)):
    node = cpio_metadata.get("dev/" + name, {})
    bootstrap_nodes[name] = (node.get("mode", 0) & 0o170000) == 0o020000 and node.get("rdevmajor") == major and node.get("rdevminor") == minor
if profile in ("qt", "display"):
    check(all(bootstrap_nodes.values()), "Final rootfs must provide initial character devices before mdev runs")
for name in ("usr/bin/vibration_analyze", "usr/bin/platform-evidence", "usr/bin/mount-captures-ro", "usr/bin/run-synthetic-evidence", "usr/bin/run-speed-evidence", "sbin/init"):
    check(name in members, f"Rootfs missing {name}")
check_arm_hardfloat(members["usr/bin/vibration_analyze"], "vibration_analyze")
check("usr/lib/libvibration_core.so" in members, "Rootfs is missing the packaged analysis core library")
check_arm_hardfloat(members["usr/lib/libvibration_core.so"], "libvibration_core.so")
for name in ("usr/bin/vibration_analyze", "usr/lib/libvibration_core.so"):
    check((out / "target" / name).read_bytes() == members[name], f"Packaged application differs from target: {name}")
for marker in (b"AX7Z020_PLATFORM_EVIDENCE_BEGIN", b"AX7Z020_PLATFORM_EVIDENCE_END", b"/proc/cpuinfo"):
    check(marker in members["usr/bin/platform-evidence"], f"Rootfs platform evidence helper is stale: {marker!r}")
shadow_root = next(line for line in members["etc/shadow"].decode().splitlines() if line.startswith("root:"))
check(shadow_root.split(":")[1] == "", "Documented serial root login must have the configured empty password")
check(b"ttyPS0" in members["etc/inittab"], "Serial getty is missing")
if profile in ("qt", "display"):
    early_mount = b"::sysinit:/bin/mount -t devtmpfs devtmpfs /dev\n"
    inittab = members["etc/inittab"]
    check(inittab.count(early_mount) == 1, "Initramfs requires one explicit early devtmpfs mount")
    check(inittab.index(early_mount) < inittab.index(b"::sysinit:/bin/mkdir -p /dev/pts") < inittab.index(b"::sysinit:/etc/init.d/rcS"),
          "devtmpfs must precede devpts and the eudev startup scripts")
fixture_name = "usr/share/vibration/synthetic/synthetic_10s.BIN"
sidecar_name = "usr/share/vibration/synthetic/synthetic_10s.meta.json"
check(fixture_name in members and sidecar_name in members, "Rootfs missing synthetic sample or same-stem metadata")
fixture_metadata = json.loads(members[sidecar_name])
fixture_hash = hashlib.sha256(members[fixture_name]).hexdigest()
check(fixture_metadata.get("synthetic") is True, "Development sample must be explicitly marked synthetic")
check(fixture_metadata.get("fault_label") is None, "Synthetic sample must not carry a mechanical fault truth label")
check(len(members[fixture_name]) == 16004096, "Synthetic 10-second 50k capture has incorrect byte count")
check(fixture_metadata["sha256"] == fixture_hash, "Synthetic sample differs from its metadata hash")
speed_name = "usr/share/vibration/speed-only/CAP00003.BIN"
check(speed_name in members, "Rootfs missing approved speed-only capture")
speed_hash = hashlib.sha256(members[speed_name]).hexdigest()
check(len(members[speed_name]) == 16004096 and speed_hash == "941b1320495af0f1af7b38a5c70bd999e658c8583539c21d4c8ba402bae2f1cb", "Speed-only capture identity mismatch")
check("usr/share/vibration/speed-only/CAP00003.meta.json" not in members, "Do not invent a synthetic sidecar for the real speed-only capture")
if profile in ("qt", "display"):
    check("usr/bin/vibration_gui" in members, "Qt profile missing GUI executable")
    check_arm_hardfloat(members["usr/bin/vibration_gui"], "vibration_gui")
    check((out / "target/usr/bin/vibration_gui").read_bytes() == members["usr/bin/vibration_gui"],
          "Packaged GUI differs from the final target executable")
    check("libvibration_core.so" in elf_needed(members["usr/bin/vibration_gui"]),
          "GUI must use the same packaged analysis core library as the CLI")
    check(any("wqy" in n.lower() or "zenhei" in n.lower() for n in members), "Qt profile missing its Chinese font")
    check(members.get("usr/bin/samples") == b"/usr/share/vibration/synthetic", "GUI demo entry must resolve to the synthetic-only folder")
    check((cpio_metadata["usr/bin/samples"]["mode"] & 0o170000) == 0o120000, "GUI demo entry must be a symbolic link")
    for name in (fixture_name, sidecar_name):
        check((cpio_metadata[name]["mode"] & 0o222) == 0, "Bundled demo inputs must be marked read-only")
linux_config = next((out / "build").glob("linux-*/.config")).read_text().splitlines()
for setting in ("CONFIG_SMP=y", "CONFIG_NR_CPUS=2", "CONFIG_SERIAL_XILINX_PS_UART_CONSOLE=y", "CONFIG_HID_MULTITOUCH=y", "CONFIG_MICREL_PHY=y", "CONFIG_USB_PHY=y", "CONFIG_USB_ULPI=y", "CONFIG_USB_CHIPIDEA_GENERIC=y", "CONFIG_USB_CHIPIDEA_HOST=y", "CONFIG_NLS_UTF8=y"):
    check(setting in linux_config, f"Kernel Kconfig dropped {setting}")
linux_tree = next((out / "build").glob("linux-*/.config")).parent
check(b"ax7z020_ulpi_probe" in (linux_tree / "vmlinux").read_bytes(), "Board ULPI platform glue is missing from linked kernel")
display_lock = None
qpa_verification = None
if profile == "display":
    check("usr/bin/vibration_touch_test" not in members, "Obsolete virtual-touch prototype must not ship")
    check("usr/bin/fb_scanout_test" in members, "Display rootfs is missing the explicit scanout test tool")
    check_arm_hardfloat(members["usr/bin/fb_scanout_test"], "fb_scanout_test")
    check("usr/bin/uinput_touch_probe" in members, "Display rootfs is missing its explicitly virtual touch QA tool")
    check_arm_hardfloat(members["usr/bin/uinput_touch_probe"], "uinput_touch_probe")
    check("etc/init.d/S10udev" in members and "etc/init.d/S10mdev" not in members, "Final display must start eudev only")
    udev_libraries = [data for name, data in members.items() if "libudev.so." in name and data.startswith(b"\x7fELF")]
    check(udev_libraries, "Final display is missing its libudev runtime library")
    for library in udev_libraries:
        check_arm_hardfloat(library, "libudev")
    check(any(name.endswith("/libqlinuxfb.so") for name in members), "Qt linuxfb platform plugin is missing")
    plugin_name = "usr/lib/qt/plugins/platforms/libqax7zfb.so"
    check(plugin_name in members, "Display profile is missing the explicitly selected ax7zfb QPA plugin")
    plugin = members[plugin_name]
    check_arm_hardfloat(plugin, "libqax7zfb.so")
    check(struct.unpack_from("<H", plugin, 16)[0] == 3, "ax7zfb must be a shared object")
    check(b"ax7zfb" in plugin and b"refusing fallback" in plugin,
          "ax7zfb is missing the explicit backend/failure policy")
    check((out / "target" / plugin_name).read_bytes() == plugin,
          "Packaged ax7zfb differs from the final stripped target plugin")
    dependencies = {}
    needed = elf_needed(plugin)
    check({"libQt5Gui.so.5", "libQt5Core.so.5", "libudev.so.1", "libc.so.6"}.issubset(needed),
          "ax7zfb lost its required Qt/input/runtime dependencies")
    dependency_queue = list(needed)
    while dependency_queue:
        library = dependency_queue.pop(0)
        if library in dependencies:
            continue
        candidates = [name for name in ("usr/lib/" + library, "lib/" + library) if name in members]
        check(candidates, f"ax7zfb runtime dependency is absent: {library}")
        resolved = resolve_member(candidates[0])
        check_arm_hardfloat(members[resolved], library)
        transitive = elf_needed(members[resolved])
        dependencies[library] = {"resolved": resolved, "needed": transitive,
                                 "sha256": hashlib.sha256(members[resolved]).hexdigest()}
        dependency_queue.extend(transitive)
    plugin_source = pathlib.Path(__file__).resolve().parents[1] / "qt-ax7zfb"
    plugin_build = next((out / "build").glob("qt-ax7zfb-*/prepared"))
    source_manifest = json.loads(members["usr/share/qt-ax7zfb/source-manifest.json"])
    check(source_manifest == json.loads((plugin_build / "source-manifest.json").read_text()),
          "Packaged ax7zfb source manifest differs from the built manifest")
    check(source_manifest["plugin"] == "ax7zfb" and source_manifest["qt_version"] == "5.15.18",
          "ax7zfb source manifest has the wrong plugin or Qt version")
    for relative, expected in source_manifest["own_source_sha256"].items():
        for root in (plugin_source, plugin_build.parent):
            check(hashlib.sha256((root / relative).read_bytes()).hexdigest() == expected,
                  f"ax7zfb own source is stale: {root / relative}")
    for relative, expected in source_manifest["generated_sha256"].items():
        check(hashlib.sha256((plugin_build / relative).read_bytes()).hexdigest() == expected,
              f"ax7zfb generated source differs: {relative}")
    qt_source = next((out / "build").glob("qt5base-*/src/plugins/platforms/linuxfb"))
    for relative, expected in source_manifest["upstream_linuxfb_sha256"].items():
        check(hashlib.sha256((qt_source / relative).read_bytes()).hexdigest() == expected,
              f"ax7zfb Qt integration source differs: {relative}")
    qpa_verification = {"path": plugin_name, "sha256": hashlib.sha256(plugin).hexdigest(),
                        "direct_needed": needed, "dependencies": dependencies, "source_manifest": source_manifest,
                        "explicit_platform": "ax7zfb:fb=/dev/fb0", "manual_rollback": "linuxfb"}
    qt_udev_consumers = [name for name, data in members.items()
                         if ("libQt5" in name or "/libq" in name) and
                         data.startswith(b"\x7fELF") and b"udev_monitor_new_from_netlink" in data]
    check(qt_udev_consumers, "Qt input discovery was not linked to the libudev monitor")
    board = pathlib.Path(__file__).resolve().parents[1] / "br2-external/board/ax7z020"
    for name, source in {
        "etc/init.d/S90vibration-gui": board / "display-service/S90vibration-gui",
        "etc/default/vibration-gui": board / "display-service/vibration-gui.default",
        "etc/udev/rules.d/10-vibration-storage.rules": board / "display-service/10-vibration-storage.rules",
        "usr/bin/vibration-gui": board / "rootfs-overlay/usr/bin/vibration-gui",
        "usr/bin/mount-captures-ro": board / "rootfs-overlay/usr/bin/mount-captures-ro",
    }.items():
        check(members.get(name) == source.read_bytes(), f"Packaged startup/helper source is stale: {name}")
    for setting in ("CONFIG_FB=y", "CONFIG_FB_CODEX_AX7Z020=y", "CONFIG_CMA=y", "CONFIG_DMA_CMA=y", "CONFIG_CMA_SIZE_MBYTES=64", "CONFIG_FB_SYS_FILLRECT=y", "CONFIG_FB_SYS_COPYAREA=y", "CONFIG_FB_SYS_IMAGEBLIT=y", "CONFIG_FB_SYS_FOPS=y", "CONFIG_INPUT_UINPUT=y"):
        check(setting in linux_config, f"Display kernel Kconfig dropped {setting}")
    for setting in ("CONFIG_OF_OVERLAY=y", "CONFIG_SUSPEND=y", "CONFIG_HIBERNATION=y"):
        check(setting not in linux_config, f"Fixed display platform forbids {setting}")
    node = "/axi/framebuffer@43000000"
    check(props[(node, "compatible")] == b"codex,ax7z020-vdma-fb\0", "Incorrect display driver binding")
    check(props[(node, "reg")] == struct.pack(">IIIIII", 0x43000000, 0x10000, 0x43c00000, 0x10000, 0x41200000, 0x10000), "Display registers differ from the implemented hardware")
    check(props[(node, "interrupts")] == struct.pack(">III", 0, 29, 4), "VDMA interrupt must use SPI29, absolute GIC ID61")
    display_lock = json.loads((linux_tree / "ax7z020-display-source-lock.json").read_text())
    expected_lock = json.loads((pathlib.Path(__file__).resolve().parents[1] / "display-platform.lock.json").read_text())
    check(display_lock == expected_lock, "Display kernel used a stale source lock; run display build")
    check(b"ax7fb_probe" in (linux_tree / "vmlinux").read_bytes(), "Display driver is absent from linked vmlinux")
    check((linux_tree / "drivers/video/fbdev/codex_ax7z020/built-in.a").is_file(), "Display driver was not built into the kernel")
uboot_config = next((out / "build").glob("uboot-*/.config")).read_text().splitlines()
check("CONFIG_SPL=y" not in uboot_config, "SPL unexpectedly enabled")
for setting in ("CONFIG_ENV_IS_NOWHERE=y", "CONFIG_OF_SEPARATE=y", "CONFIG_BOOTDELAY=-1", 'CONFIG_ENV_SOURCE_FILE="ax7z020"', 'CONFIG_SYS_CONFIG_NAME="zynq-ax7z020"'):
    check(setting in uboot_config, f"U-Boot Kconfig dropped {setting}")
for setting in ("CONFIG_ENV_IS_IN_FAT=y", "CONFIG_ENV_IS_IN_NAND=y", "CONFIG_ENV_IS_IN_SPI_FLASH=y"):
    check(setting not in uboot_config, f"Persistent environment unexpectedly enabled: {setting}")
for setting in ("CONFIG_CMD_UBI=y", "CONFIG_CMD_UBIFS=y", "CONFIG_MTD=y", "CONFIG_CMD_NAND=y", "CONFIG_CMD_SF=y", "CONFIG_USE_DEFAULT_ENV_FILE=y"):
    check(setting not in uboot_config, f"Unexpected flash or legacy environment option: {setting}")
busybox_config = next((out / "build").glob("busybox-*/.config")).read_text().splitlines()
for setting in ("CONFIG_BLOCKDEV=y", "CONFIG_TIME=y"):
    check(setting in busybox_config, f"BusyBox Kconfig dropped {setting}")
if profile in ("qt", "display"):
    for setting in ("CONFIG_BLKID=y", "CONFIG_FEATURE_BLKID_TYPE=y"):
        check(setting in busybox_config, f"Filesystem-specific read-only mount requires {setting}")
    helper = members["usr/bin/mount-captures-ro"]
    check(b"-o ro,nosuid,nodev,noexec,utf8=1" in helper and b"noexec,iocharset=utf8" not in helper,
          "VFAT must enable utf8 translation without replacing its FAT case table")
    for setting in ('CONFIG_FAT_DEFAULT_IOCHARSET="iso8859-1"', "CONFIG_NLS_ISO8859_1=y"):
        check(setting in linux_config, f"VFAT default case table requires {setting}")
if profile == "display":
    for setting in ("CONFIG_START_STOP_DAEMON=y", "CONFIG_FEATURE_START_STOP_DAEMON_FANCY=y"):
        check(setting in busybox_config, f"GUI service requires {setting}")
project = pathlib.Path(__file__).resolve().parents[2]
app_source = next((out / "build").glob("vibration-offline-*"))
# The package's local-site rsync permits exactly CMakeLists.txt/software/**.
source_paths = [project / "CMakeLists.txt"] + [path for path in (project / "software").rglob("*") if path.is_file()]
source_hashes = {}
for source in sorted(source_paths):
    relative = source.relative_to(project)
    built_source = app_source / relative
    current_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    check(built_source.is_file(), f"Package source missing: {relative}")
    check(hashlib.sha256(built_source.read_bytes()).hexdigest() == current_hash,
          f"Package source is stale: {relative}; run app-rebuild")
    source_hashes[relative.as_posix()] = current_hash
display_tool_sources = {}
if profile == "display":
    tool_build = next((out / "build").glob("ax7z020-display-tools-*"))
    for name in ("fb_scanout_test.c", "uinput_touch_probe.c"):
        source = project / "hardware/display/tools" / name
        built = tool_build / name
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        check(built.is_file() and hashlib.sha256(built.read_bytes()).hexdigest() == digest,
              f"Display tool source is stale: {name}; run display app-rebuild")
        display_tool_sources[name] = digest
manifest = {
    "profile": profile, "build_checks_passed": True,
    "board_boot_verified": False, "display_verified": False,
    "scope": "Generated image structure, required contents and config only; no board or SD access",
    "application_source_sha256": source_hashes,
    "display_tool_source_sha256": display_tool_sources,
    "synthetic_fixture_sha256": fixture_hash,
    "real_speed_only_fixture_sha256": speed_hash,
    "initial_character_devices_verified": bootstrap_nodes,
    "display_platform_source_lock": display_lock,
    "qt_udev_consumers": qt_udev_consumers if profile == "display" else [],
    "qt_ax7zfb": qpa_verification,
    "application_elf_sha256": {name: hashlib.sha256(members[name]).hexdigest()
                              for name in ("usr/bin/vibration_analyze", "usr/bin/vibration_gui", "usr/lib/libvibration_core.so") if name in members},
    "files": {name: {"bytes": (images / name).stat().st_size,
                     "sha256": hashlib.sha256((images / name).read_bytes()).hexdigest()}
              for name in required},
}
(images / "artifact-verification.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
