#!/usr/bin/env bash
# Verify a kernel.org candidate against its official digest and developer key.
set -euo pipefail
platform=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace=${AX7Z020_BUILD_ROOT:-"$HOME/ax7z020-build"}
archive="$workspace/downloads/linux/linux-6.6.70.tar.xz"
signature="$workspace/staged-sources/linux-6.6.70.tar.sign"
keyring="$workspace/verification-keyring"
fingerprint=647F28654894E3BD457199BE38DBBDC86092693E
expected=84d23ee07fb26febbcb6d1295ba15efdc67ac382b4137b2c8853146c10fd2f97
[[ -f "$archive" && -f "$signature" && -d "$keyring" ]] || {
    echo 'Download the candidate, developer signature and publisher WKD key first.' >&2; exit 1;
}
actual=$(sha256sum "$archive" | cut -d' ' -f1)
[[ "$actual" == "$expected" ]] || { echo 'Kernel archive SHA256 mismatch' >&2; exit 1; }
evidence="$platform/validation/kernel-source"
mkdir -p "$evidence"
xz -dc "$archive" | gpg --homedir "$keyring" --status-fd 1 --verify "$signature" - \
    > "$evidence/gpg-status.txt" 2> "$evidence/gpg-verification.txt"
grep -q "^\[GNUPG:\] VALIDSIG $fingerprint " "$evidence/gpg-status.txt"
gpg --homedir "$keyring" --armor --export "$fingerprint" > "$evidence/gregkh-public-key.asc"
cp "$signature" "$evidence/linux-6.6.70.tar.sign"
printf '%s  %s\n' "$expected" linux-6.6.70.tar.xz > "$evidence/SHA256SUMS"
cat "$evidence/gpg-verification.txt"
python3 - "$evidence" "$actual" "$fingerprint" <<'PY'
import json, pathlib, sys
evidence = pathlib.Path(sys.argv[1])
record = {
    "provider": "kernel.org upstream stable, not the Xilinx branch",
    "version": "6.6.70",
    "archive_sha256": sys.argv[2],
    "sha256_matches_official_kernel_org_checksum": True,
    "developer_signature_verified": True,
    "signer_fingerprint": sys.argv[3],
    "public_key_source": "kernel.org WKD over verified TLS",
    "fingerprint_source": "https://www.kernel.org/signature.html",
    "checksum_source": "https://www.kernel.org/pub/linux/kernel/v6.x/sha256sums.asc",
    "mirror": "https://mirrors.tuna.tsinghua.edu.cn/kernel/v6.x/linux-6.6.70.tar.xz",
    "signature_source": "https://www.kernel.org/pub/linux/kernel/v6.x/linux-6.6.70.tar.sign",
    "board_boot_verified": False,
    "note": "Signature/source integrity only; Kconfig, compile and board acceptance are separate checks."
}
(evidence / "release-verification.json").write_text(json.dumps(record, indent=2) + "\n")
print(json.dumps(record, indent=2))
PY
