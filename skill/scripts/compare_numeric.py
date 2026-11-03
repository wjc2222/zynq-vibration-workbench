#!/usr/bin/env python3
"""比较两个仅含数值叶子的 JSON 树；只读输入，JSON 输出到 stdout。"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys


def read_numeric(path):
    raw = Path(path).read_bytes()
    def reject(value):
        raise ValueError("不允许非有限常量: " + value)
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("重复 JSON 键: " + key)
            result[key] = value
        return result
    data = json.loads(raw.decode("utf-8-sig"), parse_constant=reject,
                      object_pairs_hook=pairs)
    def validate(value, location):
        if isinstance(value, dict):
            if not value:
                raise ValueError(location + ": 空对象无数值可比")
            for key, child in value.items():
                validate(child, location + "/" + key)
        elif isinstance(value, list):
            if not value:
                raise ValueError(location + ": 空数组无数值可比")
            for index, child in enumerate(value):
                validate(child, location + "/" + str(index))
        elif type(value) not in (int, float) or not math.isfinite(float(value)):
            raise ValueError(location + ": 叶子必须是有限 binary64 数值")
    validate(data, "$")
    return data, hashlib.sha256(raw).hexdigest()


def compare(actual, expected, atol, rtol, max_details):
    result = {"compared_values": 0, "mismatches": 0, "shape_errors": 0,
              "max_absolute_error": 0.0, "details": []}
    def issue(location, kind, **detail):
        result["shape_errors" if kind == "shape" else "mismatches"] += 1
        if len(result["details"]) < max_details:
            result["details"].append({"path": location, "kind": kind, **detail})
    def walk(a, e, location):
        if isinstance(e, dict):
            if not isinstance(a, dict) or set(a) != set(e):
                issue(location, "shape", reason="对象键集合或节点类型不同")
                return
            for key in sorted(e):
                walk(a[key], e[key], location + "/" + key.replace("~", "~0").replace("/", "~1"))
        elif isinstance(e, list):
            if not isinstance(a, list) or len(a) != len(e):
                issue(location, "shape", reason="数组长度或节点类型不同")
                return
            for index, (av, ev) in enumerate(zip(a, e)):
                walk(av, ev, location + "/" + str(index))
        elif isinstance(a, (dict, list)):
            issue(location, "shape", reason="数值叶子与容器不同")
        else:
            result["compared_values"] += 1
            delta = abs(float(a) - float(e))
            bound = atol + rtol * abs(float(e))
            if not math.isfinite(delta) or not math.isfinite(bound):
                result["max_absolute_error"] = None
                issue(location, "numeric", reason="误差或容差计算溢出", actual=a, expected=e)
            else:
                if result["max_absolute_error"] is not None:
                    result["max_absolute_error"] = max(result["max_absolute_error"], delta)
                if delta > bound:
                    issue(location, "numeric", actual=a, expected=e,
                          absolute_error=delta, allowed_error=bound)
    walk(actual, expected, "$")
    result["passed"] = result["mismatches"] == 0 and result["shape_errors"] == 0
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--actual", required=True)
    parser.add_argument("--expected", required=True)
    parser.add_argument("--atol", type=float, required=True)
    parser.add_argument("--rtol", type=float, required=True)
    parser.add_argument("--source-kind", choices=("synthetic", "measured", "mixed", "unknown"), required=True,
                        help="来源声明，不由脚本推断真实性")
    parser.add_argument("--max-details", type=int, default=20)
    args = parser.parse_args()
    try:
        if any(not math.isfinite(x) or x < 0 for x in (args.atol, args.rtol)):
            raise ValueError("atol/rtol 必须是有限非负数")
        if args.max_details < 1:
            raise ValueError("max-details 必须大于零")
        a, a_sha = read_numeric(args.actual)
        e, e_sha = read_numeric(args.expected)
        result = compare(a, e, args.atol, args.rtol, args.max_details)
        result.update(schema="numeric-comparison.v1", source_kind=args.source_kind,
                      actual_sha256=a_sha, expected_sha256=e_sha,
                      atol=args.atol, rtol=args.rtol,
                      rule="abs(actual-expected) <= atol + rtol*abs(expected)")
        print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
        return 0 if result["passed"] else 1
    except (OSError, ValueError, OverflowError, RecursionError) as error:
        print(json.dumps({"schema": "numeric-comparison.v1", "passed": False,
                          "error": str(error)}, ensure_ascii=False, indent=2))
        return 2


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
