"""Seal cycle-accurate symmetric/direct FIR checks; no board-speed claim."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cycles(log, symmetry, cases, reads):
    text = log.read_text(errors="replace")
    assert f"FIR_UNIT_PASS cases={cases} exact_sample_reads={reads} " in text
    records = {}
    for match in re.finditer(
        r"FIR_UNIT_CYCLES profile=(\d+) kind=6 symmetric=(\d+) "
        r"fir_state_cycles=(\d+) start_to_mean_cycles=(\d+)", text
    ):
        profile, sym, fir, total = map(int, match.groups())
        assert sym == symmetry
        records[profile] = (fir, total)
    assert set(records) == {0, 1, 2, 3}
    return records


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("symmetric_work", type=Path)
    parser.add_argument("direct_work", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sym = cycles(args.symmetric_work / "fir-unit.log", 1, 28, 688128)
    direct = cycles(args.direct_work / "fir-unit.log", 0, 4, 98304)
    coeff = [int(v, 16) for v in (root / "data/fir_all.mem").read_text().split()]
    rows = []
    for p, (fs, taps) in enumerate(((50000, 641), (25000, 321), (10000, 129), (5000, 65))):
        h = coeff[p * 1024:p * 1024 + taps]
        assert h == h[::-1] and taps % 2 == 1
        old_fir, old_total = direct[p]
        new_fir, new_total = sym[p]
        assert old_fir == (taps + 2) * 8192
        assert new_fir == ((taps + 1) // 2 + 3) * 8192
        rows.append(dict(profile=p, input_rate_hz=fs, taps=taps, multiplier_lanes=3,
                         direct_mac_terms=taps, symmetric_mac_terms=(taps + 1) // 2,
                         direct_fir_state_cycles=old_fir, symmetric_fir_state_cycles=new_fir,
                         fir_cycle_reduction_percent=100 * (old_fir - new_fir) / old_fir,
                         direct_start_to_mean_cycles=old_total, symmetric_start_to_mean_cycles=new_total,
                         start_to_mean_reduction_percent=100 * (old_total - new_total) / old_total,
                         simulated_start_to_mean_ms_at_100mhz=new_total / 100000))
    files = [root / rel for rel in (
        "rtl/detection_accel.sv", "tests/fir_unit_tb.sv", "tests/fir_unit_fft_stub.sv",
        "tests/result_port_unit_tb.sv", "tests/fir_unit_generate.py", "tests/run_isolated_units.ps1",
        "data/fir_all.mem", "data/hann_8192.mem")]
    # Bind the actual copied/compiled RTL, rather than merely hashing current source.
    for work in (args.symmetric_work, args.direct_work):
        for f in files[:4]:
            assert sha256(work / f.name) == sha256(f), (work, f.name, "stale compiled source")
    bus = args.symmetric_work / "result-port-unit.log"
    assert "RESULT_PORT_UNIT_PASS transactions=37345 " in bus.read_text(errors="replace")
    args.output.mkdir(parents=True, exist_ok=True)
    logs = {}
    for name, src in (("symmetric-fir.log", args.symmetric_work / "fir-unit.log"),
                      ("direct-fir.log", args.direct_work / "fir-unit.log"),
                      ("result-port.log", bus)):
        shutil.copy2(src, args.output / name)
        logs[name] = sha256(src)
    report = dict(status="passed", scope="FIR-only RTL with explicit nonfunctional FFT stub",
                  board_verified=False, routed_timing_verified=False,
                  symmetric_cases=28, symmetric_exact_sample_checks=688128, symmetric_stat_checks=504,
                  direct_adversarial_cases=4, direct_exact_sample_checks=98304,
                  result_port_transactions=37345, coefficient_symmetry_exact=True,
                  timing_caveat="100 MHz simulated AXI testbench feeding; excludes real PS MMIO, FFT, readback, GUI and capture time",
                  profiles=rows, source_sha256={str(p.relative_to(root)).replace("\\", "/"): sha256(p) for p in files},
                  evidence_sha256=logs)
    (args.output / "fir_performance_verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "profiles": rows}, indent=2))


if __name__ == "__main__":
    main()
