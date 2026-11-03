#!/usr/bin/env python3
"""只读创建/核对指定文件的 SHA256 清单，或检查用户指定的工程入口。"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys


def ordinary(path):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise ValueError("拒绝符号链接/Windows重解析点: " + str(path))
    return info


def root_path(text):
    root = Path(os.path.abspath(text))
    for path in reversed((root,) + tuple(root.parents)):
        ordinary(path)
    if not root.is_dir():
        raise ValueError("root 不是现有普通目录")
    return root


def selected_file(root, text):
    relative = PurePosixPath(text)
    if "\\" in text or relative.is_absolute() or not relative.parts or ":" in text or any(
            part in ("", ".", "..") for part in text.split("/")):
        raise ValueError("清单路径须为无 . 或 .. 的相对 / 路径: " + text)
    current = root
    for part in relative.parts:
        current = current / part
        ordinary(current)
    if not current.is_file():
        raise ValueError("不是普通文件: " + text)
    if not current.resolve().is_relative_to(root.resolve()):
        raise ValueError("路径越过 root: " + text)
    return current


def record(root, text):
    path = selected_file(root, text)
    before = path.stat()
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns, before.st_ino) != (
            after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError("读取期间文件改变: " + text)
    return {"path": text, "size": after.st_size, "sha256": digest.hexdigest()}


def manifest_records(path):
    def unique(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("清单包含重复键: " + key)
            result[key] = value
        return result
    data = json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=unique)
    if not isinstance(data, dict) or data.get("schema") != "sha256-files.v1":
        raise ValueError("清单 schema 需为 sha256-files.v1")
    rows = data.get("files")
    if not isinstance(rows, list) or not rows:
        raise ValueError("清单 files 需为非空数组")
    seen = set()
    for row in rows:
        if not isinstance(row, dict) or set(row) != {"path", "size", "sha256"}:
            raise ValueError("清单行字段需为 path/size/sha256")
        if not isinstance(row["path"], str) or row["path"] in seen:
            raise ValueError("清单路径错误或重复")
        seen.add(row["path"])
        if type(row["size"]) is not int or row["size"] < 0 or not isinstance(row["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", row["sha256"]):
            raise ValueError("非法 size/SHA256")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("manifest", "verify", "diagnose"))
    parser.add_argument("--root", required=True)
    parser.add_argument("--file", action="append", default=[], help="manifest 模式指定文件，可重复")
    parser.add_argument("--manifest", help="verify 模式读取清单")
    parser.add_argument("--require", action="append", default=[], help="diagnose 模式指定入口，可重复")
    args = parser.parse_args()
    try:
        if sys.version_info < (3, 9):
            raise ValueError("需 Python 3.9 或更新版本")
        root = root_path(args.root)
        if args.mode == "manifest":
            if not args.file or args.manifest or args.require or len(args.file) != len(set(args.file)):
                raise ValueError("manifest 需至少一个不重复 --file，不接受其他模式参数")
            result = {"schema": "sha256-files.v1", "files": [record(root, name) for name in sorted(args.file)]}
            code = 0
        elif args.mode == "verify":
            if not args.manifest or args.file or args.require:
                raise ValueError("verify 仅接受 --manifest")
            rows = manifest_records(args.manifest)
            details = []
            for expected in rows:
                try:
                    actual = record(root, expected["path"])
                    details.append({"path": expected["path"], "passed": actual == expected,
                                    "expected": expected, "actual": actual})
                except (OSError, ValueError) as error:
                    details.append({"path": expected["path"], "passed": False, "error": str(error)})
            result = {"schema": "project-audit.v1", "mode": "verify", "read_only": True,
                      "passed": all(row["passed"] for row in details), "files": details,
                      "limitation": "只检查清单列出的文件；未列文件不代表已验证"}
            code = 0 if result["passed"] else 1
        else:
            if not args.require or args.file or args.manifest:
                raise ValueError("diagnose 需至少一个 --require，不接受其他模式参数")
            details = []
            for name in args.require:
                try:
                    selected_file(root, name)
                    details.append({"path": name, "passed": True})
                except (OSError, ValueError) as error:
                    details.append({"path": name, "passed": False, "error": str(error)})
            result = {"schema": "project-audit.v1", "mode": "diagnose", "read_only": True,
                      "python": sys.version.split()[0], "passed": all(row["passed"] for row in details),
                      "entries": details, "limitation": "仅证明所列文件存在且路径安全，不证明编译/板端/工具许可可用"}
            code = 0 if result["passed"] else 1
        print(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False))
        return code
    except (OSError, ValueError, TypeError) as error:
        print(json.dumps({"schema": "project-audit.v1", "passed": False, "read_only": True,
                          "error": str(error)}, ensure_ascii=False, indent=2))
        return 2


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    sys.exit(main())
