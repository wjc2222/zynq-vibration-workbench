"""Offscreen Qt + actual CLI fidelity integration; all input is synthetic.

Run with --analyzer ABS_EXE --output NEW_DIRECTORY. No COM, bridge, sensors or
native desktop automation is used. PNGs are retained for human visual review.
"""
from __future__ import annotations

import argparse
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import zlib

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
sys.dont_write_bytecode = True
from PySide6.QtCore import Qt, QCoreApplication, QEvent, QPoint, QPointF
from PySide6.QtGui import QWheelEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QMenu
from shiboken6 import isValid

from app import Window, configure_application
from local_waveform import LocalWaveform, capture_file_identity
from plots import Plot
from preview_payload import decode_detail, decode_envelope, decode_spectrum
from protocol import ProtocolError

APP = None
ARGS = None
OUTPUT = None
FIXTURE = None
PULSE = None
RESULT = None
MEASURED = {}
RECORD = struct.Struct('<QQhhhHII')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_synthetic(path, count, raw_values, *, description):
    """Exact documented CAPSTRM1 layout; synthetic format flag cannot be erased."""
    rate, clock, first_tick = 50000, 50000000, 50000000
    period = round(clock / (1480 / 60))
    payload = bytearray(count * RECORD.size)
    for i in range(count):
        tick = first_tick + i * 1000
        RECORD.pack_into(payload, i * 32, i, tick, *raw_values(i), 3, period, tick // period)
    header = bytearray(4096)
    header[:8] = b'CAPSTRM1'
    struct.pack_into('<14I', header, 8, 1, 4096, 32, 2, rate, clock,
                     count, count, len(payload), 0, 6, 0x1002, 0, 1)
    struct.pack_into('<QQ', header, 64, first_tick, first_tick + (count - 1) * 1000)
    struct.pack_into('<IiIIII', header, 80, 0x50301, 5000000, 5, 100000000, zlib.crc32(payload), 0)
    struct.pack_into('<Q', header, 104, 0)
    header[112:128] = bytes.fromhex('31e1b78268954f9fbc61f6556f0c4512')
    struct.pack_into('<QQIII', header, 128, 0, count, count, 1, 4096)
    struct.pack_into('<I', header, 100, zlib.crc32(header))
    with path.open('xb') as handle:
        handle.write(header)
        handle.write(payload)
    metadata = dict(synthetic=True, source_kind='synthetic', purpose='software_fidelity_test_only',
                    physical_hardware=False, description=description, record_count=count,
                    sample_rate_hz=rate, channel_adc_inputs=[1, 3, 5], adc_range_uv=5000000,
                    sensor_sensitivity_v_per_g=.5, analog_gain=-1, calibration_confirmed=False,
                    sha256=digest(path))
    path.with_suffix('.meta.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding='utf-8')
    return metadata


def live_payload():
    count, rate = 512, 50000
    origin = 2**53 + 333
    offset = 12500 - count + 1  # Last sample exactly at .25s; 511/fs span.
    raw = bytearray()
    for i in range(count):
        raw.extend(struct.pack('<hhh', *(round(2*32768/5*factor*math.sin(2*math.pi*1000*(offset+i)/rate+phase))
            for factor, phase in ((1, 0), (.65, .4), (.35, .8)))))
    detail = dict(schema='vibration.waveform.raw.v1', encoding='s16le-interleaved-base64',
                  sample_count=count, sample_rate_hz=rate, first_index=str(origin+offset),
                  time_origin_index=str(origin), adc_range_uv=5000000,
                  raw_base64=base64.b64encode(raw).decode())
    envelope = dict(schema='vibration.waveform.envelope.v1', unit='V', start_s=0., end_s=.25,
                    channels=[[[i*.25/32, (i+1)*.25/32, -2*factor, 2*factor] for i in range(32)]
                              for factor in (1, .65, .35)])
    return dict(sample_rate_hz=rate, generation='synthetic-fidelity', revision=5,
                start_s=0., end_s=.25, waveform_detail=detail, waveform_envelope=envelope,
                speed=[(i*.25/50, 1480.) for i in range(51)], mean_rpm=1480.,
                amplitude_stats=dict(scope='raw_window', unit='V', input_revision=5,
                    start_s=0., end_s=.25, derived_before_display_decimation=True,
                    channels=[dict(channel=i+1, sample_count=12501, rms=2*f/math.sqrt(2), peak_to_peak=4*f)
                              for i, f in enumerate((1, .65, .35))]), spectrum=dict(available=False))


class FidelityTests(unittest.TestCase):
    def setUp(self):
        self.window = Window(ARGS.analyzer)
        self.window.setWindowTitle('合成波形保真测试 · 无硬件测量')
        self.window.resize(1280, 760)
        self.window.save_directory = OUTPUT
        self.window.show()
        QTest.qWait(30)

    def tearDown(self):
        self.window.active = False
        self.window.closing_window = True
        self.window.close()
        self.window.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        APP.processEvents()
        self.assertFalse(isValid(self.window), 'test-owned window must be destroyed')

    def wait(self, predicate, seconds=15):
        end = time.monotonic() + seconds
        while not predicate():
            if time.monotonic() > end:
                self.fail(self.window.status.text() + ' | ' + self.window.notice.text())
            QTest.qWait(15)

    def load(self, path=None):
        self.window.analyze_local(path or FIXTURE)
        self.wait(lambda: self.window.analysis_process is None)
        self.assertIsNotNone(self.window.result, self.window.notice.text())
        self.assertIsNotNone(self.window.local_waveform)
        self.assertEqual(self.window.result['integrity'], 'passed')
        self.assertTrue(self.window.result['source_metadata']['synthetic'])
        return self.window.waveforms.plot

    def grab(self, name, widget=None):
        widget = widget or self.window
        QTest.qWait(35)
        self.assertTrue(widget.grab().save(str(OUTPUT/name)))

    def wheel(self, plot, notches):
        area = plot._axis_layout()['area']
        point = area.center()
        event = QWheelEvent(point, QPointF(plot.mapToGlobal(point.toPoint())), QPoint(),
                            QPoint(0, notches*120), Qt.MouseButton.NoButton,
                            Qt.KeyboardModifier.NoModifier, Qt.ScrollPhase.NoScrollPhase, False)
        APP.sendEvent(plot, event)
        APP.processEvents()

    def zoom_raw(self, plot):
        self.wheel(plot, 31)
        layout = plot._axis_layout()
        width = layout['ranges'][1] - layout['ranges'][0]
        self.assertTrue(.005 <= width <= .011, width)
        self.assertTrue(layout['series'])
        self.assertFalse(layout['interval_series'])
        return layout

    def ticks(self, layout):
        for key in ('x_ticks', 'y_ticks'):
            labels = [label for label, _ in layout[key]]
            self.assertGreaterEqual(len(labels), 2)
            self.assertEqual(len(labels), len(set(labels)), labels)
            self.assertFalse(any('e' in s.lower() for s in labels), labels)
        self.assertNotIn('1.48e3', repr(layout))

    def test_01_actual_cli_open_full_500000_sample_envelope(self):
        plot = self.load()
        result = self.window.result
        self.assertEqual(result['header']['records'], 500000)
        self.assertEqual(result['duration_s'], 10.)
        self.assertIn('合成', self.window.source_label.text())
        self.assertEqual(self.window.waveforms.unit, 'V')
        self.assertEqual(result['amplitude_unit'], 'g_nominal_uncalibrated', 'display conversion must not rewrite analysis JSON')
        layout = plot._axis_layout()
        self.assertFalse(layout['series'], 'full overview must not be a reduced connected polyline')
        self.assertEqual(len(layout['interval_series']), 1)
        bins = layout['interval_series'][0][1]
        self.assertGreater(len(bins), 100)
        self.assertTrue(all(b[2] < -1.99 and b[3] > 1.99 for b in bins))
        self.ticks(layout)
        with LocalWaveform(FIXTURE, result, expected_identity=capture_file_identity(FIXTURE)) as reader:
            volts = reader.window(.001, .002, 800, channels=(0,), unit='V')
            peak = max(abs(p.value) for p in volts.series[0].points)
            self.assertAlmostEqual(peak, 2*math.sin(2*math.pi*12/50), delta=.0002)
            nominal = reader.window(.001, .002, 800, channels=(0,), unit='analysis')
            self.assertAlmostEqual(max(abs(p.value) for p in nominal.series[0].points), peak*2, delta=1e-12)
        displayed_rms = float(self.window.waveforms.channel_readouts[0][0].text())
        self.assertAlmostEqual(displayed_rms, math.sqrt(2), delta=.005)
        MEASURED['voltage_peak_V'] = peak
        MEASURED['nominal_g_expected_peak'] = peak*2
        MEASURED['displayed_rms_V'] = displayed_rms
        MEASURED['full_overview_bins'] = len(bins)
        self.grab('01-synthetic-offline-10s-envelope.png')

    def test_02_mouse_wheel_recovers_10_cycles_and_pan_raw_samples(self):
        plot = self.load()
        layout = self.zoom_raw(plot)
        points = layout['series'][0][1]
        crossings = sum(a[1] < 0 <= b[1] for a,b in zip(points, points[1:]))
        self.assertTrue(9 <= crossings <= 11, crossings)
        self.assertTrue(450 <= len(points) <= 550, len(points))
        self.assertTrue(all(abs((b[0]-a[0])-1/50000) < 1e-14 for a,b in zip(points,points[1:])))
        self.assertTrue(layout['x_title'].startswith('时间 / ms'))
        self.ticks(layout)
        MEASURED['raw_zoom_seconds'] = layout['ranges'][1]-layout['ranges'][0]
        MEASURED['raw_zoom_sample_count'] = len(points)
        MEASURED['raw_zoom_positive_crossings'] = crossings
        self.grab('02-synthetic-offline-mouse-zoom-10ms.png')
        before = plot.viewport.x_range
        area = layout['area']
        begin = area.center().toPoint()
        end = begin + QPoint(round(area.width()*.2), 0)
        QTest.mousePress(plot, Qt.MouseButton.LeftButton, pos=begin)
        QTest.mouseMove(plot, end, 10)
        QTest.mouseRelease(plot, Qt.MouseButton.LeftButton, pos=end)
        APP.processEvents()
        self.assertNotEqual(plot.viewport.x_range, before)
        self.assertAlmostEqual(plot.viewport.x_range[1]-plot.viewport.x_range[0], before[1]-before[0])
        moved = plot._axis_layout()['series'][0][1]
        self.assertNotEqual(moved[0][0], points[0][0])
        QTest.mouseDClick(plot, Qt.MouseButton.LeftButton, pos=begin)
        APP.processEvents()
        self.assertIsNone(plot.viewport.x_range)
        self.assertTrue(plot._axis_layout()['interval_series'])
        self.assertFalse(plot._axis_layout()['series'])

    def test_03_multichannel_and_rpm_units_restore_raw_provider(self):
        plot = self.load()
        self.zoom_raw(plot)
        for box in self.window.waveforms.channel_boxes[1:]:
            QTest.mouseClick(box, Qt.MouseButton.LeftButton)
        self.assertEqual(len(plot._axis_layout()['series']), 3)
        self.grab('03-synthetic-raw-three-channels.png')
        QTest.mouseClick(self.window.waveforms.speed_box, Qt.MouseButton.LeftButton)
        layout = plot._axis_layout()
        self.assertEqual(plot.y_label, 'rpm')
        self.assertIsNone(plot.raw_provider)
        self.assertEqual(len(layout['series']), 1)
        self.assertTrue(all(1479.9 < p[1] < 1480.1 for p in self.window.waveforms.speed_data))
        self.ticks(layout)
        self.grab('04-synthetic-rpm-separate-axis.png')
        QTest.mouseClick(self.window.waveforms.speed_box, Qt.MouseButton.LeftButton)
        self.assertEqual(plot.y_label, 'V')
        self.assertIsNotNone(plot.raw_provider)
        self.assertEqual(len(plot._axis_layout()['series']), 3)

    def test_04_fft_1khz_peak_and_hann_amplitude(self):
        self.load()
        channel = self.window.result['spectra'][0]
        k = max(range(len(channel['peak_amplitude'])), key=channel['peak_amplitude'].__getitem__)
        frequency = channel['frequency_hz'][k]
        peak = channel['peak_amplitude'][k]
        self.assertLessEqual(abs(frequency-1000), channel['bin_width_hz']/2+1e-6)
        # Exactly 1 kHz is not a power-of-two FFT bin at 50 kSPS. Check the
        # periodic-Hann scalloping response, rather than incorrectly demanding
        # that a single off-bin spectrum bin equal the time-domain peak 4 g.
        delta = 1000*channel['fft_size']/50000-k
        response = 1 if delta == 0 else math.sin(math.pi*delta)/(math.pi*delta)/(1-delta*delta)
        self.assertAlmostEqual(peak, 4*response, delta=.001)
        self.assertAlmostEqual(self.window.result['time_stats'][0]['rms'], 4/math.sqrt(2), delta=.0002)
        displayed_peak = self.window.spectrum_view.data[0][k][1]
        self.assertEqual(self.window.spectrum_view.unit, 'V')
        self.assertAlmostEqual(displayed_peak, 2*response, delta=.0005)
        MEASURED['fft'] = dict(peak_hz=frequency, peak_nominal_g=peak,
                              expected_hann_bin_peak_nominal_g=4*response,
                              displayed_peak_V=displayed_peak, expected_hann_bin_peak_V=2*response,
                              bin_width_hz=channel['bin_width_hz'], fft_size=channel['fft_size'])
        self.window.tabs.setCurrentIndex(1)
        self.window.spectrum_view.plot.set_x_range(950, 1050)
        self.ticks(self.window.spectrum_view.plot._axis_layout())
        self.grab('05-synthetic-fft-1khz-peak.png')

    def test_05_live_512_contiguous_samples_have_10_22ms_domain(self):
        payload = live_payload()
        raw, domain, rate = decode_detail(payload['waveform_detail'])
        self.assertEqual(rate, 50000)
        self.assertEqual(len(raw[0]), 512)
        self.assertAlmostEqual(domain[1]-domain[0], .01022, places=14)
        self.assertTrue(all(abs((b[0]-a[0])-1/50000)<1e-16 for a,b in zip(raw[0],raw[0][1:])))
        self.window.current_job = 'synthetic-display-only'
        self.window.set_display_live(True)
        self.window.render_preview(payload)
        self.window.source_label.setText('来源：合成保真测试 · 无硬件测量')
        self.window.status.setText('合成实时载荷测试 · 串口未连接')
        plot = self.window.waveforms.plot
        layout = plot._axis_layout()
        self.assertAlmostEqual(layout['ranges'][1]-layout['ranges'][0], .01022, places=14)
        self.assertIn('10.22 ms', self.window.window_label.text())
        self.assertAlmostEqual(layout['series'][0][1][-1][0]-layout['series'][0][1][0][0], .01022, places=14)
        self.ticks(layout)
        self.grab('06-synthetic-live-512-true-sample-spacing.png')
        # Exercise the actual context-menu mouse action, not a direct setter.
        errors = []
        def choose_overview():
            menu = APP.activePopupWidget()
            if not isinstance(menu, QMenu): errors.append('overview context menu absent');return
            action = next((a for a in menu.actions() if a.text() == '窗口包络概览'), None)
            if action is None: errors.append('overview action absent');menu.close();return
            QTest.mouseClick(menu, Qt.MouseButton.LeftButton, pos=menu.actionGeometry(action).center())
        from PySide6.QtCore import QTimer
        from PySide6.QtGui import QContextMenuEvent
        QTimer.singleShot(20, choose_overview)
        point = plot._axis_layout()['area'].center().toPoint()
        APP.sendEvent(plot, QContextMenuEvent(QContextMenuEvent.Reason.Mouse, point, plot.mapToGlobal(point)))
        self.assertFalse(errors, errors)
        self.assertEqual(plot.viewport.x_range, (0., .25))
        self.assertTrue(plot._axis_layout()['interval_series'])
        self.grab('07-synthetic-live-overview-intervals.png')
        QTest.mouseDClick(plot, Qt.MouseButton.LeftButton, pos=point)
        APP.processEvents()
        self.assertIsNone(plot.viewport.x_range)
        self.assertAlmostEqual(plot._axis_layout()['ranges'][1]-plot._axis_layout()['ranges'][0], .01022, places=14)
        MEASURED['live_detail'] = dict(records=512, sample_rate_hz=rate, duration_s=domain[1]-domain[0], overview_s=.25)

    def test_06_unique_nice_ticks_engineering_ms_and_mV(self):
        plot = Plot()
        try:
            plot.resize(1000, 420)
            plot.set_series([('#3569c8', [(1.47+i/50000, .002*math.sin(i*.12)) for i in range(1001)])], source_id='synthetic-small-voltage')
            plot.show()
            APP.processEvents()
            layout = plot._axis_layout()
            self.ticks(layout)
            self.assertIn('ms', layout['x_title'])
            self.assertIn('起点 1.470000 s', layout['x_title'])
            self.assertIn('mV', layout['y_title'])
            self.assertFalse(any(abs(float(label))>100 for label,_ in layout['x_ticks']))
            self.grab('08-synthetic-ms-mV-readable-ticks.png', plot)
        finally:
            plot.close();plot.deleteLater()
            QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)

    def test_07_narrow_pulse_survives_overview_and_raw_zoom(self):
        plot = self.load(PULSE)
        bins = plot._axis_layout()['interval_series'][0][1]
        self.assertGreaterEqual(max(b[3] for b in bins), 2.2)
        self.assertLessEqual(min(b[2] for b in bins), -2.2)
        self.grab('09-synthetic-single-sample-impulses-overview.png')
        plot.set_x_range(.4998, .5002)
        layout = plot._axis_layout()
        points = layout['series'][0][1]
        self.assertEqual(len(points), 21)
        self.assertGreaterEqual(max(v for _,v in points), 2.2)
        self.assertLessEqual(min(v for _,v in points), -2.2)
        self.grab('10-synthetic-single-sample-impulses-raw.png')

    def test_08_nonfinite_and_corrupt_binary_display_payloads_refused(self):
        payload = live_payload()
        for field, value in [('raw_base64', '*'+payload['waveform_detail']['raw_base64'][1:]),
                             ('sample_count', 511), ('sample_rate_hz', math.nan), ('first_index', '01')]:
            detail = copy.deepcopy(payload['waveform_detail']);detail[field]=value
            with self.assertRaises(ProtocolError): decode_detail(detail)
        for value in (math.nan, math.inf, -math.inf):
            envelope = copy.deepcopy(payload['waveform_envelope']);envelope['channels'][0][0][2]=value
            with self.assertRaises(ProtocolError): decode_envelope(envelope)
            packed = base64.b64encode(struct.pack('<ff', 0., value)).decode()
            spectrum = dict(available=True, channels=[dict(encoding='f32le-base64', bin_count=2,
                fft_size=128, bin_width_hz=390.625, first_hz=0, peak_amplitude_base64=packed)]*3)
            with self.assertRaises(ProtocolError): decode_spectrum(spectrum)
        for field, value in [('waveform_detail', dict(payload['waveform_detail'], raw_base64='not valid')),
                             ('waveform_envelope', dict(payload['waveform_envelope'], end_s=.1))]:
            bad = copy.deepcopy(payload);bad[field]=value
            with self.assertRaises(ProtocolError): self.window.render_preview(bad)

    def test_09_actual_cli_rejects_corrupt_payload_and_clears_record(self):
        self.load()
        with tempfile.TemporaryDirectory(prefix='vibration-fidelity-corrupt-') as temp:
            damaged = Path(temp)/'synthetic-corrupt.BIN'
            data = bytearray(PULSE.read_bytes());data[4096+16] ^= 1;damaged.write_bytes(data)
            self.window.analyze_local(damaged)
            self.wait(lambda:self.window.analysis_process is None)
            self.assertIsNone(self.window.result)
            self.assertIsNone(self.window.local_waveform)
            self.assertIn('CRC', self.window.notice.text())
            self.assertFalse(self.window.waveforms.plot.raw_provider)

    def test_10_unchanged_viewport_still_refuses_modified_local_file(self):
        with tempfile.TemporaryDirectory(prefix='vibration-fidelity-identity-') as temp:
            copied = Path(temp)/'synthetic-identity.BIN'
            copied.write_bytes(PULSE.read_bytes())
            copied.with_suffix('.meta.json').write_bytes(PULSE.with_suffix('.meta.json').read_bytes())
            try:
                plot = self.load(copied)
                self.assertTrue(plot._axis_layout()['has_data'])
                before = plot.viewport.x_range
                with copied.open('r+b') as handle:
                    handle.seek(4096+16);handle.write(b'\x42\x13')
                # Make timestamp change deterministic even on coarse filesystems.
                snapshot = self.window.local_waveform.identity
                os.utime(copied, ns=(time.time_ns(), snapshot.modified_ns+1000000))
                layout = plot._axis_layout()
                self.assertEqual(plot.viewport.x_range, before)
                self.assertFalse(layout['has_data'], 'same-view draw must not retain a stale provider cache')
                self.assertIn('modified', plot.last_error.lower())
            finally:
                self.window.clear_result()  # Close mmap before Windows temp cleanup.


def main():
    global APP, ARGS, OUTPUT, FIXTURE, PULSE, RESULT
    parser = argparse.ArgumentParser()
    parser.add_argument('--analyzer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    ARGS = parser.parse_args()
    OUTPUT = ARGS.output.resolve()
    OUTPUT.mkdir(parents=True, exist_ok=False)
    fixture_dir = OUTPUT/'fixtures';fixture_dir.mkdir()
    FIXTURE = fixture_dir/'synthetic-1kHz-50k-10s.BIN'
    PULSE = fixture_dir/'synthetic-single-sample-impulses.BIN'
    write_synthetic(FIXTURE, 500000,
        lambda i: tuple(round(2*32768/5*f*math.sin(2*math.pi*1000*i/50000+phase))
                        for f,phase in ((1,0),(.65,.35),(.35,.7))),
        description='Exactly 1 kHz, nominal +/-2 V CH1 at 50 kSPS for 10 s; CH2/CH3 phase/amplitude variants.')
    write_synthetic(PULSE, 50000,
        lambda i: (-15000 if i==25000 else 15000 if i==25001 else 0,0,0),
        description='Two opposite single-sample impulses, 20 us each, test display extremum preservation.')
    analysis = subprocess.run([str(ARGS.analyzer), str(FIXTURE), '--fft-size', '65536', '--max-hz', '2000'],
                              capture_output=True, encoding='utf-8', check=True)
    RESULT = json.loads(analysis.stdout)
    (OUTPUT/'fixture-analysis.json').write_text(json.dumps(RESULT, ensure_ascii=False, indent=2), encoding='utf-8')
    (OUTPUT/'live-synthetic-payload.json').write_text(json.dumps(live_payload(), indent=2), encoding='utf-8')
    sources = ['app.py','plots.py','single_waveform.py','local_waveform.py','preview_payload.py','test_waveform_fidelity.py']
    before = {p:digest(Path(__file__).parent/p) for p in sources}
    APP = QApplication.instance() or QApplication([])
    configure_application(APP)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(FidelityTests)
    with (OUTPUT/'tests.log').open('w', encoding='utf-8') as log:
        result = unittest.TextTestRunner(stream=log, verbosity=2).run(suite)
    after = {p:digest(Path(__file__).parent/p) for p in sources}
    report = dict(schema='vibration-pc-waveform-fidelity-qa-v1', outcome='passed' if result.wasSuccessful() else 'failed',
                  synthetic=True, hardware_or_COM_accessed=False, native_desktop_acceptance=False,
                  tests_run=result.testsRun, failures=[dict(test=str(t), trace=v) for t,v in result.failures],
                  errors=[dict(test=str(t), trace=v) for t,v in result.errors],
                  source_sha256_before=before, source_sha256_after=after, source_unchanged_during_test=before==after,
                  cli=dict(path=str(ARGS.analyzer.resolve()),sha256=digest(ARGS.analyzer)),
                  measured=MEASURED, screenshots_pending_human_visual_review=True)
    report['artifacts']=[dict(path=str(p.relative_to(OUTPUT)),bytes=p.stat().st_size,sha256=digest(p))
                         for p in sorted(OUTPUT.rglob('*')) if p.is_file()]
    (OUTPUT/'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(outcome=report['outcome'], tests_run=result.testsRun, output=str(OUTPUT)), ensure_ascii=False))
    return 0 if result.wasSuccessful() else 1


def load_tests(loader, tests, pattern):
    # This integration suite needs a real CLI plus a fresh evidence directory.
    # Run the documented --analyzer/--output command, not argument-free discovery.
    return tests if ARGS is not None else unittest.TestSuite()


if __name__ == '__main__':
    raise SystemExit(main())
