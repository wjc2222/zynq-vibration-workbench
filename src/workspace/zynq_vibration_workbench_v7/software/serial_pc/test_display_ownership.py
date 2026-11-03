"""Qt regressions for display provenance; all payloads/files are synthetic.

Set VIBRATION_ANALYZER to a real CLI executable to run the local-file cases.
No serial port, board, or native desktop automation is used.
"""
from __future__ import annotations
import copy
import math
import os
from pathlib import Path
import tempfile
import time
import unittest

os.environ.setdefault('QT_QPA_PLATFORM','offscreen')
from PySide6.QtCore import QCoreApplication,QEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication
from shiboken6 import isValid
from app import Window,configure_application
from protocol import ProtocolError
from test_waveform_fidelity import live_payload,write_synthetic

APP=QApplication.instance() or QApplication([])
configure_application(APP)
ANALYZER=Path(os.environ.get('VIBRATION_ANALYZER','unused-analyzer'))


def result_payload():
    return dict(schema='zynq_vibration.analysis.v1',integrity='passed',amplitude_unit='V',
        configuration={},header=dict(records=500000),duration_s=10.,source_metadata=dict(synthetic=True),
        waveform=[dict(time_s=i*.01,values=[math.sin(i),math.cos(i),math.sin(i/2)]) for i in range(32)],
        time_stats=[dict(rms=1.38,peak_to_peak=3.89,crest_factor=1.41,pearson_kurtosis=1.5) for _ in range(3)],
        spectra=[dict(frequency_hz=[0,1000,2000],peak_amplitude=[0,1,0],fft_size=65536,bin_width_hz=50000/65536) for _ in range(3)],
        speed=dict(pulses_per_rev=1,trace=[],held_mean_rpm=1480))


def segment_packet(result=None):
    result=copy.deepcopy(result or result_payload())
    result.pop('waveform',None);result.pop('spectra',None);result['speed'].pop('trace',None)
    result['display_scope']='features_only'
    return dict(event='analysis',data=dict(job_id='previous-capture',record_id='old-segment',
        scope='completed_segment',result=result))


class DisplayOwnershipTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='vibration-display-ownership-')
        self.window=Window(ANALYZER);self.window.save_directory=Path(self.temp.name)
        self.window.resize(1280,760);self.window.show();APP.processEvents()

    def tearDown(self):
        window=self.window
        if window.analysis_process is not None:
            window.analysis_process.kill();window.analysis_process.waitForFinished(3000)
        window.active=False;window.closing_window=True;window.close();window.deleteLater()
        QCoreApplication.sendPostedEvents(None,QEvent.Type.DeferredDelete);APP.processEvents()
        self.assertFalse(isValid(window));self.temp.cleanup()

    def begin(self):
        w=self.window;w.clear_result();w.current_job='previous-capture';w.active=True;w.set_display_live(True)
        payload=live_payload();payload.update(job_id=w.current_job,source='test_fixture',synthetic=True)
        return payload

    def test_clear_result_retires_previous_capture_events(self):
        w=self.window;w.current_job='previous-capture';w.result_origin='board';w.clear_result()
        w.on_message(segment_packet())
        self.assertEqual(w.current_job,'');self.assertEqual(w.result_origin,'');self.assertIsNone(w.result)

    def test_local_view_rejects_same_job_remote_results_and_errors(self):
        w=self.window;w.current_job='previous-capture';w.result_origin='local';w.result={'local':'sentinel'}
        w.on_message(segment_packet())
        w.on_message(dict(event='analysis',data=dict(job_id=w.current_job,record_id='old-record',scope='complete_record',result=result_payload())))
        for job in (w.current_job,None,'other-capture'):
            w.on_message(dict(event='analysis_failed',data=dict(job_id=job,message='late failure')))
        self.assertEqual(w.result,{'local':'sentinel'});self.assertEqual(w.result_origin,'local')

    def test_selected_record_retires_previous_job_and_ignores_late_features(self):
        w=self.window;w.current_job='previous-capture'
        w.complete_analysis(dict(record_id='selected-record',scope='complete_record',result=result_payload()),selected=True)
        selected=w.result;w.on_message(segment_packet())
        self.assertEqual(w.current_job,'');self.assertEqual(w.record_id,'selected-record');self.assertIs(w.result,selected)
        self.assertEqual(w.waveforms._sampling_context.get('summary_source'),'record_summary')
        self.assertIn('记录极值摘要',w.window_label.text());self.assertFalse(w._display_warning)

    def test_legacy_preview_warning_survives_channel_rpm_terminal_and_features(self):
        w=self.window;legacy=self.begin();legacy.pop('waveform_detail');legacy.pop('waveform_envelope')
        legacy['voltage']=[[(i*.25/31,math.sin(i+ch)) for i in range(32)] for ch in range(3)]
        w.preview(legacy)
        for action in (lambda:w.waveforms.channel_boxes[1].click(),
                       lambda:w.waveforms.speed_box.click(),lambda:w.waveforms.speed_box.click(),
                       lambda:w.show_notice('')):
            action();self.assertIn('当前预览未提供连续原始样点',w.notice.text())
        terminal=dict(job_id=w.current_job,state='completed',records_received=500000,durable_records=500000,plan=dict(sample_rate_hz=50000))
        w.job_status(terminal);w.on_message(segment_packet())
        self.assertTrue(w.waveforms.plot.samples_are_summary)
        self.assertEqual(w.waveforms._sampling_context.get('summary_source'),'legacy_preview')
        self.assertEqual(w.window_label.text(),'预览极值摘要 · 缺少连续原始样点')
        self.assertIn('当前预览未提供连续原始样点',w.notice.text())

    def test_new_detail_clears_only_legacy_display_warning(self):
        w=self.window;payload=self.begin();legacy=copy.deepcopy(payload)
        legacy.pop('waveform_detail');legacy.pop('waveform_envelope');legacy['voltage']=[[(0,0),(.25,1)]]*3
        w.preview(legacy);w.show_notice('independent notice');w.preview(payload)
        self.assertFalse(w.waveforms.plot.samples_are_summary);self.assertFalse(w._display_warning)
        self.assertEqual(w.notice.text(),'independent notice');self.assertIn('连续原始样点',w.window_label.text())

    def test_explicit_new_marker_missing_detail_refused_even_with_legacy_points(self):
        w=self.window;payload=self.begin();payload.pop('waveform_detail')
        payload.update(voltage_representation='use_waveform_detail',voltage=[[(0,0),(.25,1)]]*3)
        with self.assertRaisesRegex(ProtocolError,'新版预览标记缺少'):w.render_preview(payload)
        w.on_message(dict(event='preview',data=payload))
        self.assertTrue(w.preview_failed);self.assertIn('新版预览标记缺少',w.notice.text())
        self.assertFalse(w.waveforms.plot.samples_are_summary)

    def test_new_detail_survives_freeze_resume_rpm_and_terminal(self):
        w=self.window;payload=self.begin();w.preview(payload)
        w.waveforms.zoom(.5);frozen=copy.deepcopy(w.waveforms.data)
        later=copy.deepcopy(payload);later['revision']+=1
        later['waveform_detail']['first_index']=str(int(later['waveform_detail']['first_index'])+100)
        later['waveform_envelope']['end_s']+=.01;w.preview(later)
        self.assertEqual(w.waveforms.data,frozen);w.resume_display()
        w.waveforms.channel_boxes[1].click();w.waveforms.speed_box.click();w.waveforms.speed_box.click()
        w.job_status(dict(job_id=w.current_job,state='completed'))
        w.on_message(segment_packet());self.assertFalse(w.waveforms.plot.samples_are_summary)
        self.assertEqual(len(w.waveforms.data[0]),512)
        self.assertAlmostEqual(w.waveforms._sampling_context['detail'][1]-w.waveforms._sampling_context['detail'][0],.01022,places=12)

    def test_remote_detail_remains_raw_and_ignores_late_old_capture(self):
        w=self.window;w.current_job='previous-capture';result=result_payload();payload=live_payload()
        result.update(display_detail=payload['waveform_detail'],display_envelope=payload['waveform_envelope'])
        w.complete_analysis(dict(record_id='selected-raw',scope='complete_record',result=result),selected=True)
        w.on_message(segment_packet());self.assertEqual(w.record_id,'selected-raw')
        self.assertFalse(w.waveforms.plot.samples_are_summary);self.assertEqual(len(w.waveforms.data[0]),512)

    def unsolicited_replies(self):
        bodies={
            'analyze_record':dict(record_id='unsolicited-record',scope='complete_record',result=result_payload()),
            'list_records':dict(records=[]),
            'download_begin':dict(record_id='unsolicited-record',files=[]),
            'download_chunk':dict(record_id='unsolicited-record'),
            'delete_records':dict(results=[])}
        for command,data in bodies.items():
            for response_id in (None,'','unsolicited-id'):
                for ok in (True,False):
                    yield dict(command=command,id=response_id,ok=ok,data=data,error=dict(code='STALE',message='unsolicited'))

    def test_missing_and_unsolicited_response_ids_cannot_replace_live_view(self):
        w=self.window;w.preview(self.begin());before=copy.deepcopy(w.waveforms.data)
        for response in self.unsolicited_replies():
            w.on_message(response)
            self.assertEqual(w.waveforms.data,before);self.assertFalse(w.waveforms.plot.samples_are_summary)
            self.assertIsNone(w.result);self.assertTrue(w.active);self.assertFalse(w.preview_failed)
            self.assertEqual(w.current_job,'previous-capture');self.assertIsNone(w.record_dialog)
            self.assertEqual(w.notice.text(),'')

    def test_missing_and_unsolicited_response_ids_cannot_replace_local_view(self):
        w=self.window;w.result_origin='local';w.result={'local':'sentinel'};w.record_id='local-sentinel'
        for response in self.unsolicited_replies():
            w.on_message(response)
            self.assertEqual(w.result,{'local':'sentinel'});self.assertEqual(w.result_origin,'local')
            self.assertEqual(w.record_id,'local-sentinel');self.assertIsNone(w.record_dialog)
            self.assertFalse(w.preview_failed);self.assertEqual(w.notice.text(),'')

    def test_none_response_id_cannot_consume_real_pending_request(self):
        w=self.window
        fields=('records_request_id','remote_analysis_request_id','delete_request_id','download_request_id')
        for field in fields:setattr(w,field,'expected-id')
        for response in self.unsolicited_replies():w.on_message(response)
        for field in fields:self.assertEqual(getattr(w,field),'expected-id')
        self.assertIsNone(w.result);self.assertIsNone(w.record_dialog);self.assertFalse(w.preview_failed)


