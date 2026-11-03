#!/bin/sh
set -eu
target=$1
board=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project=$(CDPATH= cd -- "$board/../../../.." && pwd)
# DEVTMPFS_MOUNT does not mount /dev for an initramfs root. Do this before
# mount -a creates devpts and before eudev/getty need kernel device nodes.
python3 - "$target/etc/inittab" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
anchor = "::sysinit:/bin/mount -o remount,rw /\n"
entry = "::sysinit:/bin/mount -t devtmpfs devtmpfs /dev\n"
if entry not in text:
    if text.count(anchor) != 1:
        raise RuntimeError("Cannot locate unique early initramfs mount point")
    path.write_text(text.replace(anchor, anchor + entry))
PY
mkdir -p "$target/mnt/captures" "$target/usr/share/ax7z020"
chmod 0755 "$target/usr/bin/mount-captures-ro" "$target/usr/bin/vibration-gui" "$target/usr/bin/platform-evidence" "$target/usr/bin/run-synthetic-evidence" "$target/usr/bin/run-speed-evidence"
if [ -n "${BR2_CONFIG:-}" ] && grep -q '^BR2_AX7Z020_DISPLAY_PLATFORM=y$' "$BR2_CONFIG"; then
    echo 'AX7Z020 fixed 720p display driver built in; matching display bit/XSA/ps7_init is required.' > "$target/usr/share/ax7z020/PLATFORM_STATUS"
    echo 'S90 tries only configured SD p1 read-only, then starts fullscreen GUI once; failures are logged in RAM.' >> "$target/usr/share/ax7z020/PLATFORM_STATUS"
    cp "$project/linux/display-platform.lock.json" "$target/usr/share/ax7z020/display-platform.lock.json"
    install -D -m 0755 "$board/display-service/S90vibration-gui" "$target/etc/init.d/S90vibration-gui"
    install -D -m 0644 "$board/display-service/vibration-gui.default" "$target/etc/default/vibration-gui"
    install -D -m 0644 "$board/display-service/10-vibration-storage.rules" "$target/etc/udev/rules.d/10-vibration-storage.rules"
else
    echo 'AX7Z020 PS-only initramfs. No framebuffer hardware or driver is supplied by this profile.' > "$target/usr/share/ax7z020/PLATFORM_STATUS"
    echo 'No automatic capture mount or GUI startup in this profile.' >> "$target/usr/share/ax7z020/PLATFORM_STATUS"
fi
cat >> "$target/usr/share/ax7z020/PLATFORM_STATUS" <<'EOF'
No acquisition driver is supplied.
Capture media is mounted only read-only using mount-captures-ro; USB mounts remain manual.
No auto-format, fsck or persistent boot environment is supplied.
Serial console: UART1 on MIO48/49, ttyPS0 at 115200 8N1.
Build-time configuration does not establish successful operation on the board.
EOF
python3 "$project/linux/scripts/stage-synthetic-fixture.py" \
    "$project" "$target/usr/share/vibration/synthetic" "$BUILD_DIR"
python3 "$project/linux/scripts/stage-speed-fixture.py" \
    "$project" "$target/usr/share/vibration/speed-only"
# The frozen GUI searches appDir/samples first; keep BIN and its metadata together.
demo_link="$target/usr/bin/samples"
if [ -L "$demo_link" ] && [ "$(readlink "$demo_link")" = /usr/share/vibration/synthetic ]; then
    :
elif [ ! -e "$demo_link" ] && [ ! -L "$demo_link" ]; then
    ln -s /usr/share/vibration/synthetic "$demo_link"
else
    echo "Unexpected GUI sample entry: $demo_link" >&2
    exit 1
fi
