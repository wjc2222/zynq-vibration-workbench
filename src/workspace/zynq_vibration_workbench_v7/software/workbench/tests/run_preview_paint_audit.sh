#!/bin/sh
# Host-only synthetic GUI timing audit. Does not open devices or touch a board.
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
workbench=$(CDPATH= cd -- "$test_dir/.." && pwd)
project=$(CDPATH= cd -- "$workbench/../.." && pwd)
output=${1:?usage: run_preview_paint_audit.sh ABSOLUTE_EVIDENCE_DIRECTORY}
case "$output" in /*) ;; *) echo 'Evidence path must be absolute' >&2; exit 2 ;; esac
mkdir -p "$output"
build=$(mktemp -d /tmp/hdmi-v55-paint-audit.XXXXXX)
cat > "$build/audit.pro" <<EOF
QT += widgets
CONFIG += console c++17 release
CONFIG -= app_bundle
TARGET = preview_paint_audit
INCLUDEPATH += "$workbench" "$project/software/core/include"
SOURCES += "$workbench/plot_canvas.cpp" "$workbench/live_preview_model.cpp" \
    "$project/software/core/src/core.cpp" "$project/software/core/src/detection.cpp" \
    "$test_dir/preview_paint_audit.cpp"
QMAKE_LIBS += -pthread
EOF
(
    cd "$build"
    qmake audit.pro
    make -j2
) > "$output/build.log" 2>&1
for direct in 0 1; do
    for aa in 0 1 auto; do
        QT_QPA_PLATFORM=offscreen AX7_CAPTURE_PROFILE=1 AX7_LIVE_DEMAND_ANALYSIS=1 \
        AX7_PLOT_DIRECT_PAINT="$direct" AX7_PLOT_TRACE_AA="$aa" AX7_PLOT_SHARED_M4=1 AX7_PLOT_AXES_MODE=text_cache \
        "$build/preview_paint_audit" "$output/fresh-direct-$direct-aa-$aa.json" \
            > "$build/direct-$direct-aa-$aa.stdout.json" 2> "$output/direct-$direct-aa-$aa.stderr.txt"
    done
done
printf 'host build and synthetic audit finished; build intermediates: %s\n' "$build"
