#!/usr/bin/env python3
"""Mathematical transport fixtures for warnings and sealed-output protection."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    args = parser.parse_args()
    driver = pathlib.Path(__file__).resolve().parents[2] / "tools" / "analysis" / "run_supervised_cpp_audit.py"
    labels = ("normal", "imbalance", "misalignment", "looseness")
    with tempfile.TemporaryDirectory(prefix="supervised-transport-software-fixture-") as temporary:
        directory = pathlib.Path(temporary)
        windows, annotations = [], {}
        for category, label in enumerate(labels):
            for source in range(2):
                identity = category * 10 + source
                path = "mathematical-only-no-machine-capture-" + str(identity) + ".BIN"
                capture = hashlib.sha256(path.encode()).hexdigest()
                session = "mathematical-only-session-" + str(identity)
                annotations[path] = {"label": label, "session_id": session}
                for window in range(2):
                    windows.append({"feature_schema": "rpmfree69.v1", "rpm_used": False, "source_path": path,
                                    "capture_sha256": capture, "source_session_id": session,
                                    "raw_window_sha256": hashlib.sha256((path + str(window)).encode()).hexdigest(),
                                    "configuration": "abcd1234", "setup_id": "mathematical-only", "fs": 5000,
                                    "first_index": window * 10000, "last_index": window * 10000 + 8191,
                                    "quality": "valid", "synthetic": False,
                                    "features": [(2.0 if j % 4 == category else .1) + .0001 * identity for j in range(69)],
                                    "reasons": [], "warnings": ["raw_adc_clipping_CH1"] if identity == window == 0 else []})
        input_path, labels_path = directory / "mathematical-windows.jsonl", directory / "mathematical-labels.json"
        input_path.write_text("\n".join(json.dumps(w) for w in windows) + "\n", encoding="utf-8")
        labels_path.write_text(json.dumps(annotations), encoding="utf-8")
        output = directory / "sealed-software-fixture"
        command = [sys.executable, str(driver), "--probe", str(args.probe), "--windows", str(input_path),
                   "--labels", str(labels_path), "--output-directory", str(output)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        for algorithm in ("rf42", "linear42"):
            model = (output / algorithm / "supervised.model").read_text(encoding="utf-8")
            assert "training_warning_windows=1\n" in model, "transport silently discarded a training warning"
            replay = json.loads((output / algorithm / "full-fit-replay.json").read_text())
            assert any("input warning: raw_adc_clipping_CH1" in item["result"]["evidence"] for item in replay["windows"])
        before = {str(path.relative_to(output)): digest(path) for path in output.rglob("*") if path.is_file()}
        rejected = subprocess.run(command, capture_output=True, text=True)
        assert rejected.returncode != 0 and "Refusing to overwrite" in rejected.stderr
        direct = subprocess.run([str(args.probe), str(directory / "does-not-exist.matrix"),
                                 str(output / "rf42" / "supervised.model"), str(directory / "unwritten-validation.json"),
                                 str(directory / "unwritten-predictions.json"), "rf42"], capture_output=True, text=True)
        assert direct.returncode != 0 and "refusing to overwrite" in direct.stderr
        after = {str(path.relative_to(output)): digest(path) for path in output.rglob("*") if path.is_file()}
        assert before == after, "an attempted repeat changed sealed experiment output"
    print("PASS: mathematical warning transport retained for both native trainers; Python/C++ sealed-output refusal preserves every hash; no real machine metrics measured.")


if __name__ == "__main__":
    main()
