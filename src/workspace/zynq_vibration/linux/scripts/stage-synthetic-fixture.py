#!/usr/bin/env python3
"""Generate the explicitly approved development sample inside build storage."""
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

project = pathlib.Path(sys.argv[1]).resolve()
target = pathlib.Path(sys.argv[2]).resolve()
build = pathlib.Path(sys.argv[3]).resolve()
assert build.is_dir(), "Buildroot BUILD_DIR must already exist"
generator = project / "tools/generate_fixture.py"
assert generator.is_file(), "Project synthetic fixture generator is absent"
target.mkdir(parents=True, exist_ok=True)
target.chmod(0o755)
for name in ("synthetic_10s.BIN", "synthetic_10s.meta.json", "SYNTHETIC-ONLY.txt"):
    output = target / name
    assert not output.is_symlink(), "Unexpected link in synthetic staging directory"
    if output.exists():
        output.chmod(0o644)
with tempfile.TemporaryDirectory(prefix="ax7z020-synthetic-", dir=build) as temporary:
    temporary = pathlib.Path(temporary).resolve()
    assert temporary.parent == build
    sample = temporary / "synthetic_10s.BIN"
    subprocess.run([
        sys.executable, str(generator), str(sample), "--seconds", "10",
        "--sample-rate", "50000", "--rpm", "2000", "--ppr", "1",
        "--header-ppr", "0", "--profile", "demo", "--seed", "20261024",
    ], check=True)
    sidecar = sample.with_suffix(".meta.json")
    metadata = json.loads(sidecar.read_text())
    digest = hashlib.sha256(sample.read_bytes()).hexdigest()
    assert metadata["synthetic"] is True
    assert metadata["record_count"] == 500000 and metadata["sample_rate_hz"] == 50000
    assert metadata["duration_s"] == 10 and metadata["fault_label"] is None
    assert metadata["sha256"] == digest
    canonical = project / "artifacts/samples/synthetic_10s.meta.json"
    if canonical.is_file():
        known = json.loads(canonical.read_text())
        assert known["sha256"] == digest, "Generated sample differs from the approved canonical demo"
    shutil.copyfile(sample, target / sample.name)
    shutil.copyfile(sidecar, target / sidecar.name)
(target / "SYNTHETIC-ONLY.txt").write_text(
    "Software development sample; no physical machine was measured.\n"
    "Use only for parser, CRC, FFT, RPM and ARM timing checks.\n"
    "No normal/fault ground-truth label or diagnosis accuracy is established.\n"
)
for name in ("synthetic_10s.BIN", "synthetic_10s.meta.json", "SYNTHETIC-ONLY.txt"):
    (target / name).chmod(0o444)
# Keep the build tree directory writable for Buildroot's fakeroot/rsync cleanup.
# The packaged data files themselves are read-only.
print(f"Synthetic fixture installed with metadata: SHA256={digest}")
