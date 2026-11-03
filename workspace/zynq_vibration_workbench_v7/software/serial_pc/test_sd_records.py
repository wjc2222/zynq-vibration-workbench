"""Deletion UI safety and result handling; no serial device or file is deleted."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import unittest
from pathlib import Path
from unittest.mock import patch, Mock
from PySide6.QtWidgets import QApplication, QMessageBox, QTableWidget, QPushButton
from PySide6.QtCore import Qt
from app import Window, configure_application
from protocol import ProtocolError


class SdRecordsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app=QApplication.instance() or QApplication([]);configure_application(cls.app)

    def setUp(self):
        self.window=Window(Path("unused-analyzer"));self.window.session.phase="ready"
        self.sent=[]
        def send(command,**fields):
            self.sent.append((command,fields));return "request-"+str(len(self.sent))
        self.window.send=send
        self.records=[dict(record_id="a"*32+"-000000",bytes=1048576,synthetic=True),dict(record_id="b"*32+"-000000",bytes=2097152,synthetic=True)]

    def tearDown(self):
        if self.window.record_dialog:self.window.record_dialog.close()
        self.window.session.phase="closed";self.window.active=False
        self.window.closing_window=True;self.window.close();self.window.deleteLater();self.app.processEvents()

    def test_default_cancel_and_no_implicit_selection(self):
        self.window.show_records(self.records);self.app.processEvents()
        table=self.window.record_dialog.findChild(QTableWidget,"sdRecordsTable")
        delete=self.window.record_dialog.findChild(QPushButton,"deleteSdRecords")
        self.assertEqual(table.selectionMode(),QTableWidget.SelectionMode.ExtendedSelection)
        self.assertFalse(delete.isEnabled())
        table.selectRow(0);self.assertTrue(delete.isEnabled())
        seen={}
        def cancel(dialog):
            seen.update(default=dialog.standardButton(dialog.defaultButton()),text=dialog.text(),details=dialog.detailedText())
            return QMessageBox.StandardButton.No
        with patch.object(QMessageBox,"exec",cancel):self.assertFalse(self.window.delete_records(self.records))
        self.assertEqual(seen["default"],QMessageBox.StandardButton.No)
        self.assertIn("3.0 MiB",seen["text"]);self.assertIn(self.records[1]["record_id"],seen["details"])
        self.assertEqual(self.sent,[])

    def test_only_confirmed_ids_sent_and_controls_locked(self):
        with patch.object(QMessageBox,"exec",return_value=QMessageBox.StandardButton.Yes):
            self.assertTrue(self.window.delete_records(self.records))
        self.assertEqual(self.sent,[("delete_records",dict(record_ids=[r["record_id"] for r in self.records]))])
        self.assertFalse(self.window.start_btn.isEnabled());self.assertFalse(self.window.params_btn.isEnabled())
        self.assertFalse(self.window.records_btn.isEnabled());self.assertFalse(self.window.download_btn.isEnabled())

    def test_busy_or_disconnected_never_confirms(self):
        for field,value in [("active",True),("remote_busy",True),("download_pending",True),("download",object()),("analysis_process",object()),("delete_request_id","pending")]:
            setattr(self.window,field,value)
            with patch.object(QMessageBox,"exec") as prompt:self.assertFalse(self.window.delete_records(self.records));prompt.assert_not_called()
            setattr(self.window,field,None if field in ("download","analysis_process","delete_request_id") else False)
        self.window.session.phase="closed"
        with patch.object(QMessageBox,"exec") as prompt:self.assertFalse(self.window.delete_records(self.records));prompt.assert_not_called()

    def test_state_rechecked_after_confirmation(self):
        def disconnected(_):self.window.session.phase="closed";return QMessageBox.StandardButton.Yes
        with patch.object(QMessageBox,"exec",disconnected):self.assertFalse(self.window.delete_records(self.records))
        self.assertEqual(self.sent,[])

    def test_reject_duplicate_empty_and_large_selection(self):
        with patch.object(QMessageBox,"exec") as prompt:
            for records in ([],self.records*2,[dict(record_id=str(n),bytes=1) for n in range(101)]):self.assertFalse(self.window.delete_records(records))
            prompt.assert_not_called()

    def test_partial_success_clears_deleted_remote_and_refreshes(self):
        self.window.record_id=self.records[0]["record_id"];self.window.result={"old":True};self.window.delete_request_id="del"
        self.window.on_message(dict(command="delete_records",id="del",ok=True,data=dict(results=[dict(record_id=self.records[0]["record_id"],deleted=True),dict(record_id=self.records[1]["record_id"],deleted=False,error=dict(message="正在使用"))])))
        self.assertIsNone(self.window.result);self.assertEqual(self.window.record_id,"")
        self.assertIn("正在使用",self.window.notice.text());self.assertEqual(self.sent[-1][0],"list_records")

    def test_stale_reply_cannot_clear_current_request(self):
        self.window.delete_request_id="new"
        self.window.on_message(dict(command="delete_records",id="old",ok=True,data={}))
        self.assertEqual(self.window.delete_request_id,"new");self.assertEqual(self.sent,[])

    def test_partial_files_removed_invalidate_remote_reference(self):
        self.window.record_id=self.records[0]["record_id"];self.window.result={"old":True}
        self.window.records_deleted(dict(results=[dict(record_id=self.window.record_id,deleted=False,files_removed=1,remaining_staging_id=".deleting-test",error=dict(message="部分删除失败"))]))
        self.assertEqual(self.window.record_id,"");self.assertIsNone(self.window.result)
        self.assertIn("部分删除失败",self.window.notice.text())

    def test_server_refusal_unlocks_and_explains(self):
        self.window.delete_request_id="del"
        self.window.on_message(dict(command="delete_records",id="del",ok=False,error=dict(code="BUSY",message="采集中")))
        self.assertIsNone(self.window.delete_request_id);self.assertIn("采集中",self.window.notice.text())

    def test_local_download_manifest_rejection_releases_board_reference(self):
        self.window.download_pending=True;self.window.download_request_id="manifest-current"
        with patch("app.RecordDownload",side_effect=ProtocolError("destination is no longer available")):
            self.window.on_message(dict(command="download_begin",id="manifest-current",ok=True,data={}))
        self.assertEqual(self.sent,[("download_end",{})])
        self.assertFalse(self.window.download_pending);self.assertIsNone(self.window.download_request_id)
        self.assertIsNone(self.window.download);self.assertIn("destination",self.window.notice.text())

    def test_server_chunk_failure_releases_board_reference(self):
        receiver=Mock();self.window.download=receiver;self.window.download_request_id="chunk-current"
        self.window.on_message(dict(command="download_chunk",id="chunk-current",ok=False,error=dict(code="RECORD_CHANGED",message="selected pair changed")))
        receiver.abort.assert_called_once_with();self.assertEqual(self.sent,[("download_end",{})])
        self.assertFalse(self.window.download_pending);self.assertIsNone(self.window.download_request_id)
        self.assertIsNone(self.window.download);self.assertIn("RECORD_CHANGED",self.window.notice.text())

    def test_stale_download_error_cannot_release_new_download(self):
        receiver=Mock();self.window.download=receiver;self.window.download_request_id="chunk-new"
        self.window.on_message(dict(command="download_chunk",id="chunk-old",ok=False,error=dict(code="RECORD_CHANGED",message="old error")))
        receiver.abort.assert_not_called();self.assertEqual(self.sent,[])
        self.assertIs(self.window.download,receiver);self.assertEqual(self.window.download_request_id,"chunk-new")
        self.window.download=None

    def test_local_chunk_rejection_releases_and_clears_request(self):
        receiver=Mock();receiver.accept.side_effect=ProtocolError("chunk CRC mismatch")
        self.window.download=receiver;self.window.download_request_id="chunk-current"
        with self.assertRaises(ProtocolError):self.window.download_chunk({})
        receiver.abort.assert_called_once_with();self.assertEqual(self.sent,[("download_end",{})])
        self.assertIsNone(self.window.download);self.assertIsNone(self.window.download_request_id)


if __name__=="__main__":unittest.main(verbosity=2)
