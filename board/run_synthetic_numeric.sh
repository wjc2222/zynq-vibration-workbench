#!/bin/sh
# Source-only numerical runner. Does not flash or replace boot files.
set -eu
if [ "$#" -ne 3 ] || [ "$1" != "ax7z020-v7-ps-axil-50k-reviewed" ]; then
  echo 'Usage: sh run_synthetic_numeric.sh ax7z020-v7-ps-axil-50k-reviewed /absolute/vibration-accel-cli /absolute/synthetic_numeric_multisine_50k_s16le3.bin' >&2
  echo 'Before selecting this profile, verify the target has the matching V7 PL, driver and AXI-Lite address map.' >&2
  exit 2
fi
cli=$2
fixture=$3
case "$cli:$fixture" in /*:/*) ;; *) echo 'CLI and fixture paths must be absolute' >&2; exit 2;; esac
[ -x "$cli" ] && [ -f "$fixture" ] || { echo 'Missing CLI or fixture' >&2; exit 2; }
expected=02fe2c7c661375e46de4adc766525950116c97bbec1effa0b14ce133527b002a
actual=$(sha256sum "$fixture")
[ "${actual%% *}" = "$expected" ] || { echo 'Synthetic fixture SHA256 mismatch' >&2; exit 2; }
exec "$cli" --backend hardware --fs 50000 --raw "$fixture" --rotation-hz 25 --compare
