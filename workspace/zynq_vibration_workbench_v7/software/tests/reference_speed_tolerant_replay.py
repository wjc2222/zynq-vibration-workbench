#!/usr/bin/env python3
"""Replay already extracted measured windows through the real C++ policies.

This audits software behavior. It never estimates machine accuracy, fits a
model from queries, changes labels, or substitutes zero for missing speed CV.
All transport intermediates are contained in a system temporary directory.
"""
import argparse
import collections
import hashlib
import json
import pathlib
import subprocess
import tempfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def encoded(value):
    return value.encode("utf-8").hex() or "-"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("probe", "model", "windows", "labels", "output"):
        parser.add_argument("--" + name, required=True, type=pathlib.Path)
    args = parser.parse_args()
    inputs = (args.probe, args.model, args.windows, args.labels)
    if args.output.resolve() in {path.resolve() for path in inputs}:
        raise ValueError("audit output must not overwrite any input")
    labels = json.loads(args.labels.read_text(encoding="utf-8-sig"))
    reference_hashes = set()
    for line in args.model.read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition("=")
        if key.startswith("reference_") and key[len("reference_"):].isdigit():
            reference_hashes.add(value.split(",")[3])
    rows = []
    measured = []
    for line in args.windows.read_text(encoding="utf-8-sig").splitlines():
        wrapper = json.loads(line)
        window = dict(wrapper.get("window", wrapper))
        for field in ("speed_cv", "fresh_periods"):
            if field not in window:
                window[field] = wrapper[field]  # missing input is an error
        window["elapsed_ms"] = wrapper.get("elapsed_ms", window.get("elapsed_ms", 0))
        measured.append(window)
        row = [window["fs"], int(window["quality"] == "valid"), int(window["synthetic"]),
               window["first_index"], window["last_index"], window["analysis_first_index"],
               window["analysis_last_index"], window["rpm"], window["rpm_min"], window["rpm_max"],
               window["speed_cv"], window["fresh_periods"], window["elapsed_ms"]]
        row += [encoded(window[key]) for key in (
            "setup_id", "configuration", "source", "input_kind", "source_path", "raw_window_sha256",
            "capture_sha256", "source_session_id", "feature_schema", "quality_policy")]
        row += window["reference_rms_adc"]
        row += [int(value) for value in window["ratio_features_valid"]]
        if len(window["features"]) != 36:
            raise ValueError("window must have exactly 36 measured features")
        row += window["features"]
        row += [len(window["reasons"])] + [encoded(reason) for reason in window["reasons"]]
        rows.append(" ".join(map(str, row)))
    with tempfile.TemporaryDirectory(prefix="v7-speed-policy-replay-") as temporary:
        transport = pathlib.Path(temporary) / "measured-features.txt"
        result = pathlib.Path(temporary) / "cpp-policy-results.json"
        transport.write_text("V3_MEASURED_FEATURE_REPLAY_V1\n" + "\n".join(rows) + "\n", encoding="ascii")
        completed = subprocess.run([str(args.probe), "--windows", str(args.model), str(transport), str(result)],
                                   check=True, capture_output=True, text=True)
        report = json.loads(result.read_text(encoding="utf-8"))
    if len(report["windows"]) != len(measured):
        raise ValueError("C++ replay did not preserve every measured window")
    files = {}
    for source, item in zip(measured, report["windows"]):
        replayed = item["window"]
        for key in ("rpm", "rpm_min", "rpm_max", "speed_cv", "fresh_periods", "features", "reasons",
                    "quality", "capture_sha256", "raw_window_sha256", "source_path", "source_session_id"):
            if source[key] != replayed[key]:
                raise ValueError("measured replay transport changed " + key)
        annotation = labels.get(source["source_path"])
        member = source["capture_sha256"] in reference_hashes
        item["capture_hash_in_original_reference_model"] = member
        item["operator_label_for_organization_only_not_inference_input"] = annotation.get("label") if annotation else None
        summary = files.setdefault(source["capture_sha256"], {
            "capture_sha256": source["capture_sha256"], "source_path": source["source_path"],
            "in_reference_model": member, "operator_label": annotation.get("label") if annotation else None,
            "assessment_scope": "reference_replay_only" if member else "additional_same_rig_capture_not_four_class_blind_validation",
            "windows": 0, "quality_valid_windows": 0, "v2_decision_counts_no_accuracy": collections.Counter(),
            "v3_decision_counts_no_accuracy": collections.Counter(), "v3_status_counts": collections.Counter(),
            "v3_mode_support_counts": collections.Counter()})
        summary["windows"] += 1
        summary["quality_valid_windows"] += source["quality"] == "valid"
        for version in ("v2", "v3"):
            summary[version + "_decision_counts_no_accuracy"][item[version]["classification"]] += 1
        summary["v3_status_counts"][item["v3"]["status"]] += 1
        summary["v3_mode_support_counts"][item["v3"]["shape_support_status"] or "not_evaluated"] += 1
    report.update({"input_windows_sha256": sha256(args.windows), "input_model_file_sha256": sha256(args.model),
                   "input_labels_sha256": sha256(args.labels), "source_window_values_preserved_exactly": True,
                   "labels_passed_to_inference": False, "raw_reextraction_performed": False,
                   "files": list(files.values()), "cpp_probe_result": completed.stdout.strip(),
                   "interpretation": "Training files are replay; remaining captures include unlabeled data and one same-rig normal annotation, not a four-class blind test. Decision counts are behavior only; no accuracy or calibrated confidence is reported."})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"windows": len(measured), "files": len(files), "reference_files": sum(f["in_reference_model"] for f in files.values()),
                      "quality_valid_windows": sum(w["quality"] == "valid" for w in measured), "accuracy_measured": False,
                      "report_sha256": sha256(args.output)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
