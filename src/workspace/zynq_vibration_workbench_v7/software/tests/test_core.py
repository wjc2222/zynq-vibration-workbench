#!/usr/bin/env python3
"""Protocol rejection, numerical signal and conservative-diagnosis tests."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

sys.dont_write_bytecode = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--executable", required=True)
    ap.add_argument("--generator", required=True)
    args = ap.parse_args()
    spec = importlib.util.spec_from_file_location("fixture_generator", args.generator)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    checks = 0

    def check(ok, message):
        nonlocal checks
        if not ok:
            raise AssertionError(message)
        checks += 1

    def run(path, *options, success=True):
        proc = subprocess.run([args.executable, str(path), *options],
                              capture_output=True, text=True, encoding="utf-8")
        check((proc.returncode == 0) == success,
              f"Unexpected exit {proc.returncode}: {proc.stderr}")
        return json.loads(proc.stdout) if success else proc.stderr

    def repaired(blob):
        struct.pack_into("<I", blob, 96, zlib.crc32(blob[4096:]))
        struct.pack_into("<I", blob, 100, 0)
        struct.pack_into("<I", blob, 100, zlib.crc32(blob[:128]))
        return blob

    with tempfile.TemporaryDirectory(prefix="vibration_core_test_") as td:
        root = Path(td)
        source = root / "sine.BIN"
        n, fs, peak = 65536, 50000, 0.8
        frequency = 128 * fs / n  # Bin-centred reference, exact periodic Hann calibration.
        generator.generate(source, seconds=n / fs, sample_rate=fs, profile="sine",
                           signal_hz=frequency, sine_peak_g=peak, noise_std_g=0)
        before = source.read_bytes()
        result = run(source, "--ppr", "1", "--signal", "test", "--max-hz", "1000")
        check(result["integrity"] == "passed", "Expected integrity pass")
        check(result["channel_adc_inputs"] == [1, 3, 5], "Wrong physical ADC map")
        check(abs(result["speed"]["held_mean_rpm"] - 2000) < 1e-8, "Incorrect RPM conversion")
        check(result["speed"]["ppr_source"] == "explicit_user_override", "PPR origin lost")
        check(result["amplitude_unit"] == "g_nominal_uncalibrated", "Uncalibrated unit mislabeled")
        stats = result["time_stats"][0]
        check(abs(stats["ac_rms"] - peak / math.sqrt(2)) < 0.0002, "Incorrect RMS calibration")
        check(abs(stats["pearson_kurtosis"] - 1.5) < 0.001, "Expected sine Pearson kurtosis 1.5")
        check(abs(stats["crest_factor"] - math.sqrt(2)) < 0.001, "Incorrect crest factor")
        sp = result["spectra"][0]
        k = max(range(len(sp["peak_amplitude"])), key=sp["peak_amplitude"].__getitem__)
        check(abs(sp["frequency_hz"][k] - frequency) < 1e-8, "FFT frequency incorrect")
        check(abs(sp["peak_amplitude"][k] - peak) < 0.0002, "Hann amplitude normalization incorrect")
        spectral_power = sum(sp["psd"]) * sp["bin_width_hz"]
        check(abs(spectral_power - peak * peak / 2) < 0.0002, "PSD normalization incorrect")
        check(result["diagnosis"]["classification"] == "not_classified", "Synthetic signal classified as a machine fault")
        check(result["diagnosis"]["status"] == "not_a_confirmed_vibration_record", "Signal provenance gate bypassed")
        check(len(result["waveform"]) <= 1200, "Display points exceeded memory budget")
        check(source.read_bytes() == before, "Original capture was changed")
        check(json.loads(source.with_suffix(".meta.json").read_text())["synthetic"] is True,
              "Missing synthetic provenance")

        unknown = run(source)
        check(unknown["speed"]["held_mean_rpm"] is None, "Unknown PPR must not imply RPM")
        check(unknown["order_bands"][0][0]["rms"] is None, "Unknown RPM must disable order features")
        # A manual source declaration cannot erase known synthetic provenance.
        locked = run(source, "--ppr", "1", "--signal", "vibration", "--calibrated")
        check(locked["source_metadata"]["synthetic"] is True and
              locked["configuration"]["signal_kind"] == "test_signal" and
              locked["diagnosis"]["status"] == "not_a_confirmed_vibration_record",
              "CLI promoted a labeled synthetic record to confirmed vibration")
        unconfirmed = root / "unconfirmed.BIN"
        unconfirmed.write_bytes(before)  # No source sidecar: explicit declaration is recorded as such.
        nominal = run(unconfirmed, "--ppr", "1", "--signal", "vibration")
        check(nominal["diagnosis"]["status"] == "awaiting_calibration_and_baseline", "Missing calibration gate")

        # Public paths are UTF-8; the Windows CLI must not use code-page argv.
        unicode_dir = root / "中文 数据"
        unicode_dir.mkdir()
        unicode_input = unicode_dir / "振动采集.BIN"
        unicode_output = unicode_dir / "分析结果.json"
        unicode_input.write_bytes(before)
        unicode_input.with_suffix(".meta.json").write_text('{"synthetic":true}', encoding="utf-8")
        proc = subprocess.run([args.executable, str(unicode_input), "--output", str(unicode_output)],
                              capture_output=True, text=True, encoding="utf-8")
        check(proc.returncode == 0, "Unicode input/output paths failed: " + proc.stderr)
        exported = json.loads(unicode_output.read_text(encoding="utf-8"))
        check(exported["source_path"] == str(unicode_input) and exported["source_metadata"]["synthetic"] is True,
              "Unicode source path or synthetic provenance lost on export")
        check(unicode_input.read_bytes() == before, "Unicode source capture was modified")

        # Parse top-level JSON structurally; invalid/ambiguous sidecars cannot
        # silently erase a source restriction, and nested examples are not labels.
        sidecar = unconfirmed.with_suffix(".meta.json")
        for metadata, expected_kind, expected_status in [
            ('{"example":{"synthetic":true},"synthetic":false}', "vibration_confirmed", "unconfirmed_sidecar"),
            ('{"\\u0073ynthetic":true}', "test_signal", "synthetic_sidecar"),
            ('{"synthetic":true,"synthetic":false}', "unknown", "invalid_or_unreadable_sidecar"),
            ('{"synthetic":true', "unknown", "invalid_or_unreadable_sidecar"),
        ]:
            sidecar.write_text(metadata, encoding="utf-8")
            declared_source = run(unconfirmed, "--signal", "vibration")
            check(declared_source["configuration"]["signal_kind"] == expected_kind and
                  declared_source["source_metadata"]["status"] == expected_status,
                  "Sidecar structure/validation did not preserve source restrictions")

        missing = root / "missing_speed.BIN"
        generator.generate(missing, seconds=0.2, missing_speed=True, noise_std_g=0)
        no_speed = run(missing, "--ppr", "1")
        check(no_speed["speed"]["held_mean_rpm"] is None, "Timeout was converted to zero/valid RPM")
        check(no_speed["speed"]["timeout_fraction"] == 1, "Wrong timeout fraction")

        # A one-row timeout between old fixed-stride samples must remain a
        # visible break, including its immediately adjacent valid samples.
        short_gap = bytearray(before)
        struct.pack_into("<H", short_gap, 4096 + 100 * 32 + 22, 5)
        struct.pack_into("<I", short_gap, 4096 + 100 * 32 + 24, 0)
        gap_source = root / "short_speed_gap.BIN"
        gap_source.write_bytes(repaired(short_gap))
        gap_result = run(gap_source, "--ppr", "1", "--waveform-points", "12")
        by_index = {round(point["time_s"] * fs): point for point in gap_result["speed"]["trace"]}
        check(all(index in by_index for index in (99, 100, 101)) and
              by_index[100]["rpm"] is None and by_index[99]["rpm"] is not None and
              by_index[101]["rpm"] is not None,
              "Short speed timeout disappeared during display reduction")

        # Pathological alternating valid/missing samples remain bounded. Any
        # retained finite pair must not bridge an omitted missing odd sample.
        dense_gaps = bytearray(before)
        for index in range(1, n, 2):
            struct.pack_into("<H", dense_gaps, 4096 + index * 32 + 22, 5)
            struct.pack_into("<I", dense_gaps, 4096 + index * 32 + 24, 0)
        dense_source = root / "dense_speed_gaps.BIN"
        dense_source.write_bytes(repaired(dense_gaps))
        dense_result = run(dense_source, "--ppr", "1", "--waveform-points", "12")
        trace = dense_result["speed"]["trace"]
        check(len(trace) <= 6 * 12 and dense_result["speed"]["trace_gaps_coalesced"] is True and
              all(a["rpm"] is None or b["rpm"] is None or
                  round(b["time_s"] * fs) - round(a["time_s"] * fs) <= 1
                  for a, b in zip(trace, trace[1:])),
              "Dense speed gaps exceeded the display budget or were drawn as continuous speed")

        fixed_ppr = root / "fixed_ppr.BIN"
        generator.generate(fixed_ppr, seconds=0.2, header_ppr=2, ppr=2, noise_std_g=0)
        declared = run(fixed_ppr)
        check(abs(declared["speed"]["held_mean_rpm"] - 2000) < 1e-8, "Header PPR ignored")
        run(fixed_ppr, "--ppr", "1", success=False)

        cases = []
        b = bytearray(before); b[84] ^= 1; cases.append(("bad_header_crc", b, "Header CRC"))
        b = bytearray(before); b[4096 + 16] ^= 1; cases.append(("bad_payload_crc", b, "Payload CRC"))
        cases.append(("truncated", before[:-1], "length"))
        b = bytearray(before); struct.pack_into("<Q", b, 4096 + 32, 99)
        cases.append(("bad_index", repaired(b), "index"))
        b = bytearray(before); struct.pack_into("<Q", b, 4096 + 32 + 8, 1234)
        cases.append(("bad_tick", repaired(b), "timestamp"))
        b = bytearray(before); struct.pack_into("<H", b, 4096 + 22, 7)
        cases.append(("invalid_speed_flags", repaired(b), "SPEED"))
        b = bytearray(before); struct.pack_into("<I", b, 4096 + 24, 0)
        cases.append(("invalid_speed_period", repaired(b), "SPEED"))
        b = bytearray(before); struct.pack_into("<I", b, 80, 0x00030201)
        cases.append(("wrong_channel_map", repaired(b), "channel mapping"))
        b = bytearray(before); struct.pack_into("<I", b, 36, 0xffffffff)
        cases.append(("oversized_count", repaired(b), "record count"))
        for name, blob, expected in cases:
            p = root / (name + ".BIN")
            p.write_bytes(blob)
            error = run(p, success=False)
            check(expected.lower() in error.lower(), f"Wrong rejection for {name}: {error}")
        run(source, "--fft-size", "12345", success=False)
        run(source, "--ppr", "0", success=False)
        run(source, "--max-file-mib", "1", success=False)
        run(source, "--output", str(source), success=False)
        check(source.read_bytes() == before, "Output option overwrote input")

        # 1X band integral checked against a known bin-centred rotational sinusoid.
        order_source = root / "order.BIN"
        generator.generate(order_source, seconds=1.6384, sample_rate=40000,
                           rpm=60 * (80 * 40000 / 65536), profile="sine",
                           sine_peak_g=peak, noise_std_g=0)
        orders = run(order_source, "--ppr", "1")
        check(abs(orders["order_bands"][0][0]["rms"] - peak / math.sqrt(2)) < 0.001,
              "Measured 1X band RMS is wrong")
    print(f"PASS: {checks} protocol, numerical signal and diagnosis-boundary checks")


if __name__ == "__main__":
    main()
