"""Synthetic protocol/hash/state tests; no real-machine accuracy measurement."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--generator", required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("fixture", args.generator)
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    with tempfile.TemporaryDirectory(prefix="v6-candidate-cli-") as directory:
        root = Path(directory)
        source = root / "synthetic.BIN"
        fixture.generate(source, seconds=10, sample_rate=5000, rpm=1200,
                         ppr=1, header_ppr=1, profile="sine", signal_hz=20)
        original = source.read_bytes()
        sidecar = source.with_suffix(".meta.json")
        metadata = sidecar.read_bytes()

        def run(command, output, *extra, valid=True):
            result = subprocess.run([args.executable, command, "--input", str(source),
                                     "--output", str(output), "--setup", "rig1_mount1", *extra],
                                    capture_output=True, text=True)
            assert (result.returncode == 0) == valid, result.stderr
            return result

        features = root / "features.jsonl"
        run("features", features, "--confirm-mechanical")
        rows = [json.loads(line) for line in features.read_text().splitlines()]
        assert len(rows) == 6
        n = 65 + 8191
        for row in rows:
            assert row["quality"] == "invalid" and row["synthetic"]
            assert "synthetic_not_machine_evidence" in row["reasons"]
            assert row["capture_sha256"] == hashlib.sha256(original).hexdigest()
            first = row["first_index"]
            support = original[4096 + first * 32:4096 + (first + n) * 32]
            assert row["raw_window_sha256"] == hashlib.sha256(support).hexdigest()
            assert row["source_session_id"] == ""
            assert row["analysis_first_index"] == first + 32
            assert row["analysis_last_index"] == first + 32 + 8191
        baseline = root / "normal.model"
        run("baseline", baseline, "--confirm-mechanical", "--confirm-normal", valid=False)
        assert not baseline.exists(), "synthetic baseline must not be written"
        raw = root / "window.s16le"
        run("export-raw", raw)
        expected = b"".join(original[4096+i*32+16:4096+i*32+22] for i in range(n))
        assert raw.read_bytes() == expected
        run("export-raw", source, valid=False)
        run("features", sidecar, valid=False)
        labels = source.with_suffix(".labels.json")
        rejected = run("features", labels, valid=False)
        assert "output must not overwrite" in rejected.stderr and not labels.exists()
        labels.write_bytes(b'{"purpose":"isolated operator-label protection sentinel"}\n')
        label_hash = hashlib.sha256(labels.read_bytes()).hexdigest()
        for command, extra in (
                ("features", ()), ("export-raw", ()),
                ("baseline", ("--confirm-mechanical", "--confirm-normal")),
                ("infer", ("--model", str(root / "unused.model")))):
            rejected = run(command, labels, *extra, valid=False)
            assert "output must not overwrite" in rejected.stderr, rejected.stderr
            assert hashlib.sha256(labels.read_bytes()).hexdigest() == label_hash
        assert source.read_bytes() == original and sidecar.read_bytes() == metadata
        print("PASS synthetic CLI: exact file/window SHA256, group-delay support, raw export, quality rejection, source protection, absent/existing operator-label protection with unchanged SHA256 for all four commands")


if __name__ == "__main__":
    main()
