#!/usr/bin/env python3
"""Compare every analysis field from a complete UART JSON block with a host run."""
import argparse
import hashlib
import json
import math
import pathlib
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("reference", type=pathlib.Path)
parser.add_argument("uart_log", type=pathlib.Path)
parser.add_argument("--kind", choices=("synthetic", "speed-only"), required=True)
parser.add_argument("--rtol", type=float, default=1e-7)
parser.add_argument("--atol", type=float, default=1e-9)
parser.add_argument("--output", type=pathlib.Path)
args = parser.parse_args()
if not all(math.isfinite(v) and v >= 0 for v in (args.rtol, args.atol)):
    parser.error("Tolerances must be finite and nonnegative")
for protected in (args.reference, args.uart_log):
    if args.output and (args.output.resolve() == protected.resolve() or
                        (args.output.exists() and protected.exists() and args.output.samefile(protected))):
        parser.error("Output must not overwrite an input")

reference_bytes = args.reference.read_bytes()
uart_bytes = args.uart_log.read_bytes()
lines = uart_bytes.decode("utf-8", errors="strict").splitlines()
prefix = "AX7Z020_SPEED_ONLY_ANALYSIS_JSON" if args.kind == "speed-only" else "AX7Z020_ANALYSIS_JSON"
begins = [i for i, line in enumerate(lines) if line.strip() == prefix + "_BEGIN"]
ends = [i for i, line in enumerate(lines) if line.strip() == prefix + "_END"]
if len(begins) != 1 or len(ends) != 1 or ends[0] <= begins[0]:
    raise RuntimeError("Expected exactly one complete JSON block for the selected kind")
arm_json = "\n".join(lines[begins[0] + 1:ends[0]])
def reject_nonfinite(value):
    raise ValueError(f"Non-finite JSON constant: {value}")
reference = json.loads(reference_bytes, parse_constant=reject_nonfinite)
actual = json.loads(arm_json, parse_constant=reject_nonfinite)
differences = []
counts = {"numeric_values": 0, "exact_values": 0, "ignored_source_paths": 0, "mismatches": 0}

def mismatch(path, expected, observed):
    counts["mismatches"] += 1
    if len(differences) < 20:
        differences.append({"path": path, "expected": expected, "actual": observed})

def compare(expected, observed, path="$"):
    if path == "$.source_path":
        counts["ignored_source_paths"] += 1
    elif isinstance(expected, dict) and isinstance(observed, dict):
        if expected.keys() != observed.keys():
            mismatch(path + ".keys", sorted(expected), sorted(observed))
        for key in sorted(expected.keys() & observed.keys()):
            compare(expected[key], observed[key], path + "." + key)
    elif isinstance(expected, list) and isinstance(observed, list):
        if len(expected) != len(observed):
            mismatch(path + ".length", len(expected), len(observed))
        for index, (left, right) in enumerate(zip(expected, observed)):
            compare(left, right, f"{path}[{index}]")
    elif type(expected) is int and type(observed) is int:
        counts["numeric_values"] += 1
        if expected != observed:
            mismatch(path, expected, observed)
    elif type(expected) in (int, float) and type(observed) in (int, float):
        counts["numeric_values"] += 1
        if not math.isfinite(expected) or not math.isfinite(observed) or not math.isclose(expected, observed, rel_tol=args.rtol, abs_tol=args.atol):
            mismatch(path, expected, observed)
    else:
        counts["exact_values"] += 1
        if type(expected) is not type(observed) or expected != observed:
            mismatch(path, expected, observed)

compare(reference, actual)
result = {
    "passed": counts["mismatches"] == 0,
    "kind": args.kind,
    "scope": "Complete parsed JSON comparison; only source_path differs by platform. This does not establish fault truth or accuracy.",
    "relative_tolerance": args.rtol, "absolute_tolerance": args.atol,
    "reference_sha256": hashlib.sha256(reference_bytes).hexdigest(),
    "uart_log_sha256": hashlib.sha256(uart_bytes).hexdigest(),
    "extracted_arm_json_sha256": hashlib.sha256(arm_json.encode()).hexdigest(),
    **counts, "first_differences": differences,
}
rendered = json.dumps(result, indent=2) + "\n"
if args.output:
    args.output.write_text(rendered, encoding="utf-8")
print(rendered, end="")
sys.exit(0 if result["passed"] else 1)
