"""Train the deployable V6 linear candidate model from labeled real features.

Input is vibration_detect features JSONL with operator-assigned label, run_id
and group_id. Groups identify independent mechanical setups, not window IDs.
The held-out groups are never used for scaling, optimization or thresholds.
Requires Python 3.10+ and NumPy. Outputs are deliberately provisional.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import re
from pathlib import Path
import numpy as np

LABELS = ("normal", "imbalance", "misalignment", "looseness")
FEATURES = 36


def require(condition, message):
    if not condition:
        raise ValueError(message)


def text_id(value, field):
    require(isinstance(value, str) and 0 < len(value) <= 128 and
            not any(ord(c) < 32 or c == "=" for c in value), f"invalid {field}")
    return value


def real_number(value, field):
    require(type(value) in (int, float) and math.isfinite(value) and abs(value) < 1e100,
            f"invalid finite number: {field}")
    return float(value)


def validate_rows(rows):
    require(20 <= len(rows) <= 100000, "need 20..100000 labeled real windows")
    identity = None
    intervals, runs, content_windows, sources = {}, {}, set(), {}
    for index, row in enumerate(rows):
        prefix = f"row {index + 1}: "
        require(isinstance(row, dict), prefix + "not an object")
        require(row.get("quality") == "valid" and row.get("synthetic") is False,
                prefix + "only quality-valid real windows are accepted")
        require(row.get("feature_schema") == "lowband36.v1", prefix + "wrong feature schema")
        require(row.get("label") in LABELS, prefix + "operator-assigned four-class label required")
        for field in ("run_id", "group_id", "setup_id"):
            text_id(row.get(field), prefix + field)
        for field in ("raw_window_sha256", "capture_sha256"):
            require(isinstance(row.get(field), str) and re.fullmatch(r"[0-9a-f]{64}", row[field]),
                    prefix + "missing immutable " + field + "; re-export with V6")
        session = row.get("source_session_id")
        require(isinstance(session, str) and (session == "" or re.fullmatch(r"[0-9a-f]{32}", session)),
                prefix + "invalid source_session_id")
        require(row["raw_window_sha256"] not in content_windows,
                prefix + "duplicate raw window content, even if run/group was renamed")
        content_windows.add(row["raw_window_sha256"])
        source = "session:"+session if session else "capture:"+row["capture_sha256"]
        for source_key in (source, "capture:"+row["capture_sha256"]):
            require(source_key not in sources or sources[source_key] == row["group_id"],
                    prefix + "same physical source crosses independent groups")
            sources[source_key] = row["group_id"]
        config = row.get("configuration")
        require(isinstance(config, str) and 8 <= len(config) <= 4096 and
                re.fullmatch(r"[0-9a-f]+", config), prefix + "invalid configuration")
        require(type(row.get("fs")) is int and row["fs"] in (5000, 10000, 25000, 50000),
                prefix + "invalid sampling rate")
        current = (row["setup_id"], config, row["fs"])
        identity = identity or current
        require(identity == current, prefix + "mixed installation/filter/calibration/sample rate")
        require(isinstance(row.get("features"), list) and len(row["features"]) == FEATURES,
                prefix + "exactly 36 features required")
        for value in row["features"]:
            require(real_number(value, prefix + "feature") >= 0, prefix + "negative feature")
        rpm = real_number(row.get("rpm"), prefix + "rpm")
        lo = real_number(row.get("rpm_min"), prefix + "rpm_min")
        hi = real_number(row.get("rpm_max"), prefix + "rpm_max")
        require(0 < lo <= rpm <= hi, prefix + "invalid speed interval")
        first, last = row.get("first_index"), row.get("last_index")
        require(type(first) is int and type(last) is int and 0 <= first <= last < 2**64,
                prefix + "invalid raw index range")
        key = row["run_id"]
        run_identity = (row["group_id"], row["label"])
        require(key not in runs or runs[key] == run_identity, prefix + "run crosses groups/labels")
        runs[key] = run_identity
        intervals.setdefault(source, []).append((first, last))
    for run, spans in intervals.items():
        spans.sort()
        require(all(a[1] < b[0] for a, b in zip(spans, spans[1:])),
                f"overlapping/duplicate raw windows in source {run}; export disjoint windows")
    for label in LABELS:
        relevant = [r for r in rows if r["label"] == label]
        require(len(relevant) >= 5, f"need at least 5 windows for {label}")
        require(len({r["group_id"] for r in relevant}) >= 3,
                f"need at least 3 independent groups for {label}, not subdivisions of one run")
    return identity


def split_groups(rows, seed=20261030, fraction=.25):
    """Deterministic group holdout; labels used only to ensure class coverage."""
    groups = sorted({r["group_id"] for r in rows})
    rng = np.random.default_rng(seed)
    count = max(1, round(len(groups) * fraction))
    best = None
    for _ in range(2048):
        selected = set(rng.choice(groups, size=count, replace=False).tolist())
        train = [i for i, r in enumerate(rows) if r["group_id"] not in selected]
        test = [i for i, r in enumerate(rows) if r["group_id"] in selected]
        if any(not any(rows[i]["label"] == c for i in test) or
               sum(rows[i]["label"] == c for i in train) < 5 or
               len({rows[i]["group_id"] for i in train if rows[i]["label"] == c}) < 2
               for c in LABELS):
            continue
        error = sum(abs(sum(rows[i]["label"] == c for i in test) /
                            sum(r["label"] == c for r in rows) - fraction) for c in LABELS)
        candidate = (error, tuple(sorted(selected)), train, test)
        if best is None or candidate[:2] < best[:2]:
            best = candidate
    require(best is not None, "cannot make group holdout with all classes and 2 training groups/class; collect more independent trials")
    return np.array(best[2]), np.array(best[3])


def softmax(logits):
    shifted = logits - logits.max(axis=1, keepdims=True)
    result = np.exp(np.clip(shifted, -700, 0))
    return result / result.sum(axis=1, keepdims=True)


def fit(rows, indices, iterations=1500, l2=.01):
    """Fixed full-batch Adam; equal class then equal independent group weights."""
    train = [rows[int(i)] for i in indices]
    x = np.asarray([r["features"] for r in train], dtype=np.float64)
    y = np.asarray([LABELS.index(r["label"]) for r in train])
    groups_per_class = {c: {r["group_id"] for r in train if r["label"] == c} for c in LABELS}
    counts = {(c, g): sum(r["label"] == c and r["group_id"] == g for r in train)
              for c in LABELS for g in groups_per_class[c]}
    sample_weight = np.asarray([1 / (4 * len(groups_per_class[r["label"]]) *
                            counts[(r["label"], r["group_id"])]) for r in train])
    mean = np.sum(x * sample_weight[:, None], axis=0)
    variance = np.sum((x - mean)**2 * sample_weight[:, None], axis=0)
    floor = np.array([1e-9 if i % 12 in (0, 3) else .01 for i in range(FEATURES)])
    scale = np.maximum.reduce([np.sqrt(variance), np.abs(mean) * .01, floor])
    z = (x - mean) / scale
    design = np.column_stack((z, np.ones(len(z))))
    theta, m, v = (np.zeros((FEATURES + 1, 4)) for _ in range(3))
    target = np.eye(4)[y]
    for step in range(1, iterations + 1):
        probabilities = softmax(design @ theta)
        gradient = design.T @ ((probabilities - target) * sample_weight[:, None])
        gradient[:-1] += l2 * theta[:-1]
        m = .9 * m + .1 * gradient
        v = .999 * v + .001 * gradient**2
        theta -= .02 * (m / (1 - .9**step)) / (np.sqrt(v / (1 - .999**step)) + 1e-8)
    require(np.isfinite(theta).all(), "optimization produced nonfinite coefficients")
    return mean, scale, theta[:-1].T, theta[-1]


def predict(row, model):
    z = (np.asarray(row["features"]) - model["mean"]) / model["scale"]
    scores = 100 * softmax((np.asarray(model["weights"]).reshape(4, 36) @ z + model["bias"])[None, :])[0]
    order = np.argsort(-scores, kind="stable")
    raw = LABELS[int(order[0])]
    if row["rpm_min"] < model["rpm_min"] or row["rpm_max"] > model["rpm_max"]:
        return raw, "unknown", "outside_operating_range", scores.tolist()
    if scores[order[0]] < model["minimum_score"] or scores[order[0]] - scores[order[1]] < model["minimum_margin"] or np.sqrt(np.mean(z*z)) > model["maximum_distance"]:
        return raw, "unknown", "rejected_unknown", scores.tolist()
    return raw, raw, "provisional_candidate", scores.tolist()


def model_text(model):
    lines = []
    for key, value in model.items():
        if isinstance(value, (list, np.ndarray)):
            value = ",".join(format(float(v), ".17g") for v in np.asarray(value).ravel())
        elif isinstance(value, float):
            value = format(value, ".17g")
        lines.append(f"{key}={value}")
    return "\n".join(lines) + "\n"


def evaluate(rows, indices, model):
    confusion = np.zeros((4, 5), dtype=int)
    raw_confusion = np.zeros((4, 4), dtype=int)
    details = []
    for idx in indices:
        r = rows[int(idx)]
        raw, prediction, status, scores = predict(r, model)
        truth = LABELS.index(r["label"])
        confusion[truth, LABELS.index(prediction) if prediction in LABELS else 4] += 1
        raw_confusion[truth, LABELS.index(raw)] += 1
        details.append({"row_index": int(idx), "group_id": r["group_id"], "run_id": r["run_id"],
                        "truth": r["label"], "prediction": prediction, "status": status,
                        "scores_not_calibrated_probabilities": scores})
    total = int(confusion.sum())
    accepted = total - int(confusion[:, 4].sum())
    correct = sum(int(confusion[i, i]) for i in range(4))
    metrics = {}
    for i, label in enumerate(LABELS):
        tp = int(confusion[i, i]); fn = int(confusion[i].sum()) - tp
        fp = int(confusion[:, i].sum()) - tp
        metrics[label] = {"recall_including_rejections": tp/(tp+fn) if tp+fn else 0,
                          "precision": tp/(tp+fp) if tp+fp else 0,
                          "f1": 2*tp/(2*tp+fp+fn) if 2*tp+fp+fn else 0}
    group_names = sorted({d["group_id"] for d in details})
    weighted = np.zeros((4, 5))
    group_reports = []
    for group in group_names:
        subset = [d for d in details if d["group_id"] == group]
        for d in subset:
            weighted[LABELS.index(d["truth"]), LABELS.index(d["prediction"]) if d["prediction"] in LABELS else 4] += 1/(len(group_names)*len(subset))
        group_reports.append({"group_id": group, "windows": len(subset),
                              "correct_over_all_windows": sum(d["truth"] == d["prediction"] for d in subset)/len(subset),
                              "coverage": sum(d["prediction"] != "unknown" for d in subset)/len(subset)})
    weighted_f1 = []
    for i in range(4):
        denominator = weighted[i].sum() + weighted[:, i].sum()
        weighted_f1.append(float(2*weighted[i,i]/denominator) if denominator else 0)
    return {"rows": total, "confusion_truth_rows_prediction_columns": confusion.tolist(),
            "prediction_columns": [*LABELS, "unknown"], "raw_argmax_confusion": raw_confusion.tolist(),
            "coverage": accepted/total, "correct_over_all_windows": correct/total,
            "accuracy_on_accepted_only": correct/accepted if accepted else None,
            "macro_f1_including_rejections": sum(m["f1"] for m in metrics.values())/4,
            "per_class": metrics, "predictions": details,
            "independent_group_count": len(group_names),
            "groups_per_class": {c: len({d["group_id"] for d in details if d["truth"] == c}) for c in LABELS},
            "group_reports": group_reports,
            "equal_group_weighted": {"confusion": weighted.tolist(),
                                     "coverage": float(1-weighted[:, 4].sum()),
                                     "correct_over_all_windows": float(np.trace(weighted[:,:4])),
                                     "macro_f1_including_rejections": sum(weighted_f1)/4}}


def train(rows, model_id, seed=20261030, iterations=1500):
    setup, configuration, fs = validate_rows(rows)
    text_id(model_id, "model id")
    require(100 <= iterations <= 10000, "iterations outside 100..10000")
    train_index, test_index = split_groups(rows, seed)
    mean, scale, weights, bias = fit(rows, train_index, iterations)
    training_rows = [rows[int(i)] for i in train_index]
    # Deployment domain is derived only from the training groups.
    model = {"schema": "zynq_vibration.candidate_model.v1", "id": model_id,
             "setup_id": setup, "status": "provisional", "kind": "linear_softmax",
             "configuration": configuration, "fs": fs, "feature_count": 36,
             "baseline_count": sum(r["label"] == "normal" for r in training_rows),
             "rpm_min": min(r["rpm_min"] for r in training_rows) * .9,
             "rpm_max": max(r["rpm_max"] for r in training_rows) * 1.1,
             "minimum_score": 55, "minimum_margin": 12, "maximum_distance": 25,
             "mean": mean.tolist(), "scale": scale.tolist(),
             "bias": bias.tolist(), "weights": weights.ravel().tolist()}
    report = {"schema": "zynq_vibration.training_report.v1", "model_id": model_id,
              "status": "provisional", "mechanical_validation_complete": False,
              "validation_scope": "operator-labeled held-out groups only; prospective rig validation required",
              "seed": seed, "iterations": iterations, "l2": .01, "learning_rate": .02,
              "class_order": LABELS, "training_rows": train_index.tolist(),
              "heldout_rows": test_index.tolist(),
              "training_groups": sorted({rows[int(i)]["group_id"] for i in train_index}),
              "heldout_groups": sorted({rows[int(i)]["group_id"] for i in test_index}),
              "training_groups_per_class": {c: len({r["group_id"] for r in training_rows if r["label"] == c}) for c in LABELS},
              "source_manifest": [{k: r[k] for k in ("capture_sha256", "raw_window_sha256", "source_session_id", "group_id", "run_id", "label", "first_index", "last_index")} for r in rows],
              "heldout": evaluate(rows, test_index, model)}
    return model, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("features", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new output directory")
    parser.add_argument("--id", default="rig_linear_v1")
    parser.add_argument("--seed", type=int, default=20261030)
    parser.add_argument("--iterations", type=int, default=1500)
    args = parser.parse_args()
    require(not args.output.exists(), "output directory already exists; previous models are never overwritten")
    require(args.features.stat().st_size <= 128 * 1024**2, "input exceeds 128 MiB")
    payload = args.features.read_bytes()
    rows = [json.loads(line) for line in payload.decode("utf-8-sig").splitlines() if line.strip()]
    model, report = train(rows, args.id, args.seed, args.iterations)
    encoded = model_text(model).encode("utf-8")
    report["input_sha256"] = hashlib.sha256(payload).hexdigest()
    report["model_sha256"] = hashlib.sha256(encoded).hexdigest()
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "candidate.model").write_bytes(encoded)
    (args.output / "training_report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"model": str(args.output / "candidate.model"), "status": "provisional",
                      "heldout_coverage": report["heldout"]["coverage"],
                      "heldout_macro_f1": report["heldout"]["macro_f1_including_rejections"]}))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, TypeError) as exc:
        raise SystemExit("Training refused: " + str(exc))
