#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:?usage: build_and_test.sh OUTPUT_DIRECTORY KERNEL_BUILD_DIRECTORY ARM_CROSS_PREFIX}"
kernel="${2:?kernel build directory required}"
cross="${3:?cross prefix required (ending arm-linux-)}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
cmake -S "$root/software/accelerator" -B "$out/host" -DCMAKE_BUILD_TYPE=Release
cmake --build "$out/host" -j4
ctest --test-dir "$out/host" --output-on-failure
cmake -S "$root/software/accelerator" -B "$out/arm" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
    -DCMAKE_CXX_COMPILER="${cross}g++" -DBUILD_TESTING=ON
cmake --build "$out/arm" -j4
mkdir -p "$out/module"
cp "$root/hardware/combined/linux/accelerator/ax7z020_accel.c" "$out/module/"
cp "$root/hardware/combined/include/ax7z020_accel.h" "$out/module/"
cp "$root/hardware/combined/include/ax7z020_accel_regs.h" "$out/module/"
printf '%s\n' 'obj-m += ax7z020_accel.o' 'ccflags-y += -I$(src)' > "$out/module/Makefile"
make -C "$kernel" M="$out/module" ARCH=arm CROSS_COMPILE="$cross" modules -j4
"$out/host/vibration-accel-cli" --backend reference --synthetic --fs 50000
file "$out/arm/vibration-accel-cli" "$out/module/ax7z020_accel.ko"
sha256sum "$out/arm/vibration-accel-cli" "$out/module/ax7z020_accel.ko" > "$out/runtime.sha256"
printf 'ARM CLI: %s\nKernel module: %s\n' "$out/arm/vibration-accel-cli" "$out/module/ax7z020_accel.ko"
