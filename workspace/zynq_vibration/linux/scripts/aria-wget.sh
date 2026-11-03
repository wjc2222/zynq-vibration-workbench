#!/usr/bin/env bash
# Narrow compatibility adapter for Buildroot's wget download backend.
# Buildroot still verifies its pinned hash before accepting any downloaded file.
main() {
set -euo pipefail
original=("$@")
output=
url=
while (($#)); do
    case "$1" in
        -q|--quiet|--) shift ;;
        -O) [[ $# -ge 2 ]] || exit 2; output=$2; shift 2 ;;
        http://*|https://*|ftp://*) url=$1; shift ;;
        *) exec /usr/bin/wget -nd -t 3 --connect-timeout=10 "${original[@]}" ;;
    esac
done
if [[ -z "$output" || -z "$url" ]] || ! command -v aria2c >/dev/null; then
    exec /usr/bin/wget -nd -t 3 --connect-timeout=10 "${original[@]}"
fi
exec aria2c --no-conf=true --continue=true --max-connection-per-server=4 --split=4 \
    --min-split-size=1M --file-allocation=none --auto-file-renaming=false \
    --allow-overwrite=false --connect-timeout=15 --timeout=30 --max-tries=3 \
    --retry-wait=3 --summary-interval=30 \
    --show-console-readout=false --console-log-level=warn --enable-color=false \
    --dir="$(dirname -- "$output")" --out="$(basename -- "$output")" "$url"
}
main "$@"
