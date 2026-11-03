#!/usr/bin/env bash
set -euo pipefail
command -v pkg-config >/dev/null || { echo 'pkg-config is required for Qt5 preflight' >&2; exit 2; }
pkg-config --exists Qt5Core Qt5Widgets || { echo 'Qt5 Core/Widgets development files are required for the workbench' >&2; exit 2; }
export LC_ALL=C.UTF-8
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?Usage: build-host.sh NEW_ASCII_BUILD}
[[ "$out" = /* && "$out" != *[![:print:]]* && ! -e "$out" ]] || { echo 'Use a new absolute ASCII build path' >&2; exit 2; }
cmake -S "$root/workspace/zynq_vibration_workbench_v7" -B "$out" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DVIBRATION_BUILD_GUI=ON
cmake --build "$out" --parallel "${V7_JOBS:-4}"
test -x "$out/software/workbench/vibration_workbench" || { echo 'Primary Qt workbench was not built' >&2; exit 2; }
if command -v xvfb-run >/dev/null; then
  QT_QPA_PLATFORM=xcb xvfb-run -a ctest --test-dir "$out" --output-on-failure
else
  ctest --test-dir "$out" --output-on-failure
fi
