#!/usr/bin/env python3
"""Audit a declared dataset inventory without reading samples or training a model."""
import argparse
import collections
import posixpath
import re
import sys
sys.dont_write_bytecode = True
from _portable_contract import InputError, Parser, cli, fields, strict_load, string


SCHEMA = """Strict JSON input (unknown/duplicate keys and invalid types rejected):
  {"rows": [{"label": "normal", "group": "session-01", "split": "train",
             "filepath": "optional/relative-or-absolute-id", "hash": "optional SHA256"}],
   "required_splits": ["train", "validation", "test"],
   "expected_labels": ["normal", "fault"]}
Only rows is required and must be a nonempty array. Each row requires label,
split and exactly ONE of group/session; group and session share one ID namespace.
Strings are nonempty, without control characters. Split must be train, validation
or test. Optional filepath is an identity string, not a file to open; backslashes
become / and redundant . segments normalize, but case is preserved. Optional hash
must be 64 hex digits (SHA256), case-insensitive; hashes are NOT recomputed.
required_splits defaults to all three, must be a nonempty unique subset.
expected_labels defaults to the union of observed labels; if supplied it must
be a nonempty unique array and labels outside it fail coverage checks.
Every expected label must appear in each required split, and each required split
must be populated. A group/session in multiple splits fails; any repeated
filepath or hash fails even within one split. Row indexes in output are zero-based.
No sample loading, model training, prediction or accuracy claims. Group IDs,
labels and identity strings are caller-provided metadata; use sanitized IDs.
Read-only; --input - reads stdin. stdout JSON except --help.
Exit 0: checks pass; 1: leakage/coverage/duplicates; 2: invalid input/arguments.
"""
SPLITS = ("train", "validation", "test")


def unique_strings(value, name):
    if type(value) is not list or not value:
        raise InputError(name + " must be a nonempty array")
    for item in value:
        string(item, name + " entry")
    if len(value) != len(set(value)):
        raise InputError(name + " must contain unique values")
    return value


def evaluate(data):
    fields(data, ("rows",), ("required_splits", "expected_labels"))
    if type(data["rows"]) is not list or not data["rows"]:
        raise InputError("rows must be a nonempty array")
    required = unique_strings(data.get("required_splits", list(SPLITS)), "required_splits")
    if not set(required) <= set(SPLITS):
        raise InputError("required_splits entries must be train, validation or test")
    groups = collections.defaultdict(lambda: collections.defaultdict(list))
    labels = {split: set() for split in SPLITS}
    counts = {split: 0 for split in SPLITS}
    identities = {"filepath": collections.defaultdict(list), "hash": collections.defaultdict(list)}
    for index, row in enumerate(data["rows"]):
        where = "row " + str(index)
        fields(row, ("label", "split"), ("group", "session", "filepath", "hash"), where)
        if ("group" in row) == ("session" in row):
            raise InputError(where + " requires exactly one of group or session")
        label = string(row["label"], where + " label")
        split = string(row["split"], where + " split")
        if split not in SPLITS:
            raise InputError(where + " split must be train, validation or test")
        group = string(row["group"] if "group" in row else row["session"], where + " group/session")
        groups[group][split].append(index)
        labels[split].add(label)
        counts[split] += 1
        if "filepath" in row:
            path = string(row["filepath"], where + " filepath").replace("\\", "/")
            identities["filepath"][posixpath.normpath(path)].append(index)
        if "hash" in row:
            digest = string(row["hash"], where + " hash")
            if not re.fullmatch(r"[a-fA-F0-9]{64}", digest):
                raise InputError(where + " hash must be SHA256 (64 hex digits)")
            identities["hash"][digest.lower()].append(index)
    observed = set().union(*labels.values())
    expected = set(unique_strings(data["expected_labels"], "expected_labels")) if "expected_labels" in data else observed
    missing = {split: sorted(expected - labels[split]) for split in required}
    unexpected = sorted(observed - expected)
    leakage = [{"group": group, "rows_by_split": dict(sorted(splits.items()))}
               for group, splits in sorted(groups.items()) if len(splits) > 1]
    duplicates = [{"identity_type": kind, "identity": key, "rows": indexes}
                  for kind, values in identities.items() for key, indexes in sorted(values.items()) if len(indexes) > 1]
    checks = {"required_splits_populated": all(counts[s] > 0 for s in required),
              "label_coverage": not unexpected and not any(missing.values()),
              "groups_disjoint_across_splits": not leakage, "identities_unique": not duplicates}
    passed = all(checks.values())
    return (0 if passed else 1), {"status": "pass" if passed else "fail", "schema": "dataset-audit.v1",
        "basis": "declared metadata only; no sample reads or model evaluation",
        "row_count": len(data["rows"]), "split_counts": counts, "expected_labels": sorted(expected),
        "missing_labels_by_required_split": missing, "unexpected_labels": unexpected,
        "group_leakage": leakage, "duplicate_identities": duplicates, "checks": checks}


def main():
    parser = Parser(description=__doc__, epilog=SCHEMA, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True, help="UTF-8 JSON inventory file, or - for stdin")
    args = parser.parse_args()
    return evaluate(strict_load(args.input))


if __name__ == "__main__":
    sys.exit(cli(main))
