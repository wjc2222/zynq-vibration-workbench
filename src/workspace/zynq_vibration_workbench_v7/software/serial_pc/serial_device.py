"""Bounded pyserial worker; serial I/O never runs in the Qt painting thread."""
from __future__ import annotations

import queue
import threading
import serial
from serial.tools import list_ports
from PySide6.QtCore import QObject, Signal


def available_ports():
    return [(port.device, port.description) for port in list_ports.comports()]


class SerialDevice(QObject):
    readyRead = Signal()
    errorOccurred = Signal(str)
    _received = Signal(bytes)
    _failure = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.name = ""
        self.baud = 115200
        self._serial = None
        self._error = ""
        self._buffer = bytearray()
        self._queue = queue.Queue(maxsize=128)
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._outstanding = 0
        self._incoming = 0
        self._thread = None
        self._received.connect(self._accept)
        self._failure.connect(self.errorOccurred)

    def setPortName(self,name):self.name=name

    def setBaudRate(self,baud):
        self.baud=baud
        if self.isOpen():
            try:self._queue.put_nowait(("baud",baud))
            except queue.Full:return False
        return True

    def open(self):
        try:
            device=serial.Serial(port=None,baudrate=self.baud,timeout=.01,write_timeout=.5,
                                 bytesize=8,parity="N",stopbits=1,xonxoff=False,rtscts=False,dsrdtr=False)
            device.dtr=False;device.rts=False;device.port=self.name;device.open()
            self._serial=device
            self._thread=threading.Thread(target=self._run,name="vibration-serial-io",daemon=True)
            self._thread.start()
            return True
        except Exception as exc:
            self._error=str(exc)
            return False

    def _run(self):
        try:
            while not self._stop.is_set():
                try:
                    kind,value=self._queue.get_nowait()
                    if kind=="baud":self._serial.baudrate=value
                    else:
                        position=0
                        while position<len(value) and not self._stop.is_set():
                            written=self._serial.write(value[position:])
                            if written<=0:raise serial.SerialTimeoutException("Serial write made no progress")
                            position+=written
                            with self._lock:self._outstanding-=written
                except queue.Empty:pass
                data=self._serial.read(max(1,min(32768,self._serial.in_waiting)))
                if data:
                    with self._lock:
                        self._incoming+=len(data)
                        if self._incoming>524288:raise IOError("Serial reader queue exceeded limit")
                    self._received.emit(data)
        except Exception as exc:
            if not self._stop.is_set():
                self._error=str(exc)
                self._failure.emit(self._error)

    def _accept(self,data):
        with self._lock:self._incoming-=len(data)
        if self._stop.is_set():return
        self._buffer.extend(data)
        self.readyRead.emit()

    def readAll(self):
        data=bytes(self._buffer);self._buffer.clear();return data

    def write(self,data):
        if not self.isOpen():return -1
        with self._lock:
            if self._outstanding+len(data)>65536:return -1
            self._outstanding+=len(data)
        try:self._queue.put_nowait(("write",bytes(data)))
        except queue.Full:
            with self._lock:self._outstanding-=len(data)
            return -1
        return len(data)

    def bytesToWrite(self):
        with self._lock:return self._outstanding

    def errorString(self):return self._error

    def isOpen(self):return self._serial is not None and self._serial.is_open and not self._stop.is_set()

    def close(self):
        self._stop.set()
        if self._serial:
            try:self._serial.cancel_read();self._serial.cancel_write()
            except (OSError,AttributeError):pass
        if self._thread:self._thread.join(1.0)
        if self._serial:self._serial.close();self._serial=None
