#!/usr/bin/env bash
# Parse the whole operation before starting it, so edits to this source file
# cannot change a long-running shell's unread tail.
main() {
set -euo pipefail
profile=${1:-serial}
action=${2:-configure}
case "$profile" in serial|qt|display) ;; *) echo 'Profile must be serial, qt or display' >&2; exit 2 ;; esac
case "$action" in configure|source|kernel-extract|kernel-build|build|app-rebuild|legal-info|verify) ;; *) echo 'Action must be configure, source, kernel-extract, kernel-build, build, app-rebuild, legal-info or verify' >&2; exit 2 ;; esac
platform=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
external="$platform/br2-external"
# WSL's inherited Windows PATH contains Program Files and breaks Buildroot.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C.UTF-8
export TZ=UTC
export SOURCE_DATE_EPOCH=1792627200
# shellcheck source=versions.env
source "$platform/scripts/versions.env"
workspace=${AX7Z020_BUILD_ROOT:-"$HOME/ax7z020-build"}
mkdir -p "$workspace"
workspace=$(CDPATH= cd -- "$workspace" && pwd)
case "$workspace" in /mnt/*) echo 'Build on the WSL ext4 filesystem, not /mnt Windows storage.' >&2; exit 1 ;; esac
[[ "$workspace" != *' '* ]] || { echo 'Build directory must not contain spaces' >&2; exit 1; }
br="$workspace/buildroot-$BUILDROOT_VERSION"
if [[ "$profile" == display ]]; then
    out=${AX7Z020_OUTPUT_DIR:-"$workspace/output-display-udev"}
else
    out=${AX7Z020_OUTPUT_DIR:-"$workspace/output-$profile"}
fi
out=$(realpath -m -- "$out")
case "$out" in "$workspace"/*) ;; *) echo 'Output must stay within the build workspace' >&2; exit 1 ;; esac
[[ "$out" != *' '* ]] || { echo 'Output directory must not contain spaces' >&2; exit 1; }
downloads="$workspace/downloads"
mkdir -p "$workspace/logs"
log="$workspace/logs/${profile}-${action}-$(date -u +%Y%m%dT%H%M%SZ).log"
exec > >(tee "$log") 2>&1
printf 'UTC start: %s\nProfile: %s\nAction: %s\nLog: %s\n' "$(date -u --iso-8601=seconds)" "$profile" "$action" "$log"
trap 'result=$?; printf "UTC finish: %s; exit=%s\n" "$(date -u --iso-8601=seconds)" "$result"; exit "$result"' EXIT
bash "$platform/scripts/check-host.sh"
for hook in "$external/board/ax7z020/post-build.sh" "$external/board/ax7z020/post-image.sh"; do
    [[ -x "$hook" ]] || chmod +x "$hook"
done
if [[ ! -d "$br/.git" ]]; then
    if [[ -e "$br" ]]; then
        echo "Refusing to overwrite an existing non-git source directory: $br" >&2
        exit 1
    fi
    # Official git service first; mirror contains the exact same pinned commit.
    if ! git clone --depth 1 --branch "$BUILDROOT_VERSION" "${AX7Z020_BR_GIT_URL:-$BUILDROOT_GIT_URL}" "$br"; then
        echo 'Official clone did not complete. Retry with the documented mirror in a fresh directory; partial sources are retained.' >&2
        exit 1
    fi
fi
actual_commit=$(git -C "$br" rev-parse HEAD)
[[ "$actual_commit" == "$BUILDROOT_COMMIT" ]] || {
    printf 'Buildroot commit mismatch: expected %s, got %s\n' "$BUILDROOT_COMMIT" "$actual_commit" >&2
    exit 1
}
if [[ -n $(git -C "$br" status --porcelain) ]]; then
    echo 'Pinned Buildroot source has local modifications; inspect them before building.' >&2
    exit 1
fi
makebr() {
    local mirror_args=()
    if [[ -n ${AX7Z020_GNU_MIRROR:-} ]]; then
        mirror_args+=("BR2_GNU_MIRROR=$AX7Z020_GNU_MIRROR")
    fi
    if [[ -n ${AX7Z020_PRIMARY_SITE:-} ]]; then
        mirror_args+=("BR2_PRIMARY_SITE=$AX7Z020_PRIMARY_SITE")
    fi
    if [[ ${AX7Z020_ARIA_DOWNLOADS:-0} == 1 ]]; then
        mirror_args+=("BR2_WGET=bash $platform/scripts/aria-wget.sh")
    fi
    make -C "$br" O="$out" BR2_EXTERNAL="$external" BR2_DL_DIR="$downloads" \
        BR2_JLEVEL="${AX7Z020_JOBS:-4}" "${mirror_args[@]}" "$@"
}
configure() {
    makebr ax7z020_initramfs_defconfig
    if [[ "$profile" == qt || "$profile" == display ]]; then
        cat "$external/configs/ax7z020_qt.fragment" >> "$out/.config"
        makebr olddefconfig
    fi
    if [[ "$profile" == display ]]; then
        cat "$external/configs/ax7z020_display.fragment" >> "$out/.config"
        makebr olddefconfig
    fi
    python3 "$platform/scripts/verify-config.py" "$out/.config" "$profile"
    printf '%s\n' "$BUILDROOT_COMMIT" > "$out/BUILDROOT_COMMIT"
}
display_lock_matches() {
    python3 - "$platform/display-platform.lock.json" "$out/build/linux-custom/ax7z020-display-source-lock.json" <<'PY'
import json, pathlib, sys
paths = [pathlib.Path(value) for value in sys.argv[1:]]
sys.exit(0 if all(path.is_file() for path in paths) and
         json.loads(paths[0].read_text()) == json.loads(paths[1].read_text()) else 1)
PY
}
case "$action" in
    configure) configure ;;
    source)
        [[ -f "$out/.config" ]] || configure
        makebr -j"${AX7Z020_JOBS:-4}" source
        ;;
    kernel-extract)
        [[ -f "$out/.config" ]] || configure
        makebr linux-patch
        ;;
    kernel-build)
        [[ -f "$out/.config" ]] || configure
        if [[ "$profile" == display && -f "$out/build/linux-custom/.stamp_configured" ]] && \
           ! display_lock_matches; then
            makebr -j"${AX7Z020_JOBS:-4}" linux-reconfigure
        else
            makebr -j"${AX7Z020_JOBS:-4}" linux
        fi
        ;;
    build)
        [[ -f "$out/.config" ]] || configure
        python3 "$platform/scripts/verify-config.py" "$out/.config" "$profile"
        if [[ "$profile" == display && -f "$out/build/linux-custom/.stamp_configured" ]] && \
           ! display_lock_matches; then
            echo 'Reviewed display source lock changed; reconfigure/rebuild the matching kernel.'
            makebr -j"${AX7Z020_JOBS:-4}" linux-reconfigure
        fi
        makebr -j"${AX7Z020_JOBS:-4}"
        python3 "$platform/scripts/verify-artifacts.py" "$out" "$profile"
        ;;
    app-rebuild)
        [[ -f "$out/.config" ]] || { echo 'Configure/build this profile first' >&2; exit 1; }
        if [[ "$profile" == display ]]; then
            makebr ax7z020-display-tools-rebuild
            makebr qt-ax7zfb-reconfigure
        fi
        makebr vibration-offline-rebuild
        makebr -j"${AX7Z020_JOBS:-4}"
        python3 "$platform/scripts/verify-artifacts.py" "$out" "$profile"
        ;;
    legal-info)
        # The upstream local-site collector ignores the rsync source whitelist.
        # Package REDISTRIBUTE=NO disables that unsafe project-root collector.
        python3 "$platform/scripts/archive-app-source.py" "$platform/.." "$out/legal-info/project-source"
        makebr legal-info
        ;;
    verify) python3 "$platform/scripts/verify-artifacts.py" "$out" "$profile" ;;
esac
printf 'Profile output: %s\n' "$out"
}
main "$@"
