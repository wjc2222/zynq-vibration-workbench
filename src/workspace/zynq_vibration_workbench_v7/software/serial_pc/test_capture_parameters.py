"""Acquisition parameter UI checks; all Qt windows run on the offscreen plugin."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QDialog
from app import CaptureDialog, Window, BoardSession, capture_plan_error, configure_application

EVIDENCE = None


class CaptureParameterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.application = QApplication.instance() or QApplication([])
        configure_application(cls.application)

    def setUp(self):
        self.dialog = CaptureDialog(dict(mode="timed", sample_rate_hz=50000, duration_seconds=10, pulses_per_rev=1))
        self.dialog.show(); self.application.processEvents()

    def tearDown(self):
        self.dialog.close(); self.dialog.deleteLater(); self.application.processEvents()

    def test_all_rates_and_duration_count_boundaries(self):
        self.assertEqual((self.dialog.duration.minimum(), self.dialog.duration.maximum()), (1, 86400))
        for rate in (5000, 10000, 25000, 50000):
            self.dialog.rate.setCurrentIndex(self.dialog.rate.findData(rate))
            for seconds in (1, 40, 3600, 86400):
                with self.subTest(rate=rate, seconds=seconds):
                    self.dialog.duration.setValue(seconds)
                    self.assertEqual(int(self.dialog.samples.text()), rate * seconds)
                    self.assertTrue(self.dialog.apply_button.isEnabled())
                    other_seconds = 86400 if seconds == 1 else 1
                    self.dialog.samples.setText(str(rate * other_seconds))
                    self.assertEqual(self.dialog.duration.value(), other_seconds)
                    self.assertTrue(self.dialog.apply_button.isEnabled())
                    self.assertEqual(set(self.dialog.plan()), {"mode", "sample_rate_hz", "duration_seconds", "pulses_per_rev"})

    def test_invalid_raw_counts_survive_focus_enter_click_and_direct_accept(self):
        for raw in ("8192", "0", "-1", "2000001", "2097024", "2097025", "2147483648", "9999999999999999999999999999", "50000.0", "1e5", " 50000", "５００００", ""):
            with self.subTest(raw=raw):
                self.dialog.samples.setText(raw)
                self.assertFalse(self.dialog.apply_button.isEnabled())
                self.assertTrue(self.dialog.error.isVisible())
                self.dialog.samples.setFocus(); QTest.keyClick(self.dialog.samples, Qt.Key.Key_Return)
                self.dialog.ppr.setFocus(); self.application.processEvents()
                QTest.mouseClick(self.dialog.apply_button, Qt.MouseButton.LeftButton)
                self.dialog.accept(); self.application.processEvents()
                self.assertNotEqual(self.dialog.result(), QDialog.DialogCode.Accepted)
                self.assertEqual(self.dialog.samples.text(), raw)
                with self.assertRaises(ValueError): self.dialog.plan()
        if EVIDENCE:
            self.dialog.samples.setText("8192"); self.application.processEvents()
            self.assertTrue(self.dialog.grab().save(str(EVIDENCE / "pc-invalid-count.png")))

    def test_bidirectional_signals_are_bounded_and_rate_changes_keep_duration(self):
        duration_changes, sample_changes = [], []
        self.dialog.duration.valueChanged.connect(duration_changes.append)
        self.dialog.samples.textChanged.connect(sample_changes.append)
        for cycle in range(100):
            self.dialog.duration.setValue(1 if cycle % 2 else 40)
            self.dialog.samples.setText(str(50000 * (2 if cycle % 2 else 3)))
        self.assertLessEqual(len(duration_changes), 100)
        self.assertLessEqual(len(sample_changes), 100)
        duration = self.dialog.duration.value()
        self.dialog.rate.setCurrentIndex(self.dialog.rate.findData(5000))
        self.assertEqual(self.dialog.duration.value(), duration)
        self.assertEqual(int(self.dialog.samples.text()), 5000 * duration)

    def test_count_meanings_and_exact_plan(self):
        self.dialog.samples.setText("150000")
        self.assertEqual(self.dialog.duration.value(), 3)
        self.assertEqual(self.dialog.plan(), dict(mode="timed", sample_rate_hz=50000, duration_seconds=3, pulses_per_rev=1))
        self.assertIn("FFT", self.dialog.samples.toolTip())
        if EVIDENCE: self.assertTrue(self.dialog.grab().save(str(EVIDENCE / "pc-parameters.png")))

    def test_active_acquisition_locks_all_fields_and_apply(self):
        self.dialog.set_acquisition_active(True)
        for field in (self.dialog.mode, self.dialog.rate, self.dialog.samples, self.dialog.duration, self.dialog.ppr, self.dialog.apply_button): self.assertFalse(field.isEnabled())
        self.dialog.accept(); self.assertNotEqual(self.dialog.result(), QDialog.DialogCode.Accepted)
        with self.assertRaises(ValueError): self.dialog.plan()
        self.dialog.set_acquisition_active(False)
        self.assertTrue(self.dialog.apply_button.isEnabled())

    def test_window_rejects_illegal_or_busy_plan_before_transport(self):
        # No physical serial ports are enumerated or opened in this test.
        with patch.object(BoardSession, "ports", return_value=[]): window = Window()
        try:
            sent = []
            window.session.phase = "ready"; window.available = True
            window.send = lambda command, **data: sent.append((command, data)) or "request-1"
            for plan in (dict(sample_rate_hz=50000, duration_seconds=0, pulses_per_rev=1),
                         dict(sample_rate_hz=50000, duration_seconds=86401, pulses_per_rev=1),
                         dict(sample_rate_hz=8192, duration_seconds=1, pulses_per_rev=1),
                         dict(sample_rate_hz=50000, duration_seconds=1, pulses_per_rev=0),
                         dict(sample_rate_hz=50000, duration_seconds=1, pulses_per_rev=1, samples=8192)):
                plan["mode"]="timed"
                self.assertTrue(capture_plan_error(plan)); window.plan = plan
                window.start_capture(); self.assertEqual(sent, []); self.assertFalse(window.active)
            window.plan = dict(mode="timed", sample_rate_hz=50000, duration_seconds=1, pulses_per_rev=1)
            window.capture_dialog = self.dialog; window.active = True; window.update_controls()
            self.assertFalse(self.dialog.samples.isEnabled()); self.assertFalse(window.params_btn.isEnabled())
            window.start_capture(); self.assertEqual(sent, [])
            window.active = False; window.remote_busy = True; window.start_capture(); self.assertEqual(sent, [])
            window.remote_busy = False; window.update_controls(); window.start_capture()
            self.assertEqual(len(sent), 1); self.assertEqual(sent[0], ("start", {"plan": window.plan}))
        finally:
            window.capture_dialog = None; window.active = False; window.session.phase = "closed"
            window.close(); window.deleteLater(); self.application.processEvents()

    def test_continuous_omits_duration_and_restores_timed_input(self):
        self.dialog.samples.setText("4320000000")
        self.assertEqual(self.dialog.duration.value(),86400)
        self.assertFalse(capture_plan_error(self.dialog.plan()))
        self.dialog.mode.setCurrentIndex(self.dialog.mode.findData("continuous"))
        self.application.processEvents()
        self.assertFalse(self.dialog.samples.isVisible());self.assertFalse(self.dialog.duration.isVisible())
        self.assertEqual(self.dialog.plan(),dict(mode="continuous",sample_rate_hz=50000,pulses_per_rev=1))
        if EVIDENCE:self.assertTrue(self.dialog.grab().save(str(EVIDENCE/"pc-continuous.png")))
        self.dialog.mode.setCurrentIndex(self.dialog.mode.findData("timed"))
        self.assertEqual(self.dialog.samples.text(),"4320000000")
        self.assertEqual(self.dialog.plan()["duration_seconds"],86400)
        self.assertTrue(capture_plan_error(dict(mode="continuous",sample_rate_hz=50000,duration_seconds=10,pulses_per_rev=1)))

    def test_stop_preserves_busy_state_until_sd_terminal(self):
        with patch.object(BoardSession,"ports",return_value=[]):window=Window()
        try:
            sent=[];window.send=lambda command,**data:sent.append((command,data)) or "req"
            window.session.phase="ready";window.active=True;window.available=True;window.current_job="session-1"
            window.plan=dict(mode="continuous",sample_rate_hz=50000,pulses_per_rev=1)
            window.stop_capture();window.stop_capture()
            self.assertEqual(sent,[("stop",{"job_id":"session-1"})])
            self.assertTrue(window.active);self.assertTrue(window.stop_pending)
            self.assertFalse(window.params_btn.isEnabled());self.assertFalse(window.cancel_btn.isEnabled())
            window.job_status(dict(job_id="session-1",state="stopping",records_received="100000",durable_records="0",total_records="0"))
            self.assertTrue(window.active)
            window.session_done(dict(job_id="session-1",state="stopped",records_received="100000",durable_records="100000",total_records="0",session_complete=True))
            window.update_controls()
            self.assertFalse(window.active);self.assertFalse(window.remote_busy);self.assertFalse(window.stop_pending)
            self.assertTrue(window.params_btn.isEnabled());self.assertIn("SD 已保存 2.0 s",window.status.text())
        finally:
            window.active=False;window.session.phase="closed";window.close();window.deleteLater()

    def test_failed_storage_does_not_claim_complete(self):
        with patch.object(BoardSession,"ports",return_value=[]):window=Window()
        try:
            window.active=True;window.current_job="s1"
            window.session_done(dict(job_id="s1",state="failed",records_received="600000",durable_records="500000",total_records="0",session_complete=False,error_message="SD 空间不足"))
            self.assertFalse(window.active);self.assertIn("异常",window.status.text())
            self.assertIn("SD 已保存 10.0 s",window.status.text());self.assertEqual(window.notice.text(),"SD 空间不足")
        finally:window.active=False;window.close();window.deleteLater()


def main():
    global EVIDENCE
    parser = argparse.ArgumentParser(); parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    EVIDENCE = args.evidence
    if EVIDENCE: EVIDENCE.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(CaptureParameterTests))
    report = dict(schema="serial-pc-capture-parameters-host-v1", passed=result.wasSuccessful(), tests=result.testsRun,
        failures=len(result.failures), errors=len(result.errors), scope="Offscreen Qt; no physical serial ports enumerated or opened",
        app_sha256=hashlib.sha256(Path(__file__).with_name("app.py").read_bytes()).hexdigest())
    if EVIDENCE: (EVIDENCE / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report)); return 0 if result.wasSuccessful() else 1


if __name__ == "__main__": sys.exit(main())
