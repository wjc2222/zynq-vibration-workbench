#!/usr/bin/env python3
"""Self-contained CLI contract tests. Writes a sanitized report only with --output."""
import argparse
import copy
import json
import math
from pathlib import Path
import subprocess
import sys


HERE = Path(__file__).resolve().parent
SCRIPTS = HERE.parents[1] / "scripts"


def load(name):
    return json.loads((HERE / name).read_text(encoding="utf-8"))


def changed(original, **values):
    result = copy.deepcopy(original)
    result.update(values)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", help="Optional destination for sanitized JSON results")
    args = parser.parse_args()
    results = []

    def call(case_id, tool, value, exit_code, check=lambda result: True, raw=False):
        flag = "--profile" if tool == "engineering_budget" else "--input"
        process = subprocess.run([sys.executable, "-B", str(SCRIPTS / (tool + ".py")), flag, "-"],
                                 input=value if raw else json.dumps(value), text=True,
                                 capture_output=True, timeout=15)
        try:
            result = json.loads(process.stdout)
            passed = process.returncode == exit_code and not process.stderr and check(result)
        except (ValueError, KeyError, TypeError):
            passed = False
        results.append({"case": case_id, "expected_exit": exit_code,
                        "actual_exit": process.returncode, "pass": bool(passed)})

    audio = load("audio_profile.json")
    telemetry = load("telemetry_profile.json")
    call("audio_declared_budget", "engineering_budget", audio, 0,
         lambda r: r["quantities"]["raw_bytes_per_second"] == 480000
         and r["quantities"]["duration_payload_bytes"] == 14400000
         and r["quantities"]["storage_margin_bytes_per_second"] == 3520000
         and r["quantities"]["uart_payload_bytes_per_second"] == 1200000
         and math.isclose(r["quantities"]["fft_observation_seconds"], 4096 / 24000)
         and r["quantities"]["fft_bin_spacing_hz"] == 5.859375)
    call("telemetry_declared_budget", "engineering_budget", telemetry, 0,
         lambda r: r["quantities"]["raw_bytes_per_second"] == 1280
         and r["quantities"]["fft_observation_seconds"] == 12.8
         and r["quantities"]["fft_bin_spacing_hz"] == 0.078125)
    call("raw_uart_bottleneck", "engineering_budget", changed(audio, uart_baud=115200), 1,
         lambda r: not r["checks"]["uart_full_raw_feasible"] and r["checks"]["storage_full_raw_feasible"])
    call("storage_bottleneck", "engineering_budget", changed(audio, storage_MBps=0.1), 1,
         lambda r: r["quantities"]["storage_margin_bytes_per_second"] == -380000)
    call("exact_storage_boundary", "engineering_budget", changed(audio, storage_MBps=0.48), 0,
         lambda r: r["quantities"]["storage_margin_bytes_per_second"] == 0)
    call("exact_uart_boundary", "engineering_budget", changed(audio, uart_baud=4800000), 0,
         lambda r: r["quantities"]["uart_margin_bytes_per_second"] == 0)
    call("short_recording_cannot_fit_fft", "engineering_budget", changed(audio, duration_seconds=0.1), 1,
         lambda r: not r["checks"]["fft_window_fits_duration"])
    call("resource_limit_exceeded", "engineering_budget", changed(audio, resources={"logic": {"used": 81, "capacity": 100, "limit_fraction": 0.8}}), 1,
         lambda r: not r["resources"]["logic"]["pass"])
    call("resource_exact_limit", "engineering_budget", changed(audio, resources={"memory": {"used": 80, "capacity": 100, "limit_fraction": 0.8}}), 0)
    call("near_post_decimation_nyquist", "engineering_budget", changed(audio, signal_max_hz=11999), 0)
    call("at_post_decimation_nyquist", "engineering_budget", changed(audio, signal_max_hz=12000), 1,
         lambda r: not r["checks"]["signal_below_analysis_nyquist"])
    call("above_post_decimation_nyquist", "engineering_budget", changed(audio, signal_max_hz=13000), 1)
    without_band = dict(audio)
    del without_band["signal_max_hz"]
    call("no_inferred_signal_band", "engineering_budget", without_band, 0,
         lambda r: "signal_below_analysis_nyquist" not in r["checks"])
    call("zero_record_overhead", "engineering_budget", changed(audio, per_record_overhead_bytes=0), 0,
         lambda r: r["quantities"]["raw_bytes_per_second"] == 288000)
    call("no_assumed_fft_radix", "engineering_budget", changed(audio, fft_size=3000), 0)
    for case_id, values in [
        ("invalid_bool", {"channels": True}), ("invalid_fractional_channels", {"channels": 1.5}),
        ("invalid_zero_rate", {"sampling_hz": 0}), ("invalid_negative_overhead", {"per_record_overhead_bytes": -1}),
        ("invalid_nan", {"sampling_hz": float("nan")}), ("invalid_inf", {"uart_baud": float("inf")}),
        ("invalid_numeric_overflow", {"sampling_hz": 1e308}),
        ("invalid_efficiency", {"uart_framing_efficiency": 1.01}),
        ("invalid_resource_bool", {"resources": {"cpu": {"used": True, "capacity": 1}}}),
        ("invalid_resource_zero_capacity", {"resources": {"cpu": {"used": 1, "capacity": 0}}}),
        ("invalid_resource_limit", {"resources": {"cpu": {"used": 1, "capacity": 2, "limit_fraction": 1.1}}}),
        ("invalid_unknown_key", {"measured_MBps": 4}), ("invalid_null_fft", {"fft_size": None}),
    ]:
        call(case_id, "engineering_budget", changed(audio, **values), 2, lambda r: r["status"] == "invalid")
    missing = dict(audio)
    del missing["channels"]
    call("invalid_missing_channels", "engineering_budget", missing, 2)
    call("invalid_duplicate_profile_key", "engineering_budget", '{"channels":1,"channels":2}', 2, raw=True)
    call("invalid_nested_duplicate_key", "engineering_budget", '{"resources":{"cpu":{"used":1,"used":2}}}', 2, raw=True)
    good = load("dataset_good.json")
    call("dataset_good_disjoint_groups", "dataset_audit", good, 0,
         lambda r: r["row_count"] == 6 and r["split_counts"] == {"train": 2, "validation": 2, "test": 2})
    call("dataset_cross_split_group_path_hash_leakage", "dataset_audit", load("dataset_leakage.json"), 1,
         lambda r: len(r["group_leakage"]) == 1 and len(r["duplicate_identities"]) == 2)
    missing_label = copy.deepcopy(good)
    missing_label["rows"].pop()
    call("dataset_missing_label", "dataset_audit", missing_label, 1,
         lambda r: r["missing_labels_by_required_split"]["test"] == ["fault"])
    unexpected = changed(good, expected_labels=["normal"])
    call("dataset_unexpected_label", "dataset_audit", unexpected, 1,
         lambda r: r["unexpected_labels"] == ["fault"])
    train_only = {"rows": good["rows"][:2], "required_splits": ["train"]}
    call("dataset_explicit_train_only_inventory", "dataset_audit", train_only, 0)
    call("dataset_default_missing_splits", "dataset_audit", {"rows": good["rows"][:2]}, 1,
         lambda r: not r["checks"]["required_splits_populated"])
    normalized = copy.deepcopy(good)
    normalized["rows"][1]["filepath"] = "samples\\train\\.\\normal.csv"
    call("dataset_normalized_path_duplicate", "dataset_audit", normalized, 1,
         lambda r: len(r["duplicate_identities"]) == 1)
    same_group = copy.deepcopy(good)
    same_group["rows"][1]["group"] = "train-a"
    call("dataset_group_reuse_within_split_allowed", "dataset_audit", same_group, 0)
    no_ids = copy.deepcopy(good)
    for row in no_ids["rows"]:
        del row["filepath"]
    call("dataset_optional_file_identities", "dataset_audit", no_ids, 0)
    for case_id, edits in [
        ("dataset_invalid_label_bool", {"label": True}), ("dataset_invalid_blank_group", {"group": " "}),
        ("dataset_invalid_split", {"split": "holdout"}), ("dataset_invalid_hash", {"hash": "not-sha256"}),
        ("dataset_invalid_hash_bool", {"hash": False}), ("dataset_invalid_path_number", {"filepath": 1}),
        ("dataset_invalid_both_group_session", {"session": "also-provided"}),
        ("dataset_invalid_extra_key", {"accuracy": 1}), ("dataset_invalid_control_label", {"label": "bad\nlabel"}),
    ]:
        value = copy.deepcopy(good)
        value["rows"][0].update(edits)
        call(case_id, "dataset_audit", value, 2)
    no_group = copy.deepcopy(good)
    del no_group["rows"][0]["group"]
    call("dataset_invalid_no_group_session", "dataset_audit", no_group, 2)
    for case_id, data in [
        ("dataset_invalid_empty_rows", {"rows": []}), ("dataset_invalid_rows_object", {"rows": {}}),
        ("dataset_invalid_duplicate_splits", changed(good, required_splits=["train", "train"])),
        ("dataset_invalid_unknown_split", changed(good, required_splits=["holdout"])),
        ("dataset_invalid_empty_expected_labels", changed(good, expected_labels=[])),
        ("dataset_invalid_duplicate_labels", changed(good, expected_labels=["normal", "normal"])),
    ]:
        call(case_id, "dataset_audit", data, 2)
    call("dataset_invalid_duplicate_json_key", "dataset_audit", '{"rows":[],"rows":[]}', 2, raw=True)
    call("dataset_invalid_nan_metadata", "dataset_audit", '{"rows":[{"label":NaN}]}', 2, raw=True)
    for tool in ("engineering_budget", "dataset_audit"):
        process = subprocess.run([sys.executable, "-B", str(SCRIPTS / (tool + ".py")), "--help"],
                                 text=True, capture_output=True, timeout=15)
        results.append({"case": tool + "_help_schema", "pass": process.returncode == 0
                        and "Strict JSON" in process.stdout and not process.stderr})
        process = subprocess.run([sys.executable, "-B", str(SCRIPTS / (tool + ".py"))],
                                 text=True, capture_output=True, timeout=15)
        try:
            passed = process.returncode == 2 and json.loads(process.stdout)["status"] == "invalid" and not process.stderr
        except ValueError:
            passed = False
        results.append({"case": tool + "_missing_flag_json_error", "pass": passed})
    passed_count = sum(item["pass"] for item in results)
    report = {"schema": "portable-tools-contract-checks.v1", "basis": "synthetic metadata and declared profiles only",
              "hardware_measurement": False, "model_training": False,
              "total": len(results), "passed": passed_count, "failed": len(results) - passed_count,
              "checks": results}
    text = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if passed_count == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