@unittest.skipUnless(ANALYZER.is_file(),'Set VIBRATION_ANALYZER for real CLI integration')
class LocalCliOwnershipTests(unittest.TestCase):
    setUp=DisplayOwnershipTests.setUp
    tearDown=DisplayOwnershipTests.tearDown
    @classmethod
    def setUpClass(cls):
        cls.fixture_dir=tempfile.TemporaryDirectory(prefix='vibration-display-cli-fixture-')
        cls.fixture=Path(cls.fixture_dir.name)/'synthetic-1kHz-50k-10s.BIN'
        write_synthetic(cls.fixture,500000,lambda i:tuple(round(2*32768/5*f*math.sin(2*math.pi*1000*i/50000+phase))
            for f,phase in ((1,0),(.65,.4),(.35,.8))),description='Synthetic display event ownership regression; no hardware measurement')

    @classmethod
    def tearDownClass(cls):cls.fixture_dir.cleanup()

    def wait_local(self):
        end=time.monotonic()+45
        while self.window.analysis_process is not None and time.monotonic()<end:QTest.qWait(15)
        self.assertIsNone(self.window.analysis_process);self.assertIsNotNone(self.window.result,self.window.notice.text())

    def test_late_features_during_actual_cli_keep_local_raw_provider(self):
        w=self.window;w.current_job='previous-capture';w.analyze_local(self.fixture)
        self.assertIsNotNone(w.analysis_process);w.on_message(segment_packet());self.wait_local()
        self.assertEqual(w.result_origin,'local');self.assertEqual(w.current_job,'');self.assertEqual(w.record_id,'')
        self.assertIsNotNone(w.local_waveform);self.assertIsNotNone(w.waveforms.plot.raw_provider)
        self.assertFalse(w.waveforms.plot.samples_are_summary);self.assertEqual(w.result['header']['records'],500000)
        w.waveforms.plot.set_x_range(9.51,9.52)
        self.assertTrue(500<=len(w.waveforms.plot._axis_layout()['series'][0][1])<=501)

    def test_late_features_after_actual_cli_do_not_mix_record_identity(self):
        w=self.window;w.current_job='previous-capture';w.analyze_local(self.fixture);self.wait_local()
        result=w.result;reader=w.local_waveform;w.on_message(segment_packet(result))
        self.assertIs(w.result,result);self.assertIs(w.local_waveform,reader)
        self.assertEqual(w.result_origin,'local');self.assertEqual(w.record_id,'');self.assertIn('waveform',w.result)

    def test_late_complete_record_and_failure_during_cli_are_ignored(self):
        w=self.window;w.current_job='previous-capture';w.analyze_local(self.fixture)
        w.on_message(dict(event='analysis',data=dict(job_id='previous-capture',record_id='old-record',scope='complete_record',result=result_payload())))
        for job in ('previous-capture',None):w.on_message(dict(event='analysis_failed',data=dict(job_id=job,message='stale board failure')))
        self.wait_local();self.assertEqual(w.result_origin,'local');self.assertIsNotNone(w.local_waveform)
        self.assertFalse(w.waveforms.plot.samples_are_summary);self.assertNotIn('stale board failure',w.notice.text())


if __name__=='__main__':unittest.main(verbosity=2)
