#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C.UTF-8
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?Usage: build-arm.sh NEW_ASCII_BUILD SDK_HOST}
sdk=$(realpath -- "${2:?Buildroot SDK host path required}")
[[ "$out" = /* && "$out" != *[![:print:]]* && ! -e "$out" ]] || { echo 'Use a new absolute ASCII build path' >&2; exit 2; }
toolchain="$sdk/share/buildroot/toolchainfile.cmake"
test -f "$toolchain"
cmake_bin="$sdk/bin/cmake"
[[ -x "$cmake_bin" ]] || cmake_bin=cmake
"$cmake_bin" -S "$root/workspace/zynq_vibration_workbench_v7" -B "$out" -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DVIBRATION_BUILD_GUI=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
"$cmake_bin" --build "$out" --parallel "${V7_JOBS:-4}"
test -x "$out/software/workbench/vibration_workbench" || { echo 'Primary Qt5 ARM workbench was not built; check SDK Qt5' >&2; exit 2; }
find "$out" -type f -executable -exec file {} +
