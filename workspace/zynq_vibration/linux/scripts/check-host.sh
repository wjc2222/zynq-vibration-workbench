#!/usr/bin/env bash
set -euo pipefail
missing=()
for program in bash make gcc g++ git rsync patch perl python3 file cpio unzip bzip2 xz wget curl bc flex bison; do
    command -v "$program" >/dev/null 2>&1 || missing+=("$program")
done
if ((${#missing[@]})); then
    printf 'Missing host programs: %s\n' "${missing[*]}" >&2
    echo 'Run the documented Ubuntu dependency installation first.' >&2
    exit 1
fi
if [[ $(uname -m) != x86_64 ]]; then
    echo 'Pinned Bootlin prebuilt toolchain requires an x86_64 Linux build host.' >&2
    exit 1
fi
if [[ ${PATH} == *' '* ]]; then
    echo 'Buildroot rejects PATH entries containing spaces; use the clean PATH in build.sh.' >&2
    exit 1
fi
gcc --version | sed -n '1p'
make --version | sed -n '1p'
python3 --version
echo 'Host program checks passed; this is not a board acceptance test.'
