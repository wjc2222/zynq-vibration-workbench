#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 ]]; then echo "usage: $0 BUILD_DIR EVIDENCE_DIR" >&2; exit 2; fi
build=$1
evidence=$2
source_root=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$evidence"
cmake -S "$source_root" -B "$build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release -DWORKBENCH_TEST_PLATFORM=xcb > "$evidence/configure.log" 2>&1
cmake --build "$build" -j4 > "$evidence/build.log" 2>&1
export QT_QPA_PLATFORM=xcb
export XDG_CONFIG_HOME="$build/compact-qa-config"
export XDG_RUNTIME_DIR="$build/qt-runtime"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
xvfb-run -a -s '-screen 0 1400x900x24' "$build/software/workbench/vibration_workbench_compact_layout_probe" \
    "$build/software/streaming/vibration_stream_service" "$evidence" > "$evidence/report.json" 2> "$evidence/stderr.log"
xvfb-run -a -s '-screen 0 1400x900x24' ctest --test-dir "$build" --output-on-failure -j1 > "$evidence/ctest.log" 2>&1
