#!/bin/sh
set -eu
images=$1
board=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cp "$board/ramboot.txt" "$images/ramboot.txt"
cp "$board/sd-initramfs.cmd" "$images/sd-initramfs.cmd"
if [ -f "$images/rootfs.cpio.gz" ]; then
    bytes=$(wc -c < "$images/rootfs.cpio.gz" | tr -d ' ')
    hex=$(printf '%x' "$bytes")
    {
        echo "# Exact byte count for this rootfs; do not use a previous value."
        echo "setenv ramdisk_size 0x$hex"
        echo "run ramboot"
    } > "$images/ramboot-current.txt"
fi
cd "$images"
for artifact in zImage zynq-ax7z020.dtb rootfs.cpio.gz u-boot.elf u-boot.bin; do
    [ -f "$artifact" ] || { echo "Missing build artifact: $artifact" >&2; exit 1; }
done
sha256sum zImage zynq-ax7z020.dtb rootfs.cpio.gz u-boot.elf u-boot.bin \
    ramboot-current.txt > SHA256SUMS
echo "RAM-only artifacts built; this script has not accessed an SD device."
