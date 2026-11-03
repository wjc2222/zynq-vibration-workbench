#!/usr/bin/env bash
# Optional range downloader for slow source servers. No proxy or TLS override.
main() {
set -euo pipefail
component=${1:?Specify linux, uboot or toolchain}
case "$component" in linux|linux-upstream|uboot|toolchain) ;; *) exit 2 ;; esac
platform=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace=${AX7Z020_BUILD_ROOT:-"$HOME/ax7z020-build"}
command -v aria2c >/dev/null || { echo 'Optional aria2 package is required for this helper' >&2; exit 1; }
mapfile -t pin < <(python3 - "$platform/versions.lock.json" "$component" <<'PY'
import json, sys
all_pins = json.load(open(sys.argv[1]))
p = all_pins["linux_upstream_candidate" if sys.argv[2] == "linux-upstream" else sys.argv[2]]
if sys.argv[2] == "linux-upstream" or (sys.argv[2] == "linux" and "tag" not in p):
    archive = p["archive"]
    url = p["mirror_url"]
    digest = p["sha256"]
    package = "linux"
elif sys.argv[2] == "toolchain":
    archive = p["variant"] + ".tar.xz"
    url = "https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/" + archive
    digest = p["archive_sha256"]
    package = "toolchain-external-bootlin"
else:
    archive = p["archive"]
    repository = "linux-xlnx" if sys.argv[2] == "linux" else "u-boot-xlnx"
    url = "https://github.com/Xilinx/" + repository + "/archive/refs/tags/" + p["tag"] + ".tar.gz"
    digest = p["sha256"]
    package = sys.argv[2]
print(archive); print(url); print(digest); print(package)
PY
)
archive=${pin[0]}
url=${pin[1]}
expected=${pin[2]}
package=${pin[3]}
[[ "$archive" != */* && "$expected" =~ ^[a-f0-9]{64}$ ]] || exit 1
staging="$workspace/staged-sources"
cache="$workspace/downloads/$package"
mkdir -p "$staging" "$cache" "$workspace/logs"
exec > >(tee "$workspace/logs/fetch-$component-$(date -u +%Y%m%dT%H%M%SZ).log") 2>&1
exec 9>"$cache/.lock"
flock -x 9
if [[ -f "$cache/$archive" ]] && [[ $(sha256sum "$cache/$archive" | cut -d' ' -f1) == "$expected" ]]; then
    echo "Pinned cache already verified: $cache/$archive"
    exit 0
fi
echo "Fetching official pinned archive: $url"
if [[ -f "$staging/$archive" ]] && [[ $(sha256sum "$staging/$archive" | cut -d' ' -f1) == "$expected" ]]; then
    echo 'Complete staged archive already matches the official SHA256.'
else
aria2c --continue=true --max-connection-per-server=4 --split=4 --min-split-size=1M \
    --file-allocation=none --auto-file-renaming=false --allow-overwrite=false \
    --check-integrity=true --checksum="sha-256=$expected" --connect-timeout=15 \
    --timeout=30 --max-tries=5 --retry-wait=3 --summary-interval=30 \
    --console-log-level=warn --dir="$staging" --out="$archive" "$url"
fi
[[ $(sha256sum "$staging/$archive" | cut -d' ' -f1) == "$expected" ]] || exit 1
echo "Official SHA256 verified. Importing under the held Buildroot cache lock."
if [[ ! -f "$cache/$archive" ]] || [[ $(sha256sum "$cache/$archive" | cut -d' ' -f1) != "$expected" ]]; then
    cp "$staging/$archive" "$cache/$archive.importing"
    [[ $(sha256sum "$cache/$archive.importing" | cut -d' ' -f1) == "$expected" ]] || exit 1
    mv "$cache/$archive.importing" "$cache/$archive"
fi
echo "Verified cache imported: $cache/$archive"
# This is a task-generated staging copy; the verified cache remains available.
rm -- "$staging/$archive"
}
main "$@"
