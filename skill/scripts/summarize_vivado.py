#!/usr/bin/env python3
"""严格解析 Vivado 资源表及设计级 WNS/WHS 表；不启动 Vivado、不推算未知值。"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys


def read_report(path):
    raw = Path(path).read_bytes()
    return raw.decode("utf-8-sig"), hashlib.sha256(raw).hexdigest()


def finite_number(text):
    if not re.fullmatch(r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?", text):
        raise ValueError("无法识别的数字: " + text)
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("非有限数字")
    return value


def utilization(text):
    aliases = {"lut": {"Slice LUTs", "CLB LUTs"},
               "ff": {"Slice Registers", "CLB Registers"},
               "bram_36k_equivalent": {"Block RAM Tile"},
               "dsp": {"DSPs", "DSP48E1", "DSP48E2"}}
    found = {key: [] for key in aliases}
    supported_header = ["Site Type", "Used", "Fixed", "Prohibited", "Available", "Util%"]
    active_header = None
    for line in text.splitlines():
        stripped = line.strip()
        if re.fullmatch(r"[+\-]+", stripped):
            continue
        if not stripped.startswith("|") or not stripped.endswith("|"):
            active_header = None
            continue
        columns = [value.strip() for value in line.split("|")[1:-1]]
        if columns and columns[0] == "Site Type":
            if columns != supported_header:
                raise ValueError("不支持的资源表头/列次序，需 Site Type/Used/Fixed/Prohibited/Available/Util%")
            active_header = columns
            continue
        if len(columns) != len(supported_header):
            active_header = None
            continue
        label = columns[0].rstrip("*").strip()
        for key, names in aliases.items():
            if label in names:
                if active_header != supported_header:
                    raise ValueError("资源行缺少有效表头: " + label)
                value = finite_number(columns[1].replace(",", ""))
                if value < 0 or (key != "bram_36k_equivalent" and not value.is_integer()):
                    raise ValueError("非法资源使用量: " + label)
                found[key].append(value)
    result = {}
    for key, values in found.items():
        if not values or len(set(values)) != 1:
            raise ValueError("缺失或歧义资源字段: " + key)
        result[key] = values[0] if key == "bram_36k_equivalent" else int(values[0])
    return result


def timing(text):
    # 只接受完整标准列次序，避免把路径详情或时钟子表当成设计汇总。
    header = re.compile(r"^\s*WNS\(ns\)\s+TNS\(ns\)\s+TNS Failing Endpoints\s+TNS Total Endpoints\s+WHS\(ns\)\s+THS\(ns\)\s+THS Failing Endpoints\s+THS Total Endpoints(?:\s+WPWS\(ns\)\s+TPWS\(ns\)\s+TPWS Failing Endpoints\s+TPWS Total Endpoints)?\s*$")
    lines = text.splitlines()
    matches = []
    for index, line in enumerate(lines):
        if not header.fullmatch(line):
            continue
        expected_columns = 12 if "WPWS(ns)" in line else 8
        for row in lines[index + 1:index + 8]:
            if not row.strip() or re.fullmatch(r"[\s-]+", row):
                continue
            columns = row.split()
            if len(columns) != expected_columns:
                raise ValueError("时序汇总列数不符")
            values = [finite_number(value) for value in columns]
            for offset in (2, 3, 6, 7) + ((10, 11) if expected_columns == 12 else ()):
                if values[offset] < 0 or not values[offset].is_integer():
                    raise ValueError("非法时序端点数量")
            for failing, total in ((2, 3), (6, 7)) + (((10, 11),) if expected_columns == 12 else ()):
                if values[failing] > values[total]:
                    raise ValueError("失败端点数量大于总端点数量")
            item = {"wns_ns": values[0], "tns_ns": values[1],
                    "setup_failing_endpoints": int(values[2]),
                    "setup_total_endpoints": int(values[3]),
                    "whs_ns": values[4], "ths_ns": values[5],
                    "hold_failing_endpoints": int(values[6]),
                    "hold_total_endpoints": int(values[7])}
            if expected_columns == 12:
                item.update(wpws_ns=values[8], tpws_ns=values[9],
                            pulse_width_failing_endpoints=int(values[10]),
                            pulse_width_total_endpoints=int(values[11]))
            matches.append(item)
            break
        else:
            raise ValueError("时序表缺少数值行")
    if len(matches) != 1:
        raise ValueError("需恰好一个标准设计级时序汇总表，实际: " + str(len(matches)))
    return matches[0]


def metadata(text):
    result = {}
    for key, label in (("tool_version", "Tool Version"), ("design", "Design"),
                       ("device", "Device"), ("design_state", "Design State")):
        values = re.findall(r"^\|\s*" + label + r"\s*:\s*(.*?)\s*$", text, re.M)
        if len(set(values)) != 1 or not values[0]:
            raise ValueError("缺失或歧义报告头: " + label)
        result[key] = values[0]
    return result


def matching_devices(left, right):
    """仅归一化已有实物报告的7z完整/短器件别名；其他器件需字面一致。"""
    left, right = left.strip().lower(), right.strip().lower()
    if left == right:
        return True
    # Vivado utilization: xc7z020clg400-2; timing: 7z020-clg400.
    # 不任意移除其他系列的等级/封装字段，也不把不认识的名字猜成同器件。
    alias = re.compile(r"(?:xc)?(7z\d{3})-?([a-z]{3}\d+)(?:-([1-3]))?")
    first, second = alias.fullmatch(left), alias.fullmatch(right)
    if not first or not second or first.group(1, 2) != second.group(1, 2):
        return False
    return not (first[3] and second[3]) or first[3] == second[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--utilization", required=True)
    parser.add_argument("--timing", required=True)
    parser.add_argument("--source-kind", choices=("actual-report", "synthetic"), required=True,
                        help="调用者声明来源；不证明报告来自当前 bitstream")
    args = parser.parse_args()
    try:
        util, util_sha = read_report(args.utilization)
        times, time_sha = read_report(args.timing)
        util_meta, time_meta = metadata(util), metadata(times)
        for key in ("tool_version", "design", "design_state"):
            if util_meta[key] != time_meta[key]:
                raise ValueError("资源/时序报告身份不一致: " + key)
        if not matching_devices(util_meta["device"], time_meta["device"]):
            raise ValueError("资源/时序报告器件不一致，或属于未支持的器件别名")
        result = {"schema": "vivado-summary.v1", "parsed": True,
                  "source_kind": args.source_kind,
                  "reports": {"utilization": {"sha256": util_sha, **util_meta},
                              "timing": {"sha256": time_sha, **time_meta}},
                  "resources": utilization(util), "timing": timing(times),
                  "limitation": "仅摘录报告；不判断CDC/DRC/约束完整性，不证明上板或当前构建身份"}
        print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
        return 0
    except (OSError, UnicodeError, ValueError) as error:
        print(json.dumps({"schema": "vivado-summary.v1", "parsed": False,
                          "error": str(error)}, ensure_ascii=False, indent=2))
        return 2


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
