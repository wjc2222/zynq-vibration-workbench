#!/usr/bin/env bash
main() {
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C.UTF-8 TZ=UTC SOURCE_DATE_EPOCH=1792627200
project=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
workspace=${AX7Z020_BUILD_ROOT:-"$HOME/ax7z020-build"}
base="$workspace/output-combined-mrg1"
sdk="$workspace/output-display-udev"
out=${AX7Z020_SG_OUTPUT:-"$workspace/output-combined-sg-preview-r1"}
out=$(realpath -m -- "$out")
case "$out" in "$workspace"/output-combined-sg-*) ;; *) echo 'Use a NEW output-combined-sg-* directory' >&2; exit 2;; esac
kernel="$out/build/linux-custom"
mkdir -p "$out/build" "$out/images" "$out/logs"
exec > >(tee "$out/logs/kernel-build-$(date -u +%Y%m%dT%H%M%SZ).log") 2>&1
if [[ ! -d "$kernel" ]]; then
    cp -a --reflink=auto "$base/build/linux-custom" "$kernel"
    python3 -B - "$base" "$out" <<'PY'
import hashlib,json,pathlib,sys
base,out=map(pathlib.Path,sys.argv[1:])
paths=[base/"images/zImage",base/"build/linux-custom/.config",base/"build/linux-custom/ax7z020-combined-source-lock.json"]
(out/"base-provenance.json").write_text(json.dumps({"base":str(base),"derivation":"independent SG kernel; old outputs read-only","source_sha256":{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}},indent=2)+"\n")
PY
fi
python3 -B "$project/linux/combined/scripts/prepare-kernel-sg.py" "$kernel" > "$out/logs/kernel-source-lock.json"
cross="$sdk/host/bin/arm-linux-"
(cd "$kernel" && ARCH=arm CROSS_COMPILE="$cross" scripts/kconfig/merge_config.sh -m .config "$project/hardware/combined/linux/sg_kernel.fragment")
make -C "$kernel" ARCH=arm CROSS_COMPILE="$cross" olddefconfig
make -C "$kernel" ARCH=arm CROSS_COMPILE="$cross" -j"${AX7Z020_JOBS:-4}" zImage zynq-ax7z020-combined-sg.dtb
install -m 0644 "$kernel/arch/arm/boot/zImage" "$out/images/zImage"
install -m 0644 "$kernel/arch/arm/boot/dts/zynq-ax7z020-combined-sg.dtb" "$out/images/zynq-ax7z020-combined-sg.dtb"
python3 -B "$project/linux/combined/scripts/verify-kernel-sg.py" "$out"
}
main "$@"
