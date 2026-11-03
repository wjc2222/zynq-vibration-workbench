#!/usr/bin/env bash
set -euo pipefail
for tool in cmake pkg-config python3 xvfb-run Xvfb xauth realpath; do
  command -v "$tool" >/dev/null || { echo "$tool is required for the full Qt5 host build and tests" >&2; exit 2; }
done
pkg-config --exists Qt5Core Qt5Gui Qt5Widgets || { echo 'Qt5 Core/Gui/Widgets development files are required for the workbench' >&2; exit 2; }
python3 -c 'import sys; assert sys.version_info >= (3, 10); import numpy' >/dev/null || { echo 'Python 3.10+ with NumPy is required for the host test suite' >&2; exit 2; }
export LC_ALL=C.UTF-8
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:?Usage: build-host.sh NEW_ASCII_BUILD}
[[ "$out" = /* && "$out" != *[!\ -~]* ]] || { echo 'Use a new absolute ASCII build path outside the source tree' >&2; exit 2; }
out=$(realpath -m -- "$out")
[[ ! -e "$out" && "$out" != "$root" && "$out" != "$root/"* ]] || { echo 'Use a new absolute ASCII build path outside the source tree' >&2; exit 2; }
cmake -S "$root/src/workspace/zynq_vibration_workbench_v7" -B "$out" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DVIBRATION_BUILD_GUI=ON -DWORKBENCH_TEST_PLATFORM=xcb -DPython3_EXECUTABLE="$(command -v python3)"
cmake --build "$out" --parallel "${V7_JOBS:-4}"
test -x "$out/software/workbench/vibration_workbench" || { echo 'Primary Qt workbench was not built' >&2; exit 2; }
mkdir -m 700 "$out/qt-runtime"
XDG_RUNTIME_DIR="$out/qt-runtime" QT_QPA_PLATFORM=xcb xvfb-run -a -s '-screen 0 1920x1080x24' ctest --test-dir "$out" --output-on-failure
