#!/usr/bin/env python3
"""Calculate declared acquisition/link/storage/FFT budgets; never measure hardware."""
import math
import sys
sys.dont_write_bytecode = True
from _portable_contract import InputError, Parser, cli, fields, number, strict_load, string


SCHEMA = """Strict JSON profile (unknown/duplicate keys and booleans rejected):
  channels: positive integer
  sampling_hz: positive finite number, per channel
  bytes_per_sample: positive integer, per channel sample
  per_record_overhead_bytes: nonnegative integer, per synchronized record
  uart_baud: positive finite number, line bits/second
  uart_framing_efficiency: finite fraction in (0,1], payload bits/line bits
  storage_MBps: positive finite number, DECLARED sustained payload MB/s (decimal)
  duration_seconds: positive finite number
  analysis_decimation: positive integer
  fft_size: positive integer (no hardware radix restriction is assumed)
Optional:
  resources: {name: {used: nonnegative number, capacity: positive number,
                      limit_fraction: optional fraction (0,1], default 1}}
  signal_max_hz: positive number; when supplied, checks strictly below
                 post-decimation Nyquist. This does not verify an anti-alias filter.
All numbers finite. Record bytes = channels*bytes_per_sample + overhead;
records/s = sampling_hz. UART payload B/s = baud*efficiency/8. The caller
must account for all protocol/storage overhead in declared inputs.
FFT observation seconds = fft_size/(sampling_hz/analysis_decimation);
bin spacing Hz = (sampling_hz/analysis_decimation)/fft_size.
Checks: full raw UART capacity, storage capacity, one FFT window fits duration,
optional resources and signal band. Equality passes capacity checks, but fails
the optional Nyquist check. No FFT peak resolution or real hardware claim.
Read-only; --profile - reads stdin. stdout JSON except --help.
Exit 0: all checks pass; 1: a budget check fails; 2: invalid input/arguments.
"""


def evaluate(p):
    required = ("channels", "sampling_hz", "bytes_per_sample", "per_record_overhead_bytes",
                "uart_baud", "uart_framing_efficiency", "storage_MBps", "duration_seconds",
                "analysis_decimation", "fft_size")
    fields(p, required, ("resources", "signal_max_hz"), "profile")
    integers = {"channels", "bytes_per_sample", "per_record_overhead_bytes", "analysis_decimation", "fft_size"}
    for key in required:
        number(p[key], key, zero=key == "per_record_overhead_bytes", integer=key in integers)
    if p["uart_framing_efficiency"] > 1:
        raise InputError("uart_framing_efficiency must be at most 1")
    record_bytes = p["channels"] * p["bytes_per_sample"] + p["per_record_overhead_bytes"]
    raw = record_bytes * p["sampling_hz"]
    uart = p["uart_baud"] * p["uart_framing_efficiency"] / 8
    storage = p["storage_MBps"] * 1_000_000
    analysis_hz = p["sampling_hz"] / p["analysis_decimation"]
    if analysis_hz == 0 or raw == 0 or uart == 0:
        raise InputError("numeric input underflows calculation range")
    observation = p["fft_size"] / analysis_hz
    quantities = {"record_bytes": record_bytes, "raw_bytes_per_second": raw,
                  "raw_bits_per_second": raw * 8, "duration_payload_bytes": raw * p["duration_seconds"],
                  "uart_payload_bytes_per_second": uart, "uart_margin_bytes_per_second": uart - raw,
                  "storage_bytes_per_second": storage, "storage_margin_bytes_per_second": storage - raw,
                  "storage_capacity_to_required_ratio": storage / raw,
                  "analysis_sampling_hz": analysis_hz, "analysis_nyquist_hz": analysis_hz / 2,
                  "fft_observation_seconds": observation, "fft_bin_spacing_hz": analysis_hz / p["fft_size"]}
    checks = {"uart_full_raw_feasible": raw <= uart, "storage_full_raw_feasible": raw <= storage,
              "fft_window_fits_duration": observation <= p["duration_seconds"]}
    resources = {}
    if "resources" in p:
        if type(p["resources"]) is not dict:
            raise InputError("resources must be an object keyed by resource name")
        for name, resource in p["resources"].items():
            string(name, "resource name")
            fields(resource, ("used", "capacity"), ("limit_fraction",), "resource " + name)
            used = number(resource["used"], "resource used", zero=True)
            capacity = number(resource["capacity"], "resource capacity")
            limit = number(resource.get("limit_fraction", 1), "resource limit_fraction")
            if limit > 1:
                raise InputError("resource limit_fraction must be at most 1")
            utilization = used / capacity
            if not math.isfinite(utilization):
                raise InputError("resource utilization exceeds calculation range")
            resources[name] = {"utilization_fraction": utilization, "limit_fraction": limit,
                               "pass": utilization <= limit}
        checks["resources_within_limits"] = all(r["pass"] for r in resources.values())
    if "signal_max_hz" in p:
        signal = number(p["signal_max_hz"], "signal_max_hz")
        checks["signal_below_analysis_nyquist"] = signal < analysis_hz / 2
    if not all(math.isfinite(v) for v in quantities.values()):
        raise InputError("numeric input exceeds calculation range")
    passed = all(checks.values())
    return (0 if passed else 1), {"status": "pass" if passed else "fail",
        "schema": "engineering-budget.v1", "basis": "declared inputs only; no measured hardware bandwidth",
        "quantities": quantities, "checks": checks, "resources": resources}


def main():
    parser = Parser(description=__doc__, epilog=SCHEMA, formatter_class=__import__("argparse").RawDescriptionHelpFormatter)
    parser.add_argument("--profile", required=True, help="UTF-8 JSON file, or - for stdin")
    args = parser.parse_args()
    return evaluate(strict_load(args.profile))


if __name__ == "__main__":
    sys.exit(cli(main))
