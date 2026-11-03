"""Shutdown regressions using actual Qt dispatch and an inert transport, no COM."""
import json
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch

from PySide6.QtCore import QCoreApplication, QObject, QProcess

from protocol import PacketDecoder
from session import BoardSession
from test_protocol import packet


class Transport(QObject):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.writes=[]
        self.closed=False
        self.baud_changes=[]

    def write(self,data):
        self.writes.append(bytes(data));return len(data)

    def bytesToWrite(self):return 0
    def close(self):self.closed=True
    def closeWriteChannel(self):self.closed=True
    def state(self):return QProcess.ProcessState.NotRunning
    def isOpen(self):return not self.closed
    def errorString(self):return "injected serial error"
    def setBaudRate(self,baud):self.baud_changes.append(baud);return True


class ShutdownTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app=QCoreApplication.instance() or QCoreApplication([])

    def setUp(self):
        self.session=BoardSession()
        self.transport=Transport(self.session)
        self.session.port=self.transport
        self.session.phase="ready"
        self.session.framed_started=True
        self.errors=[];self.messages=[]
        self.session.failed.connect(self.errors.append)
        self.session.message.connect(self.messages.append)

    def tearDown(self):
        self.session._close()
        self.app.processEvents()

    def begin(self):
        self.session.disconnect()
        self.assertEqual(self.session.phase,"closing")
        request=json.loads(self.transport.writes[-1])
        self.assertEqual(request["command"],"shutdown")
        self.assertEqual(request["id"],self.session.shutdown_request_id)
        return request["id"]

    def ack(self,request_id=None,**overrides):
        body=dict(id=request_id or self.session.shutdown_request_id,command="shutdown",ok=True,data={"accepted":True})
        body.update(overrides)
        return packet(body)

    def assert_failed(self):
        self.assertEqual(self.session.phase,"closed")
        self.assertTrue(self.errors)
        self.assertFalse(self.session.shutdown_acknowledged)

    def test_ack_and_shell_tail_in_same_read(self):
        request_id=self.begin()
        self.session._receive(self.ack()+b"\r\nAX7_PC_RETURNED\r\nroot@board:~# ")
        self.assertEqual(self.session.phase,"closed")
        self.assertTrue(self.session.shutdown_acknowledged)
        self.assertFalse(self.errors)
        self.assertEqual(self.messages,[dict(id=request_id,command="shutdown",ok=True,data={"accepted":True})])

    def test_fragmented_ack_then_late_shell_tail(self):
        self.begin()
        for byte in self.ack():self.session._receive(bytes([byte]))
        self.assertEqual(self.session.phase,"closing")
        self.assertFalse(self.transport.closed)
        self.session._receive(b"AX7_PC_RETURNED\nroot@board:# ")
        self.assertTrue(self.session.shutdown_acknowledged)
        self.assertTrue(self.session.shell_return_confirmed)
        self.assertEqual(self.transport.baud_changes,[115200])
        self.assertFalse(self.errors)

    def test_ack_keeps_port_until_shell_restored_at_handshake_baud(self):
        self.session.baud=921600
        self.begin();self.session._receive(self.ack())
        self.assertTrue(self.session.shutdown_acknowledged)
        self.assertTrue(self.session.awaiting_shell_return)
        self.assertFalse(self.session.shell_return_confirmed)
        self.assertEqual(self.session.phase,"closing")
        self.assertFalse(self.transport.closed)
        self.assertEqual(self.transport.baud_changes,[115200])
        with self.assertRaisesRegex(ValueError,"Disconnect"):
            self.session.open("NO_COM_SHOULD_BE_OPENED",921600)
        self.session._receive(b"\r\nroot@board:~# ")
        self.assertEqual(self.session.phase,"closed")
        self.assertTrue(self.session.shell_return_confirmed)
        self.assertTrue(self.transport.closed)
        self.assertFalse(self.errors)

    def test_prompt_after_missed_returned_marker_completes_disconnect(self):
        self.begin();self.session._receive(self.ack())
        for _ in range(6):self.session._request_shell_prompt()
        self.assertEqual(self.transport.writes[1:],[b"\r"]*3)
        self.session._receive(b"root@bo")
        self.assertEqual(self.session.phase,"closing")
        self.session._receive(b"ard:~# ")
        self.assertTrue(self.session.shell_return_confirmed)
        self.assertEqual(self.session.phase,"closed")
        self.assertFalse(self.errors)

    def test_unreturned_shell_times_out_without_false_confirmation(self):
        self.begin();self.session._receive(self.ack())
        with patch("session.time.monotonic",return_value=self.session.shell_return_deadline+1):
            self.session._tick()
        self.assertEqual(self.session.phase,"closed")
        self.assertTrue(self.session.shutdown_acknowledged)
        self.assertFalse(self.session.shell_return_confirmed)
        self.assertTrue(self.errors)
        self.assertIn("恢复未确认",self.errors[0])

    def test_port_error_after_ack_does_not_confirm_shell_return(self):
        self.begin();self.session._receive(self.ack())
        self.session._port_error("injected")
        self.assertTrue(self.session.shutdown_acknowledged)
        self.assertFalse(self.session.shell_return_confirmed)
        self.assertTrue(self.errors)
        self.assertEqual(self.session.phase,"closed")

    def test_baud_restore_failure_is_reported_after_valid_ack(self):
        self.begin()
        with patch.object(self.transport,"setBaudRate",return_value=False):
            self.session._receive(self.ack())
        self.assertTrue(self.session.shutdown_acknowledged)
        self.assertFalse(self.session.shell_return_confirmed)
        self.assertTrue(self.errors)
        self.assertEqual(self.session.phase,"closed")

    def test_initial_probe_retries_only_on_shell_evidence_and_is_bounded(self):
        self.session.phase="probing"
        scheduled=[]
        with patch.object(self.session,"_later",side_effect=lambda delay,fn:scheduled.append((delay,fn))),patch("session.time.monotonic",return_value=10):
            self.session._probe()
            self.session._receive(b"old bridge still finishing\n")
            self.assertFalse(scheduled)
            self.session._receive(b"\nAX7_PC_RETURNED\nroot@board:~# ")
            self.session._receive(b"root@board:~# ")
            self.assertEqual(len(scheduled),1)
        with patch("session.time.monotonic",return_value=11):scheduled.pop()[1]()
        self.assertEqual(len(self.transport.writes),2)
        with patch.object(self.session,"_later",side_effect=lambda delay,fn:scheduled.append((delay,fn))):
            self.session._receive(b"\nroot@board:~# ")
        with patch("session.time.monotonic",return_value=12):scheduled.pop()[1]()
        with patch.object(self.session,"_later",side_effect=lambda delay,fn:scheduled.append((delay,fn))):
            self.session._receive(b"\nroot@board:~# ")
        self.assertFalse(scheduled)
        self.assertEqual(len(self.transport.writes),3)

    def test_shell_tail_before_ack_is_not_ignored(self):
        self.begin();self.session._receive(b"AX7_PC_RETURNED\n"+self.ack())
        self.assert_failed()

    def test_corrupt_ack_is_not_ignored(self):
        self.begin()
        frame=json.loads(self.ack());frame["sha256"]="0"*64
        self.session._receive(json.dumps(frame).encode()+b"\n")
        self.assert_failed()

    def test_wrong_response_id_is_not_accepted(self):
        self.begin();self.session._receive(self.ack("unmatched"))
        self.assert_failed()

    def test_wrong_response_command_is_not_accepted(self):
        self.begin();self.session._receive(self.ack(command="ping"))
        self.assert_failed()

    def test_failed_shutdown_response_is_not_accepted(self):
        self.begin();self.session._receive(self.ack(ok=False,error={"code":"REJECTED"}))
        self.assert_failed()

    def test_unaccepted_shutdown_is_not_accepted(self):
        self.begin();self.session._receive(self.ack(data={"accepted":False}))
        self.assert_failed()

    def test_missing_shutdown_data_is_not_accepted(self):
        self.begin();self.session._receive(self.ack(data=None))
        self.assert_failed()

    def test_no_ack_timeout_reports_failure(self):
        self.begin();self.session._shutdown_timeout()
        self.assert_failed()
        self.assertIn("shutdown",self.errors[0])

    def test_repeated_disconnect_keeps_waiting_for_ack(self):
        self.begin();count=len(self.transport.writes);self.session.disconnect()
        self.assertEqual(self.session.phase,"closing")
        self.assertEqual(len(self.transport.writes),count)
        self.session._shutdown_timeout();self.assert_failed()

    def test_child_exit_before_ack_reports_failure(self):
        self.begin();self.session.process=self.transport;self.session.port=None
        self.session._process_finished(self.transport)
        self.assert_failed()

    def test_port_error_before_ack_reports_failure(self):
        self.begin();self.session._port_error("injected")
        self.assert_failed()

    def test_other_valid_reply_before_ack_is_delivered(self):
        ping_id=self.session.send("ping");self.begin()
        ping=dict(id=ping_id,command="ping",ok=True,data={})
        self.session._receive(packet(ping)+packet(dict(id=self.session.shutdown_request_id,command="shutdown",ok=True,data={"accepted":True}),2)+b"shell prompt\n")
        self.assertFalse(self.errors)
        self.assertEqual(self.messages[0],ping)
        self.assertTrue(self.session.shutdown_acknowledged)

    def test_ready_phase_keeps_strict_framing(self):
        self.session._receive(packet({"event":"preview","data":{}})+b"shell prompt\n")
        self.assert_failed()

    def test_decoder_batch_api_remains_strict(self):
        with self.assertRaises(ValueError):
            PacketDecoder().feed(packet({"event":"a"})+b"shell prompt\n")

    def test_decoder_iterator_exposes_valid_packet_before_next_line(self):
        stream=PacketDecoder().iter_feed(packet({"event":"a"})+b"shell prompt\n")
        self.assertEqual(next(stream),{"event":"a"})
        with self.assertRaises(ValueError):next(stream)

    def test_previous_epoch_timeout_cannot_close_reconnected_session(self):
        callbacks=[]
        self.session._later(0,lambda:callbacks.append("old"))
        self.session._reset();self.session.phase="ready"
        self.app.processEvents()
        self.assertFalse(callbacks)
        self.session._later(0,lambda:callbacks.append("current"));self.app.processEvents()
        self.assertEqual(callbacks,["current"])
        self.assertEqual(self.session.phase,"ready")
        self.assertFalse(self.errors)

    def test_real_child_ack_and_shell_tail_then_exit(self):
        self.session.port=None
        self.session.phase="closed"
        child="""import json,os,sys
sys.path.insert(0,sys.argv[1])
from test_protocol import packet
for sequence,line in enumerate(sys.stdin.buffer,1):
    request=json.loads(line)
    command=request['command']
    data={'accepted':True} if command=='shutdown' else {'servicecaps':{'available':True}}
    response={'id':request['id'],'command':command,'ok':True,'data':data}
    os.write(1,packet(response,sequence)+(b'AX7_PC_RETURNED\\nroot@board:# ' if command=='shutdown' else b''))
    if command=='shutdown':break
"""
        self.session.open_process([sys.executable,"-u","-c",child,str(Path(__file__).parent)])
        deadline=time.monotonic()+5
        while self.session.phase!="ready" and time.monotonic()<deadline:
            self.app.processEvents();time.sleep(.005)
        self.assertEqual(self.session.phase,"ready",self.errors)
        self.session.disconnect()
        while self.session.phase!="closed" and time.monotonic()<deadline:
            self.app.processEvents();time.sleep(.005)
        self.assertEqual(self.session.phase,"closed",self.errors)
        self.assertTrue(self.session.shutdown_acknowledged)
        self.assertFalse(self.errors)


if __name__=="__main__":unittest.main(verbosity=2)
