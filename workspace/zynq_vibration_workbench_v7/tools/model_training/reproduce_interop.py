"""mathematical_fixture_only: Python-trained weights versus frozen C++ inference.

This additional QA tool does not modify CMake, production code, the 52-test
suite, any capture, or the board. It links existing frozen static libraries.
No mechanical accuracy or measured machine data is produced by this test.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path, PureWindowsPath
import subprocess
import sys
sys.dont_write_bytecode = True
import numpy as np
import train_model as training
from test_train_model import fixtures

SCOPE = "mathematical_fixture_only"
TOLERANCE = 1e-8
ROOT = Path(__file__).resolve().parents[2]
WSL_DISTRIBUTION = None


def execution_path(path):
    text = str(path)
    if not WSL_DISTRIBUTION or text.startswith("/") or ("/" not in text and "\\" not in text):
        return text
    parsed = PureWindowsPath(text)
    if parsed.drive.lower().startswith("\\\\wsl.localhost\\"):
        require(parsed.drive.split("\\")[-1] == WSL_DISTRIBUTION, "build path belongs to another WSL distribution")
        return "/" + "/".join(parsed.parts[1:])
    require(len(parsed.drive) == 2 and parsed.drive[1] == ":", "absolute Windows path required for WSL execution")
    return "/mnt/" + parsed.drive[0].lower() + "/" + "/".join(parsed.parts[1:])


def executable_command(command):
    return (["wsl.exe", "-d", WSL_DISTRIBUTION, "--"] if WSL_DISTRIBUTION else []) + command


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def frozen_sources():
    result = {}
    for folder in ("software/core", "software/workbench", "software/accelerator"):
        for path in sorted((ROOT / folder).rglob("*")):
            if path.is_file() and path.suffix in (".cpp", ".hpp", ".h", ".txt", ".py"):
                result[path.relative_to(ROOT).as_posix()] = digest(path)
    return result


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def cases_for(rows, model):
    cases = []

    def add(name, features, mode="valid", rpm_min=1499., rpm_max=1501., cohort="scored"):
        cases.append(dict(scope=SCOPE, case_id=name, mode=mode, features=list(map(float, features)),
                          rpm_min=float(rpm_min), rpm_max=float(rpm_max), cohort=cohort))

    for index, row in enumerate(rows):
        add(f"fixture_{index:03d}_{row['label']}", row["features"])
    add("center_low_score_unknown", model["mean"])
    add("high_score_but_ood_unknown", np.asarray(model["mean"]) + 50*np.asarray(model["scale"]))
    for feature in range(36):
        vector = np.asarray(model["mean"]).copy()
        vector[feature] += 2*model["scale"][feature]
        add(f"coordinate_{feature:02d}", vector)
    # Inclusive operating-domain endpoints, followed by separate early gates.
    add("rpm_inclusive_bounds", rows[0]["features"], rpm_min=model["rpm_min"], rpm_max=model["rpm_max"])
    add("rpm_below_domain", rows[0]["features"], rpm_min=model["rpm_min"]-1, cohort="early_gate")
    add("rpm_above_domain", rows[0]["features"], rpm_max=model["rpm_max"]+1, cohort="early_gate")
    for mode in ("quality_invalid", "setup_mismatch", "configuration_mismatch", "fs_mismatch"):
        add(mode, rows[0]["features"], mode=mode, cohort="early_gate")
    return cases


def expectations(cases, model):
    expected = []
    for case in cases:
        raw, label, status, latent_scores = training.predict(case, model)
        if case["mode"] == "quality_invalid":
            label, status = "unknown", "quality_gated"
        elif case["mode"] in ("setup_mismatch", "configuration_mismatch", "fs_mismatch"):
            label, status = "unknown", "configuration_mismatch"
        early = case["cohort"] == "early_gate"
        expected.append(dict(scope=SCOPE, case_id=case["case_id"], cohort=case["cohort"],
                             classification=label, status=status,
                             scores=[0.]*4 if early else latent_scores,
                             numpy_latent_scores_not_executed_by_cpp=latent_scores if early else None,
                             comparison="C++ early-return contract; no scoring executed" if early else "NumPy predict four scores",
                             validated=False))
    require(next(r for r in expected if r["case_id"] == "center_low_score_unknown")["status"] == "rejected_unknown", "center must exercise low-score rejection")
    require(next(r for r in expected if r["case_id"] == "high_score_but_ood_unknown")["status"] == "rejected_unknown", "OOD must exercise unknown rejection")
    require(set(training.LABELS) <= {r["classification"] for r in expected}, "four output branches must be exercised")
    return expected


def compare(expected_path, actual_path, model_hash=None):
    expected = [json.loads(line) for line in Path(expected_path).read_text(encoding="utf-8").splitlines() if line.strip()]
    actual = [json.loads(line) for line in Path(actual_path).read_text(encoding="utf-8").splitlines() if line.strip()]
    require(len(expected) == len(actual), "C++ row count differs")
    details = []
    max_error = 0.
    for reference, observed in zip(expected, actual):
        require(reference["case_id"] == observed.get("case_id"), "C++ case order differs")
        require(observed.get("scope") == SCOPE, "scope missing from C++ output")
        require(observed.get("loaded_nonzero_weights") == 144, "actual C++ model did not load 144 nonzero weights")
        if model_hash:
            require(observed.get("model_sha256") == model_hash, "C++ loaded a different model file")
        result = observed["inference"]
        require(result["validated"] is False, "fixture cannot claim validation")
        require(result["classification"] == reference["classification"], f"label mismatch {reference['case_id']}")
        require(result["status"] == reference["status"], f"status mismatch {reference['case_id']}")
        scores = np.asarray(result["scores"], dtype=np.float64)
        require(scores.shape == (4,) and np.isfinite(scores).all(), "invalid C++ scores")
        error = float(np.max(np.abs(scores - np.asarray(reference["scores"]))))
        require(error <= TOLERANCE, f"score error {reference['case_id']}: {error}")
        if reference["cohort"] == "early_gate":
            require(np.count_nonzero(scores) == 0, "early gate must not execute scoring")
        else:
            max_error = max(max_error, error)
        details.append(dict(case_id=reference["case_id"], cohort=reference["cohort"],
                            status=result["status"], classification=result["classification"], max_abs_score_error=error))
    lookup = {r["case_id"]: r["inference"]["scores"] for r in actual}
    center = np.asarray(lookup["center_low_score_unknown"])
    changes = [float(np.max(np.abs(np.asarray(lookup[f"coordinate_{i:02d}"]) - center))) for i in range(36)]
    require(min(changes) > TOLERANCE, "at least one feature coordinate had no executed weight effect")
    return dict(scope=SCOPE, passed=True, total_cases=len(details),
                numpy_scored_cases=sum(d["cohort"] == "scored" for d in details),
                early_return_cases=sum(d["cohort"] == "early_gate" for d in details),
                absolute_score_tolerance=TOLERANCE, max_abs_score_error=max_error,
                all_36_coordinates_change_scores=True, minimum_coordinate_score_change=min(changes),
                output_labels_exercised=sorted({d["classification"] for d in details}),
                statuses_exercised=sorted({d["status"] for d in details}),
                mechanical_accuracy_evaluated=False, details=details)


def compile_probe(cxx, build, destination):
    build = Path(build)
    libraries = [build / "software/core/libvibration_core.a", build / "software/accelerator/libvibration_accelerator.a"]
    for library in libraries:
        require(library.is_file(), f"frozen library missing: {library}")
    command = executable_command([execution_path(cxx), "-std=c++17", "-O2", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
               "-I"+execution_path(ROOT / "software/core/include"), "-I"+execution_path(ROOT / "software/accelerator/include"),
               execution_path(Path(__file__).with_name("interop_probe.cpp")), *map(execution_path, libraries), "-pthread", "-o", execution_path(destination)])
    completed = subprocess.run(command, text=True, capture_output=True, encoding="utf-8", errors="replace")
    require(completed.returncode == 0, "QA probe compilation failed: "+completed.stderr)
    return dict(command=command, binary_sha256=digest(destination),
                frozen_library_sha256={str(p): digest(p) for p in libraries}, compiler_output=completed.stdout+completed.stderr)


def build(args):
    output = args.output.resolve()
    require(not output.exists(), "new output directory required; prior QA evidence is never overwritten")
    before = frozen_sources()
    rows = fixtures()
    model, training_report = training.train(rows, "mathematical_fixture_only_python_cpp_v1", iterations=1500)
    weights = np.asarray(model["weights"])
    require(weights.size == 144 and np.count_nonzero(weights) == 144, "training must yield 144 genuine nonzero weights")
    output.mkdir(parents=True)
    model_path = output / "candidate.model"
    model_path.write_text(training.model_text(model), encoding="utf-8", newline="\n")
    cases = cases_for(rows, model)
    expected = expectations(cases, model)
    expected_path = output / "expected.jsonl"
    expected_path.write_text("".join(json.dumps(r, ensure_ascii=False, allow_nan=False)+"\n" for r in expected), encoding="utf-8")
    vector_path = output / "vectors.txt"
    vector_path.write_text("# mathematical_fixture_only; case_id mode rpm_min rpm_max then 36 floats\n"+
                           "".join(" ".join([c["case_id"], c["mode"], format(c["rpm_min"], ".17g"), format(c["rpm_max"], ".17g"),
                                             *(format(v, ".17g") for v in c["features"])])+"\n" for c in cases), encoding="utf-8")
    host = output / "interop_probe_host"
    host_build = compile_probe(args.host_cxx, args.host_build, host)
    completed = subprocess.run(executable_command([execution_path(host), execution_path(model_path), execution_path(vector_path)]),
                               text=True, capture_output=True, encoding="utf-8", errors="replace")
    require(completed.returncode == 0, "actual C++ host execution failed: "+completed.stderr)
    actual_path = output / "host-results.jsonl"
    actual_path.write_text(completed.stdout, encoding="utf-8")
    report = compare(expected_path, actual_path, digest(model_path))
    report["host_build"] = host_build
    report["python_version"] = sys.version
    report["numpy_version"] = np.__version__
    if args.arm_cxx:
        require(args.arm_build, "--arm-build required with --arm-cxx")
        report["arm_build"] = compile_probe(args.arm_cxx, args.arm_build, output / "interop_probe_arm")
        report["arm_execution_performed_by_this_script"] = False
    after = frozen_sources()
    require(before == after, "production/frozen suite sources changed during standalone QA")
    recipe = dict(scope=SCOPE, fixture_factory="test_train_model.fixtures()", fixture_seed=8, training_seed=20261030,
                  iterations=1500, model_kind=model["kind"], nonzero_weights=144,
                  minimum_abs_weight=float(np.min(np.abs(weights))),
                  training_rows=training_report["training_rows"], heldout_rows=training_report["heldout_rows"],
                  training_groups=training_report["training_groups"], heldout_groups=training_report["heldout_groups"],
                  mechanical_accuracy_evaluated=False,
                  note="Fixture internals deliberately construct training-shaped metadata for software tests; these are not machine records or physical class labels.")
    write_json(output / "training-recipe.json", recipe)
    report["model_sha256"] = digest(model_path)
    report["model_nonzero_weights"] = 144
    report["frozen_source_sha256"] = before
    report["frozen_production_and_52_suite_sources_unchanged"] = True
    report["python_tool_sha256"] = {str(p.relative_to(ROOT)): digest(p) for p in (Path(__file__), Path(training.__file__), Path(__file__).with_name("test_train_model.py"), Path(__file__).with_name("interop_probe.cpp"))}
    write_json(output / "interop-report.json", report)
    (output / "README.txt").write_text(
        "mathematical_fixture_only\nThis package demonstrates Python training -> serialized 144 nonzero weights -> frozen C++ loader -> numeric inference.\n"
        "It contains no measured machine data and evaluates no mechanical accuracy. Do not use this QA model for machine diagnosis.\n"
        "Host scores are compared to NumPy predict with absolute tolerance 1e-8. Early gates are tested against the C++ zero-score contract; Python latent scores are retained in expected.jsonl and are not falsely compared as executed scores.\n"
        "Board command (run only by the authorized board operator):\n"
        "  ./interop_probe_arm candidate.model vectors.txt > arm-results.jsonl\n"
        "Then verify on the host:\n"
        "  python -B tools/model_training/reproduce_interop.py check --expected <package>/expected.jsonl --actual <package>/arm-results.jsonl --model <package>/candidate.model --report <package>/arm-interop-report.json\n",
        encoding="utf-8")
    print(json.dumps({k: report[k] for k in ("scope", "passed", "total_cases", "numpy_scored_cases", "early_return_cases", "max_abs_score_error", "mechanical_accuracy_evaluated")}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    make = sub.add_parser("build")
    make.add_argument("--output", type=Path, required=True)
    make.add_argument("--host-build", type=Path, required=True)
    make.add_argument("--host-cxx", default="g++")
    make.add_argument("--arm-build", type=Path)
    make.add_argument("--arm-cxx")
    make.add_argument("--wsl-distribution", help="Use installed Windows NumPy and frozen libraries in this WSL distribution; no package installation")
    check = sub.add_parser("check")
    check.add_argument("--expected", type=Path, required=True)
    check.add_argument("--actual", type=Path, required=True)
    check.add_argument("--model", type=Path, required=True)
    check.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "build":
        global WSL_DISTRIBUTION
        WSL_DISTRIBUTION = args.wsl_distribution
        build(args)
    else:
        report = compare(args.expected, args.actual, digest(args.model))
        report["actual_sha256"] = digest(args.actual)
        report["model_sha256"] = digest(args.model)
        write_json(args.report, report)
        print(json.dumps({k: report[k] for k in ("scope", "passed", "total_cases", "max_abs_score_error", "mechanical_accuracy_evaluated")}))


if __name__ == "__main__":
    main()
