"""Local-only synthetic format/navigation tests; no hardware or Qt required.

Set VIBRATION_TEST_CLI to run every receipt through the real analysis CLI.
Otherwise receipts model the existing core JSON contract for isolated tests.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest
import zlib

from local_waveform import (LocalWaveform, WaveformError, FileIdentityError,
                            capture_file_identity)

RECORD = struct.Struct('<QQhhhHII')
CLI = os.environ.get('VIBRATION_TEST_CLI')


def fixture(path, *, count=5000, rate=50000, stream=True, first=0, tick=50000000, values=None):
    data = bytearray(count * 32)
    step = 50000000 // rate
    for i in range(count):
        raw = values(i) if values else (i % 200 - 100, 700, -1500)
        RECORD.pack_into(data, i * 32, first + i if stream else i,
                         tick + i * step, *raw, 3, 1500000, i // 1500)
    header = bytearray(4096)
    header[:8] = b'CAPSTRM1' if stream else b'CAPDDR2\0'
    struct.pack_into('<14I', header, 8, 1 if stream else 2, 4096, 32, 2, rate, 50000000,
                     count, count, len(data), 0, 0, 0, 0, 1)
    struct.pack_into('<QQ', header, 64, tick, tick + (count - 1) * step)
    struct.pack_into('<IiIIII', header, 80, 0x50301, 5000000, 5, 100000000, zlib.crc32(data), 0)
    if stream:
        struct.pack_into('<Q', header, 104, first)
        header[112:128] = bytes(range(1, 17))
        struct.pack_into('<QQIII', header, 128, 7, 0, max(count, rate), 1, 4096)
    struct.pack_into('<I', header, 100, zlib.crc32(header if stream else header[:128]))
    path.write_bytes(header + data)
    identity = capture_file_identity(path)  # Same ordering required of app.py.
    if CLI:
        run = subprocess.run([CLI, str(path), '--fft-size', '1024', '--waveform-points', '1200'],
                             capture_output=True, encoding='utf-8', check=True)
        return json.loads(run.stdout), identity
    result = dict(schema='zynq_vibration.analysis.v1', source_path=str(path), integrity='passed',
                channel_adc_inputs=[1, 3, 5], amplitude_unit='g_nominal_uncalibrated',
                configuration=dict(sensor_sensitivity_v_per_g=0.5, analog_gain=-1., calibration_confirmed=False),
                header=dict(protocol_version=1 if stream else 2, sample_rate_hz=rate, clock_hz=50000000,
                    records=count, payload_bytes=len(data), first_tick=tick, last_tick=tick+(count-1)*step,
                    adc_range_uv=5000000, format='CAPSTRM1' if stream else 'CAPDDR2',
                    global_first_index=first if stream else 0, segment_index=7 if stream else 0,
                    session_total_records=0))
    return result, identity


class LocalWaveformTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='vibration_local_waveform_')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.serial = 0

    def source(self, **kwargs):
        self.serial += 1
        path = self.root / ('原始 波形%d.BIN' % self.serial)
        result, identity = fixture(path, **kwargs)
        return path, result, identity

    def provider(self, **kwargs):
        path, result, identity = self.source(**kwargs)
        provider = LocalWaveform(path, result, expected_identity=identity)
        self.addCleanup(provider.close)
        return provider

    def test_1khz_at_50k_10s_full_envelope_and_zoom_raw(self):
        values = lambda i: (round(10000*math.sin(2*math.pi*1000*i/50000)), 0, 0)
        p = self.provider(count=500000, values=values)
        started = time.monotonic()
        full = p.window(*p.domain, 1000, unit='V')
        self.assertEqual(full.mode, 'interval_envelope')
        self.assertEqual(full.sample_count, 500000)
        ch = full.series[0]
        self.assertEqual(len(ch.intervals), 1000)
        self.assertFalse(ch.points)
        self.assertTrue(all(b.minimum < -1.5 and b.maximum > 1.5 for b in ch.intervals))
        self.assertEqual(sum(b.sample_count for b in ch.intervals), 500000)
        zoom = p.window(2, 2.002, 800, unit='V')
        self.assertEqual(zoom.mode, 'raw')
        self.assertEqual(zoom.sample_count, 101)
        self.assertEqual(zoom.series[0].points[0].index, 100000)
        for point in zoom.series[0].points:
            self.assertEqual(point.time_s, point.index / 50000)
            self.assertEqual(point.value, values(point.index)[0] * 5 / 32768)
        self.assertGreater(max(s.value for s in zoom.series[0].points), 1.5)
        self.assertLess(min(s.value for s in zoom.series[0].points), -1.5)
        print('500k/3ch envelope seconds=%.3f' % (time.monotonic() - started))

    def test_single_sample_impulse_survives_each_channel(self):
        spikes = {123: (32767, 0, 0), 2549: (0, -32768, 0), 4999: (0, 0, 30000)}
        p = self.provider(values=lambda i: spikes.get(i, (0, 0, 0)))
        view = p.window(*p.domain, 17, unit='V')
        self.assertEqual(max(b.maximum for b in view.series[0].intervals), 32767*5/32768)
        self.assertEqual(min(b.minimum for b in view.series[1].intervals), -5)
        last = view.series[2].intervals[-1]
        self.assertEqual(last.maximum_index, 4999)
        self.assertEqual(last.maximum_time_s, 4999/50000)
        self.assertEqual(last.maximum, 30000*5/32768)

    def test_capddr2_raw_and_signed_analysis_conversion(self):
        p = self.provider(stream=False, count=100, values=lambda i: (-32768, 32767, i))
        view = p.window(*p.domain, 100)
        self.assertEqual(view.unit, 'g_nominal_uncalibrated')
        self.assertEqual(view.series[0].points[0].value, 10)
        self.assertEqual(view.series[1].points[0].value, -32767*10/32768)
        self.assertEqual(p.domain, (0, 99/50000))
        self.assertEqual(p.duration_s, 100/50000)
        # Negative gain reverses min/max; envelope must stay ordered.
        reduced = p.window(*p.domain, 1)
        self.assertEqual(reduced.series[2].intervals[0].minimum, -99*10/32768)
        self.assertEqual(reduced.series[2].intervals[0].maximum, 0)

    def test_global_u64_indices_ticks_and_segment_relative_time(self):
        first, tick = 2**53+111, 2**53+9000
        p = self.provider(first=first, tick=tick, count=100)
        view = p.window(0.0002, 0.0004, 100, channels=(2, 0))
        self.assertEqual([s.channel for s in view.series], [2, 0])
        sample = view.series[0].points[0]
        self.assertEqual((sample.index, sample.tick, sample.time_s), (first+10, tick+10000, 10/50000))
        self.assertEqual(view.sample_count, 11)

    def test_boundaries_clipping_empty_single_sample_and_subsample_view(self):
        p = self.provider(count=100)
        for x0, x1, first, stop in [(-1, 0, 0, 1), (99/50000, 1e100, 99, 100),
                (-1e100, -1, 0, 0), (1e100, 1e100, 100, 100),
                (0.000201, 0.000219, 11, 11), (0.0002, 0.0002, 10, 11)]:
            view = p.window(x0, x1, 10)
            self.assertEqual((view.first_record, view.stop_record), (first, stop))
        one = self.provider(count=1)
        self.assertEqual(one.domain, (0, 0))
        self.assertEqual(one.window(0, 0, 1).sample_count, 1)

    def test_time_pixel_buckets_match_all_original_extrema(self):
        p = self.provider(count=5013, first=555)
        view = p.window(0.00123, 0.09877, 37)
        self.assertEqual(view.mode, 'interval_envelope')
        for series in view.series:
            self.assertLessEqual(len(series.intervals), 37)
            for b in series.intervals:
                lo, hi = b.start_index-555, b.end_index-555+1
                original = [((i % 200-100, 700, -1500)[series.channel])*p.analysis_scale for i in range(lo, hi)]
                self.assertEqual((b.minimum, b.maximum), (min(original), max(original)))
                self.assertEqual(b.sample_count, hi-lo)
        self.assertEqual(sum(b.sample_count for b in view.series[0].intervals), view.sample_count)

    def test_pixel_boundary_pulse_is_not_dropped(self):
        p = self.provider(count=5000, values=lambda i: (30000 if i in (0, 1250, 2500, 3750, 4999) else 0, 0, 0))
        view = p.window(0, 0.1, 4, unit='V')
        self.assertEqual([b.maximum_index for b in view.series[0].intervals], [0, 1250, 2500, 3750])
        self.assertEqual(sum(b.sample_count for b in view.series[0].intervals), 5000)

    def test_unverified_wrong_path_header_units_or_missing_receipt_rejected(self):
        path, result, identity = self.source()
        for transform in (lambda r:r.update(integrity='failed'),
                          lambda r:r.update(source_path=str(path)+'.other'),
                          lambda r:r['header'].update(records=2),
                          lambda r:r.update(amplitude_unit='V'),
                          lambda r:r['configuration'].update(analog_gain=0)):
            altered = json.loads(json.dumps(result))
            transform(altered)
            with self.assertRaises(WaveformError): LocalWaveform(path, altered, expected_identity=identity)
        with self.assertRaises(FileIdentityError): LocalWaveform(path, result, expected_identity=None)

    def test_file_replaced_between_snapshot_and_open_refused(self):
        path, result, identity = self.source()
        replacement = self.root/'replacement.BIN'
        replacement.write_bytes(path.read_bytes())
        os.replace(replacement, path)
        with self.assertRaises(FileIdentityError): LocalWaveform(path, result, expected_identity=identity)

    def test_in_place_modification_and_closed_provider_refused(self):
        p = self.provider()
        with p.path.open('r+b') as handle:
            handle.seek(4096+16)
            handle.write(b'\x12\x34')
        os.utime(p.path, ns=(time.time_ns(), p.identity.modified_ns+1000000))
        with self.assertRaises(FileIdentityError): p.window(*p.domain, 100)
        p.close()
        with self.assertRaises(FileIdentityError): p.window(0, 1, 100)

    def test_open_provider_path_replacement_detected_or_denied_by_os(self):
        p = self.provider()
        replacement = self.root/'open-replacement.BIN'
        replacement.write_bytes(p.path.read_bytes())
        try:
            os.replace(replacement, p.path)
        except PermissionError:
            # Windows may deny replacement while the read-only mmap is open.
            self.assertEqual(p.window(0, 0, 1).sample_count, 1)
        else:
            with self.assertRaises(FileIdentityError): p.window(0, 0, 1)

    def test_cached_redraw_keeps_identity_guard(self):
        p = self.provider()
        first = p.window(*p.domain, 100)
        self.assertIs(p.window(*p.domain, 100), first)
        with p.path.open('r+b') as handle:
            handle.seek(4096+16)
            handle.write(b'\x34\x12')
        os.utime(p.path, ns=(time.time_ns(), p.identity.modified_ns+1000000))
        with self.assertRaises(FileIdentityError): p.window(*p.domain, 100)

    def test_invalid_header_crc_and_local_record_continuity_refused(self):
        path, result, _ = self.source()
        data = bytearray(path.read_bytes())
        data[84] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(WaveformError, 'Header CRC'): LocalWaveform(path, result, expected_identity=capture_file_identity(path))
        path, result, _ = self.source()
        data = bytearray(path.read_bytes())
        struct.pack_into('<Q', data, 4096+77*32, 999)
        path.write_bytes(data)
        # A fabricated/stale passed marker cannot bypass local record checks.
        with LocalWaveform(path, result, expected_identity=capture_file_identity(path)) as p:
            with self.assertRaisesRegex(WaveformError, 'index'): p.window(77/50000, 78/50000, 10)

    def test_invalid_queries_and_source_unchanged(self):
        p = self.provider()
        before = hashlib.sha256(p.path.read_bytes()).hexdigest()
        for args in [(math.nan, 1, 100), (1, 0, 100), (0, 1, 0), (0, 1, True), (-1e308, 1e308, 1)]:
            with self.assertRaises(WaveformError): p.window(*args)
        for kwargs in [dict(channels=(3,)), dict(channels=(0, 0)), dict(unit='g')]:
            with self.assertRaises(WaveformError): p.window(0, 1, 100, **kwargs)
        p.window(*p.domain, 100)
        self.assertEqual(hashlib.sha256(p.path.read_bytes()).hexdigest(), before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
