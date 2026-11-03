"""Asynchronous Windows serial connection; the board remains the acquisition owner."""
from __future__ import annotations

import time
import uuid
import re

from PySide6.QtCore import QObject, QProcess, QTimer, Signal
from serial_device import SerialDevice, available_ports

from protocol import PacketDecoder, ProtocolError, request_bytes


class BoardSession(QObject):
    state_changed = Signal(str)
    message = Signal(dict)
    failed = Signal(str)
    traffic = Signal(int, int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.port = None
        self.process = None
        self.phase = "closed"
        self.decoder = PacketDecoder()
        self.console = bytearray()
        self.pending = {}
        self.counter = 0
        self.rx = self.tx = 0
        self.last_rx = time.monotonic()
        self.deadline = 0.0
        self.nonce = ""
        self.timer = QTimer(self)
        self.timer.setInterval(500)
        self.timer.timeout.connect(self._tick)
        self.heartbeat_at = 0.0
        self.closing = False
        self.login_sent = False
        self.framed_started = False
        self.epoch = 0
        self.baud = 115200
        self.shutdown_request_id = None
        self.shutdown_acknowledged = False
        self.awaiting_shell_return = False
        self.shell_return_confirmed = False
        self.shell_return_deadline = 0.0
        self.shell_prompt_requests = 0
        self.probe_attempts = 0
        self.probe_retry_pending = False
        self.last_probe = 0.0

    @staticmethod
    def ports():
        return available_ports()

    @property
    def connected(self):
        return self.phase == "ready"

    def _phase(self, text):
        self.phase = text
        self.state_changed.emit(text)

    def open(self, name, baud=115200):
        if self.phase != "closed":
            raise ProtocolError("Disconnect the current board first")
        self._reset()
        if baud not in (115200,460800,921600):raise ProtocolError("Unsupported baud rate")
        self.baud=baud
        port = SerialDevice(self)
        self.port = port
        port.setPortName(name)
        port.setBaudRate(115200)
        port.readyRead.connect(lambda: self._receive(bytes(port.readAll())) if self.port is port else None)
        port.errorOccurred.connect(lambda error:self._port_error(error) if self.port is port else None)
        if not port.open():
            self._fail("串口打开失败：" + port.errorString())
            return
        self._phase("probing")
        self.deadline = time.monotonic() + 15
        self.timer.start()
        self._write(b"\r")
        self._later(350, self._probe)

    def open_process(self, command):
        """Explicit test harness path; never selected as a serial fallback."""
        if self.phase != "closed":
            raise ProtocolError("Session already open")
        self._reset()
        self.process = QProcess(self)
        process=self.process
        process.setProcessChannelMode(QProcess.ProcessChannelMode.SeparateChannels)
        process.readyReadStandardOutput.connect(lambda: self._receive(bytes(process.readAllStandardOutput())) if self.process is process else None)
        process.readyReadStandardError.connect(lambda: process.readAllStandardError() if self.process is process else None)
        process.finished.connect(lambda *_: self._process_finished(process))
        process.started.connect(self._bridge_ready)
        self._phase("launching")
        self.deadline = time.monotonic() + 15
        self.timer.start()
        process.start(command[0], command[1:])

    def _reset(self):
        self.epoch += 1
        self.decoder = PacketDecoder()
        self.console.clear()
        self.pending.clear()
        self.closing = False
        self.login_sent = False
        self.framed_started = False
        self.shutdown_request_id = None
        self.shutdown_acknowledged = False
        self.awaiting_shell_return = False
        self.shell_return_confirmed = False
        self.shell_return_deadline = 0.0
        self.shell_prompt_requests = 0
        self.probe_attempts = 0
        self.probe_retry_pending = False
        self.last_probe = 0.0
        self.rx = self.tx = 0
        self.nonce = uuid.uuid4().hex[:12]

    def _later(self, delay, callback):
        epoch = self.epoch
        QTimer.singleShot(delay, lambda: callback() if epoch == self.epoch else None)

    def _probe(self):
        self.probe_retry_pending = False
        if self.phase != "probing":
            return
        now = time.monotonic()
        if self.probe_attempts >= 3 or (self.probe_attempts and now-self.last_probe < .5):
            return
        # Fixed read-only commands only. Never inject a user's file path or port name.
        command = ("printf '\\nAX7_PC_PROBE_" + self.nonce + "\\n'; "
                   "if test -x /usr/bin/vibration_stream_bridge && test -x /usr/bin/vibration_stream_service; then echo AX7_PC_READY; "
                   "else echo AX7_PC_MISSING; fi\r")
        self._write(command.encode("ascii"))
        self.probe_attempts += 1
        self.last_probe = now

    @staticmethod
    def _shell_prompt(text):
        # A complete idle shell prompt, never a fragment inside echoed input.
        return re.search(rb"(?:^|\n)(?:[A-Za-z0-9_.:@/~+-]+)?[#$] ?$", text) is not None

    def _retry_probe_on_shell(self):
        if not self.probe_retry_pending and self.probe_attempts < 3:
            self.probe_retry_pending = True
            self._later(550, self._probe)

    def _await_shell_return(self, tail):
        # The ACK seals the raw session; foreground analysis futures may still
        # delay bridge destruction and the wrapper's restoration of 115200.
        # Keep the existing closing phase until the shell actually returns.
        if not self.port:
            self._close()  # Explicit QProcess tests have no serial shell wrapper.
            return
        self.pending.clear()
        self.awaiting_shell_return = True
        self.framed_started = False
        self.console.clear()
        self.shell_return_deadline = time.monotonic() + 120
        if self.port.setBaudRate(115200) is False:
            raise ProtocolError("Cannot restore serial handshake baud")
        self._receive_shell_return(tail)
        # The returned marker can race the asynchronous PC baud change. A
        # bounded single-byte CR requests a fresh prompt after restoration;
        # no long shell command is sent while the old bridge is terminating.
        if self.awaiting_shell_return:
            self._later(250, self._request_shell_prompt)

    def _request_shell_prompt(self):
        if not self.awaiting_shell_return or not self.port or self.shell_prompt_requests >= 3:
            return
        self.shell_prompt_requests += 1
        try:
            self._write(b"\r")
        except Exception as exc:
            self._fail("等待开发板命令行恢复时串口发送失败：" + str(exc))
            return
        if self.shell_prompt_requests < 3:
            self._later(1000, self._request_shell_prompt)

    def _receive_shell_return(self, data):
        self.console.extend(data)
        del self.console[:-32768]
        text = bytes(self.console).replace(b"\r", b"")
        if b"AX7_PC_RETURNED" in text.split(b"\n") or self._shell_prompt(text):
            self.shell_return_confirmed = True
            self.awaiting_shell_return = False
            self._close()

    def _launch(self):
        self.console.clear()
        self._phase("launching")
        self.deadline = time.monotonic() + 20
        # The shell always restores its own termios when our foreground bridge exits.
        command = ("AX7_PC_TTY=$(stty -g); stty raw -echo " + str(self.baud) + "; sleep 1; "
                   "/usr/bin/vibration_stream_bridge --baud " + str(self.baud) + " --service /usr/bin/vibration_stream_service; "
                   "stty \"$AX7_PC_TTY\"; printf '\\nAX7_PC_RETURNED\\n'\r")
        self._write(command.encode("ascii"))
        self._later(180, lambda: self.port.setBaudRate(self.baud) if self.port else None)
        self._later(1350, self._bridge_ready)

    def _bridge_ready(self):
        if self.phase != "launching":
            return
        self._phase("handshake")
        self.send("hello")

    def send(self, command, **fields):
        if self.phase not in ("handshake", "ready", "closing"):
            raise ProtocolError("串口服务尚未连接")
        if len(self.pending) >= 64:
            raise ProtocolError("Too many pending serial requests")
        self.counter += 1
        request_id = "pc_" + str(self.counter)
        raw = request_bytes(request_id, command, **fields)
        self.pending[request_id] = (command, time.monotonic() + (120 if command in ("analyze_record","shutdown","stop") else 35))
        self._write(raw)
        return request_id

    def _write(self, data):
        target = self.process or self.port
        if target is None or target.write(data) != len(data):
            raise ProtocolError("Serial output could not be queued")
        if target.bytesToWrite() > 65536:
            self._fail("串口发送队列拥塞")
            return
        self.tx += len(data)

    def _receive(self, data):
        if not data or self.phase == "closed":
            return
        self.last_rx = time.monotonic()
        self.rx += len(data)
        self.traffic.emit(self.rx, self.tx)
        try:
            if self.awaiting_shell_return:
                self._receive_shell_return(data)
                return
            if self.phase == "probing":
                self.console.extend(data)
                del self.console[:-32768]
                text = bytes(self.console).replace(b"\r", b"")
                lines = text.split(b"\n")
                if b"AX7_PC_READY" in lines:
                    self._launch()
                elif b"AX7_PC_MISSING" in lines:
                    self._fail("开发板尚未部署 SD 连续采集版本。请部署配套固件后再连接。")
                elif text.rstrip().endswith(b"login:") and not self.login_sent:
                    self.login_sent = True
                    self._write(b"root\r")
                    self._later(800, self._probe)
                elif b"AX7_PC_RETURNED" in lines or self._shell_prompt(text):
                    self._retry_probe_on_shell()
                return
            if self.phase in ("launching", "handshake") and not self.framed_started:
                self.console.extend(data)
                if len(self.console) > 262144:
                    raise ProtocolError("No valid serial bridge handshake")
                start = self.console.find(b'{"codec":')
                if start < 0:
                    start = self.console.find(b'{"schema":')
                if start < 0:
                    return
                data = bytes(self.console[start:])
                self.console.clear()
                self.framed_started = True
            for value in self.decoder.iter_feed(data):
                if "id" in value:
                    request = self.pending.pop(value["id"], None)
                    if request is None or request[0] != value.get("command"):
                        raise ProtocolError("Unmatched serial response")
                    if value.get("command") == "hello" and value.get("ok") is True:
                        self._phase("ready")
                        self.heartbeat_at = time.monotonic()
                    if value.get("command") == "shutdown" and self.closing:
                        if (value["id"] != self.shutdown_request_id or value.get("ok") is not True or
                                not isinstance(value.get("data"), dict) or value["data"].get("accepted") is not True):
                            raise ProtocolError("Shutdown was not successfully acknowledged")
                        self.shutdown_acknowledged = True
                        self.message.emit(value)
                        # Only this validated, matched success ends framing.
                        # Preserve an already buffered shell tail from this read.
                        tail = bytes(self.decoder.buffer)
                        self.decoder.buffer.clear()
                        self._await_shell_return(tail)
                        return
                self.message.emit(value)
        except Exception as exc:
            self._fail("通信校验失败：" + str(exc))

    def _tick(self):
        now = time.monotonic()
        if self.awaiting_shell_return and now > self.shell_return_deadline:
            self._fail("采集关闭已确认，但开发板串口命令行恢复未确认；串口已释放，请确认开发板状态后重连。")
            return
        if self.phase in ("probing", "launching", "handshake") and now > self.deadline:
            self._fail("连接超时。请确认开发板已启动串口版 Linux、COM 口正确且未被其他程序占用。")
            return
        if self.phase == "ready" and now - self.heartbeat_at >= 3:
            if not any(command == "ping" for command, _ in self.pending.values()):
                self.send("ping")
            self.heartbeat_at = now
        for command, deadline in list(self.pending.values()):
            if now > deadline:
                self._fail("串口响应超时：" + command)
                return

    def _port_error(self, error):
        if self.port and self.port.isOpen():
            self._fail("串口异常：" + self.port.errorString())

    def _process_finished(self, process):
        if self.process is not process:
            return
        # QProcess can still expose final stdout when finished is emitted.
        # Consume a possible success acknowledgement before classifying EOF.
        if hasattr(process, "readAllStandardOutput"):
            self._receive(bytes(process.readAllStandardOutput()))
        if self.process is not process:
            return
        self._fail("板端进程在关闭确认前退出" if self.closing else "板端进程已退出")

    def _shutdown_timeout(self):
        if self.phase == "closing" and not self.shutdown_acknowledged:
            self._fail("串口服务关闭未确认：未收到匹配的 shutdown 成功响应。串口已释放，请重新确认开发板状态。")

    def disconnect(self):
        if self.phase == "closing":
            return
        if self.phase == "ready":
            self.closing = True
            self._phase("closing")
            try:
                self.shutdown_request_id = self.send("shutdown")
            except Exception as exc:
                self._fail("关闭请求发送失败：" + str(exc))
                return
            # The board acknowledges only after the final SD segment is sealed.
            self._later(120000, self._shutdown_timeout)
        else:
            self._close()

    def _fail(self, message):
        if self.phase == "closed" and self.port is None and self.process is None:
            return
        self.failed.emit(message)
        self._close()

    def _close(self):
        self.closing = True
        self.awaiting_shell_return = False
        self.timer.stop()
        self.pending.clear()
        if self.port:
            self.port.close()
            self.port.deleteLater()
            self.port = None
        if self.process:
            process = self.process
            self.process = None
            process.closeWriteChannel()
            # Parent the timer to the test child so deleting either cannot
            # leave a callback that calls methods on a deleted QProcess.
            timer = QTimer(process)
            timer.setSingleShot(True)
            timer.timeout.connect(lambda: process.kill() if process.state() != QProcess.ProcessState.NotRunning else None)
            timer.start(3000)
            if process.state() == QProcess.ProcessState.NotRunning:
                process.deleteLater()
            else:
                process.finished.connect(process.deleteLater)
        self._phase("closed")
