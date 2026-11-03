#!/usr/bin/env python3
"""在临时目录复验三个只读CLI；27项全部合成，不读取私人报告。"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, help="新 JSON 文件，不覆盖")
    args = parser.parse_args()
    destination = Path(args.output)
    if destination.exists():
        parser.error("output 已存在，请选新文件")
    skill = Path(__file__).resolve().parents[2]
    cases = []
    with tempfile.TemporaryDirectory(prefix=".runtime-", dir=Path(__file__).parent) as temporary:
        scratch = Path(temporary)
        def write(name, text):
            with (scratch / name).open("w", encoding="utf-8", newline="\n") as handle:
                handle.write(text)
        def run(name, script, arguments, expected_code, check):
            process = subprocess.run([sys.executable, "-X", "utf8", "-B", str(skill / "scripts" / script), *arguments],
                                     cwd=scratch, capture_output=True, text=True, encoding="utf-8", timeout=30)
            try:
                output = json.loads(process.stdout)
                invariant = bool(check(output))
            except (ValueError, KeyError, TypeError):
                output = {"raw_stdout": process.stdout}
                invariant = False
            def public_paths(value):
                if isinstance(value, str):
                    return value.replace(str(scratch).replace(chr(92), chr(92)*2), "<temporary-test-root>").replace(str(scratch), "<temporary-test-root>").replace(scratch.as_posix(), "<temporary-test-root>")
                if isinstance(value, dict):
                    return {key: public_paths(child) for key, child in value.items()}
                if isinstance(value, list):
                    return [public_paths(child) for child in value]
                return value
            output = public_paths(output)
            public_stderr = public_paths(process.stderr)
            cases.append({"case": name, "script": script, "expected_exit": expected_code,
                          "actual_exit": process.returncode,
                          "passed": process.returncode == expected_code and invariant,
                          "output": output, "stderr": public_stderr})
            return output
        def numeric(name, actual, expected, atol, rtol, code, check):
            write("actual.json", actual)
            write("expected.json", expected)
            return run(name, "compare_numeric.py", ["--actual", "actual.json", "--expected", "expected.json",
                       "--atol", atol, "--rtol", rtol, "--source-kind", "synthetic"], code, check)
        numeric("numeric-absolute-pass", "[0.0005, 2]", "[0, 2]", "0.001", "0", 0,
                lambda r: r["passed"] and r["compared_values"] == 2)
        numeric("numeric-relative-pass", '{"x":[1000.5]}', '{"x":[1000]}', "0", "0.001", 0,
                lambda r: r["passed"] and r["compared_values"] == 1)
        numeric("numeric-tolerance-fail", "[1002]", "[1000]", "0", "0.001", 1,
                lambda r: not r["passed"] and r["mismatches"] == 1)
        numeric("numeric-shape-fail", "[[1,2],[3]]", "[[1,2],[3,4]]", "0", "0", 1,
                lambda r: r["shape_errors"] == 1)
        numeric("numeric-nan-rejected", "[NaN]", "[0]", "0", "0", 2,
                lambda r: not r["passed"] and "error" in r)
        numeric("numeric-overflow-rejected", "[1e999]", "[0]", "0", "0", 2,
                lambda r: not r["passed"] and "error" in r)
        numeric("numeric-arithmetic-overflow-fail", "[1e308]", "[-1e308]", "0", "0", 1,
                lambda r: not r["passed"] and r["mismatches"] == 1 and r["max_absolute_error"] is None)
        numeric("numeric-bool-rejected", "[true]", "[1]", "0", "0", 2,
                lambda r: not r["passed"] and "error" in r)
        numeric("numeric-duplicate-key-rejected", '{"x":1,"x":2}', '{"x":2}', "0", "0", 2,
                lambda r: not r["passed"] and "error" in r)
        numeric("numeric-negative-tolerance-rejected", "[1]", "[1]", "-1", "0", 2,
                lambda r: not r["passed"] and "error" in r)
        header = "| Tool Version : SYNTHETIC FORMAT FIXTURE\n| Design : synthetic_design\n| Device : synthetic_device\n| Design State : Routed\n"
        resource_header = "| Site Type | Used | Fixed | Prohibited | Available | Util% |\n"
        util = header + resource_header + "| Slice LUTs | 123 | 0 | 0 | 1000 | 12.3 |\n| Slice Registers | 456 | 0 | 0 | 2000 | 22.8 |\n| Block RAM Tile | 2.5 | 0 | 0 | 10 | 25 |\n| DSPs | 3 | 0 | 0 | 10 | 30 |\n"
        timing = header + "WNS(ns) TNS(ns) TNS Failing Endpoints TNS Total Endpoints WHS(ns) THS(ns) THS Failing Endpoints THS Total Endpoints\n------- ------- ------- ------- ------- ------- ------- -------\n-0.100 -0.200 2 100 0.050 0.000 0 100\n"
        write("util.rpt", util)
        write("timing.rpt", timing)
        vivado_args = ["--utilization", "util.rpt", "--timing", "timing.rpt", "--source-kind", "synthetic"]
        run("vivado-synthetic-negative-slack-preserved", "summarize_vivado.py", vivado_args, 0,
            lambda r: r["parsed"] and r["timing"]["wns_ns"] == -0.1 and r["resources"]["bram_36k_equivalent"] == 2.5)
        write("util.rpt", header + "| Slice LUTs | 123 |\n")
        run("vivado-missing-resources-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("util.rpt", util)
        write("timing.rpt", header + "WNS=0.7 WHS=0.1\n")
        run("vivado-unknown-format-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("timing.rpt", timing.replace("synthetic_design", "other_design"))
        run("vivado-paired-design-mismatch-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("timing.rpt", timing.replace("synthetic_device", "different_device"))
        run("vivado-paired-device-mismatch-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("timing.rpt", timing)
        write("util.rpt", util.replace(resource_header, "| Site Type | Available | Used | Fixed | Prohibited | Util% |\n"))
        run("vivado-reordered-resource-columns-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("util.rpt", util.replace(resource_header, ""))
        run("vivado-missing-resource-header-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("util.rpt", util)
        write("timing.rpt", timing.replace("-0.100 -0.200 2 100", "-0.100 -0.200 101 100"))
        run("vivado-setup-failing-exceeds-total-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("timing.rpt", timing.replace("0.050 0.000 0 100", "0.050 0.000 101 100"))
        run("vivado-hold-failing-exceeds-total-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        full_timing = timing.replace("THS Total Endpoints\n", "THS Total Endpoints WPWS(ns) TPWS(ns) TPWS Failing Endpoints TPWS Total Endpoints\n")
        full_timing = full_timing.replace("-0.100 -0.200 2 100 0.050 0.000 0 100\n", "-0.100 -0.200 2 100 0.050 0.000 0 100 -0.1 -0.2 101 100\n")
        write("timing.rpt", full_timing)
        run("vivado-pulse-failing-exceeds-total-rejected", "summarize_vivado.py", vivado_args, 2,
            lambda r: not r["parsed"])
        write("payload.txt", "SYNTHETIC SHA256 fixture\n")
        manifest = run("sha-manifest-create", "project_audit.py", ["manifest", "--root", ".", "--file", "payload.txt"], 0,
                       lambda r: r["files"][0]["sha256"] == hashlib.sha256(b"SYNTHETIC SHA256 fixture\n").hexdigest())
        write("manifest.json", json.dumps(manifest))
        run("sha-verify-pass", "project_audit.py", ["verify", "--root", ".", "--manifest", "manifest.json"], 0,
            lambda r: r["passed"] and len(r["files"]) == 1)
        write("payload.txt", "SYNTHETIC changed fixture\n")
        run("sha-content-change-detected", "project_audit.py", ["verify", "--root", ".", "--manifest", "manifest.json"], 1,
            lambda r: not r["passed"])
        run("sha-traversal-rejected", "project_audit.py", ["manifest", "--root", ".", "--file", "../escape.txt"], 2,
            lambda r: not r["passed"])
        run("diagnose-selected-entry-pass", "project_audit.py", ["diagnose", "--root", ".", "--require", "payload.txt"], 0,
            lambda r: r["passed"])
        run("diagnose-missing-entry-fail", "project_audit.py", ["diagnose", "--root", ".", "--require", "missing.txt"], 1,
            lambda r: not r["passed"])
        # Public replacement for the private actual-report test; not equivalent coverage.
        timing12 = header + "WNS(ns) TNS(ns) TNS Failing Endpoints TNS Total Endpoints WHS(ns) THS(ns) THS Failing Endpoints THS Total Endpoints WPWS(ns) TPWS(ns) TPWS Failing Endpoints TPWS Total Endpoints\n-------\n0.2 0 0 100 0.1 0 0 100 0.15 0 0 100\n"
        write("util.rpt", util)
        write("timing.rpt", timing12)
        run("vivado-public-synthetic-12column-pass", "summarize_vivado.py", vivado_args, 0,
            lambda r: r["parsed"] and r["timing"]["wpws_ns"] == 0.15
            and r["timing"]["pulse_width_total_endpoints"] == 100)
    summary = {"schema": "skill-cli-validation.v1", "created_utc": datetime.now(timezone.utc).isoformat(),
               "python": sys.version.split()[0], "cases": cases, "case_count": len(cases),
               "passed_count": sum(case["passed"] for case in cases), "passed": all(case["passed"] for case in cases),
               "helper_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted((skill / "scripts").glob("*.py"))},
               "scope": "27项全部合成；26项继承合成断言，1项12列成功案例替换私人实际报告案例。临时路径已脱敏；未综合/上板/训练，不是机械准确率验证"}
    with destination.open("x", encoding="utf-8") as handle:
        json.dump(summary, handle, ensure_ascii=False, indent=2, allow_nan=False)
        handle.write("\n")
    print(json.dumps({"passed": summary["passed"], "cases": len(cases), "passed_count": summary["passed_count"]}))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())

