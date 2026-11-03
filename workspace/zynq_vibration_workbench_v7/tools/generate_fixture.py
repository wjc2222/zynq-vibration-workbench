#!/usr/bin/env python3
"""Generate synthetic CAPDDR2 v2 development fixtures, never measurement evidence.

The same-stem .meta.json sidecar declares synthetic=true. The simulated signal
uses nominal 500 mV/g sensitivity and signed analog gain -1; there is no fault
ground-truth label and these fixtures must not be used as diagnosis validation.
Python standard library only.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import struct
import zlib

RECORD = struct.Struct("<QQhhhHII")
CLOCK_HZ = 50_000_000
MAX_RECORDS = (64 * 1024 * 1024 - 4096) // 32


def generate(path, *, seconds=10.0, sample_rate=50000, rpm=2000.0,
             ppr=1, header_ppr=0, missing_speed=False, profile="demo",
             signal_hz=None, sine_peak_g=0.8, noise_std_g=0.005, seed=20261024):
    path = Path(path)
    sidecar = path.with_suffix(".meta.json")
    if path.exists() or sidecar.exists():
        raise ValueError("Refusing to overwrite an existing fixture or metadata file")
    if sample_rate <= 0 or sample_rate > 100000 or CLOCK_HZ % sample_rate:
        raise ValueError("sample_rate must divide 50 MHz and be at most 100 kSPS")
    if not math.isfinite(seconds) or seconds <= 0:
        raise ValueError("seconds must be positive and finite")
    count = round(seconds * sample_rate)
    if not 1 <= count <= MAX_RECORDS:
        raise ValueError("record count exceeds the firmware capture region")
    if not math.isfinite(rpm) or rpm <= 0 or ppr < 1 or header_ppr < 0:
        raise ValueError("RPM/PPR must be positive; header_ppr may be zero/unknown")
    if header_ppr not in (0, ppr):
        raise ValueError("header_ppr must match simulated PPR or be zero")
    if profile not in ("demo", "sine"):
        raise ValueError("profile must be demo or sine")
    fundamental = rpm / 60 if signal_hz is None else signal_hz
    if not math.isfinite(fundamental) or fundamental <= 0 or fundamental * 3 >= sample_rate / 2:
        raise ValueError("signal frequency and its third harmonic must be below Nyquist")
    if not math.isfinite(sine_peak_g) or sine_peak_g <= 0:
        raise ValueError("sine_peak_g must be positive and finite")
    if not math.isfinite(noise_std_g) or noise_std_g < 0:
        raise ValueError("noise_std_g must be nonnegative and finite")
    period = round(CLOCK_HZ / (rpm / 60 * ppr))
    if not 1 <= period <= 0xffffffff:
        raise ValueError("SPEED period cannot be represented")
    first_tick, step = CLOCK_HZ, CLOCK_HZ // sample_rate
    payload = bytearray(count * RECORD.size)
    rng = random.Random(seed)
    saturated = 0
    for i in range(count):
        t = i / sample_rate
        raw = []
        for axis, factor in enumerate((1.0, 0.75, 0.5)):
            phase = (0.0, 0.3, 0.9)[axis]
            acc = sine_peak_g * factor * math.sin(2 * math.pi * fundamental * t + phase)
            if profile == "demo":
                acc += 0.12 * factor * math.sin(4 * math.pi * fundamental * t + 0.4)
                acc += 0.04 * factor * math.sin(6 * math.pi * fundamental * t + 0.1)
            if noise_std_g:
                acc += rng.gauss(0, noise_std_g)
            voltage = -1.0 * 0.5 * acc
            code = round(voltage / 5.0 * 32768)
            if not -32768 <= code <= 32767:
                saturated += 1
            raw.append(max(-32768, min(32767, code)))
        tick = first_tick + i * step
        flags = 5 if missing_speed else 3
        edge_count = 0 if missing_speed else (tick // period) & 0xffffffff
        RECORD.pack_into(payload, i * RECORD.size, i, tick, *raw, flags,
                         0 if missing_speed else period, edge_count)
    header = bytearray(4096)
    header[:8] = b"CAPDDR2\0"
    struct.pack_into("<14I", header, 8, 2, 4096, 32, 2, sample_rate, CLOCK_HZ,
                     count, count, len(payload), 0, 0, 0, 0, header_ppr)
    struct.pack_into("<QQ", header, 64, first_tick, first_tick + (count - 1) * step)
    struct.pack_into("<IiIIII", header, 80, 0x00050301, 5_000_000, 5,
                     100_000_000, zlib.crc32(payload), 0)
    struct.pack_into("<I", header, 100, zlib.crc32(header[:128]))
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as f:
        f.write(header)
        f.write(payload)
    metadata = {
        "synthetic": True,
        "source_kind": "synthetic",
        "purpose": "software_development_only_not_hardware_or_diagnosis_validation",
        "generator": "tools/generate_fixture.py",
        "protocol": "CAPDDR2_v2",
        "profile": profile,
        "record_count": count,
        "sample_rate_hz": sample_rate,
        "duration_s": count / sample_rate,
        "simulated_rpm": rpm,
        "simulated_ppr": ppr,
        "header_ppr": header_ppr,
        "speed_missing": missing_speed,
        "fundamental_hz": fundamental,
        "sine_peak_g_ch1": sine_peak_g,
        "noise_std_g": noise_std_g,
        "sensor_sensitivity_v_per_g": 0.5,
        "analog_gain": -1.0,
        "seed": seed,
        "clamped_adc_samples": saturated,
        "fault_label": None,
        "note": "No physical machine was measured. Display demonstration only.",
        "sha256": hashlib.sha256(header + payload).hexdigest(),
    }
    with sidecar.open("x", encoding="utf-8") as f:
        json.dump(metadata, f, ensure_ascii=False, indent=2)
        f.write("\n")
    return metadata


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--sample-rate", type=int, default=50000)
    ap.add_argument("--rpm", type=float, default=2000.0)
    ap.add_argument("--ppr", type=int, default=1)
    ap.add_argument("--header-ppr", type=int, default=0)
    ap.add_argument("--missing-speed", action="store_true")
    ap.add_argument("--profile", choices=["demo", "sine"], default="demo")
    ap.add_argument("--signal-hz", type=float)
    ap.add_argument("--sine-peak-g", type=float, default=0.8)
    ap.add_argument("--noise-std-g", type=float, default=0.005)
    ap.add_argument("--seed", type=int, default=20261024)
    args = vars(ap.parse_args())
    path = args.pop("output")
    try:
        metadata = generate(path, **args)
    except (ValueError, OSError) as error:
        ap.error(str(error))
    print(f"SYNTHETIC ONLY: {path} ({metadata['record_count']} records)")
    print(f"Metadata: {path.with_suffix('.meta.json')}")


if __name__ == "__main__":
    main()
