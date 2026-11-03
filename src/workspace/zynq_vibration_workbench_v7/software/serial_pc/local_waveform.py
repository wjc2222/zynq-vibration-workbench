"""Read verified local CAPDDR2/CAPSTRM1 samples at the requested time scale.

Capture ``capture_file_identity(path)`` immediately before launching the CLI,
then construct ``LocalWaveform(path, result, expected_identity=identity)`` only
after that CLI reports integrity=passed.  This reader checks the header and
the identity, and validates the records it actually reads; it deliberately
does not repeat the CLI's full payload CRC/continuity scan on the GUI thread.
The input must remain an immutable regular local file for the reader's life.

``window(x0, x1, pixels)`` uses record-local seconds, just like core.cpp's JSON.
With <=2 samples/pixel it returns original samples. Otherwise every occupied
pixel is an independent interval min/max envelope; do not join those intervals
as a sparsely sampled waveform. All indices and ticks remain Python integers.
No Qt, NumPy, interpolation, filtering or amplitude normalization is involved.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import math
import mmap
import os
from pathlib import Path
import stat
import struct
import zlib

_HEADER_BYTES = 4096
_RECORD = struct.Struct("<QQhhhHII")
_MAX_FILE_BYTES = 64 * 1024 * 1024
_U64_MAX = (1 << 64) - 1


class WaveformError(ValueError):
    """Invalid format, verification receipt, requested view, or conversion."""


class FileIdentityError(WaveformError):
    """The path no longer names the immutable file that the CLI verified."""


@dataclass(frozen=True)
class FileIdentity:
    path: str
    device: int
    inode: int
    size: int
    modified_ns: int
    change_or_birth_ns: int
    header_sha256: str


def _stat_key(value):
    # On Windows Python 3.12 stat and fstat can disagree on deprecated ctime;
    # birthtime is the consistent creation identity. POSIX ctime tracks edits.
    stamp = getattr(value, "st_birthtime_ns", value.st_ctime_ns) if os.name == "nt" else value.st_ctime_ns
    return (value.st_dev, value.st_ino, value.st_size,
            value.st_mtime_ns, stamp)


def _require(condition, message):
    if not condition:
        raise WaveformError(message)


def capture_file_identity(path):
    """Cheap pre-CLI snapshot: file ID, length, ns timestamps and 4 KiB header.

    This is an identity receipt, not an integrity verdict or a whole-file SHA.
    Symlinks are rejected. Ordinary modification/replacement invalidates it.
    """
    path = Path(os.path.abspath(os.fspath(path)))
    try:
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode):
            raise FileIdentityError("Capture must be a regular file, not a link")
        with path.open("rb") as handle:
            opened = os.fstat(handle.fileno())
            header = handle.read(_HEADER_BYTES)
            after = os.fstat(handle.fileno())
        named = path.lstat()
    except OSError as exc:
        raise FileIdentityError("Cannot identify capture file: " + str(exc)) from exc
    if not (_stat_key(before) == _stat_key(opened) == _stat_key(after) == _stat_key(named)):
        raise FileIdentityError("Capture changed while its identity was read")
    return FileIdentity(str(path), *_stat_key(after), hashlib.sha256(header).hexdigest())


@dataclass(frozen=True)
class Sample:
    time_s: float
    value: float
    index: int                 # Exact session-global raw-record index.
    tick: int                  # Exact 50 MHz raw-record timestamp.


@dataclass(frozen=True)
class EnvelopeInterval:
    start_time_s: float        # First actual sample in this pixel interval.
    end_time_s: float          # Last actual sample, inclusive.
    minimum: float
    maximum: float
    start_index: int
    end_index: int             # Inclusive session-global record index.
    minimum_index: int
    maximum_index: int
    minimum_time_s: float
    maximum_time_s: float
    sample_count: int


@dataclass(frozen=True)
class ChannelWindow:
    channel: int              # 0,1,2 = stored CH1,CH2,CH3 / ADC inputs 1,3,5.
    points: tuple[Sample, ...] = ()
    intervals: tuple[EnvelopeInterval, ...] = ()


@dataclass(frozen=True)
class WaveformWindow:
    mode: str                 # 'raw' or 'interval_envelope'.
    unit: str                 # CLI amplitude_unit, or 'V' when requested.
    series: tuple[ChannelWindow, ...]
    first_record: int          # File-local inclusive record offset.
    stop_record: int           # File-local exclusive record offset.
    requested_range: tuple[float, float]
    pixel_width: int

    @property
    def sample_count(self):
        return self.stop_record - self.first_record


class LocalWaveform:
    """An mmap-backed provider bound to one successful CLI analysis receipt.

    ``expected_identity`` is required: acquire it before starting that CLI.
    ``unit='analysis'`` reproduces signed gain/sensitivity conversion exactly;
    ``unit='V'`` returns ADC voltage (raw * adc_range_uv / 1e6 / 32768).
    ``domain`` ends at the last sample; ``duration_s`` includes one sample period.
    Close the provider on source change. It can also be used as a context manager.
    """
    def __init__(self, path, result, *, expected_identity):
        if not isinstance(expected_identity, FileIdentity):
            raise FileIdentityError("A pre-CLI FileIdentity receipt is required")
        self.path = Path(os.path.abspath(os.fspath(path)))
        self.identity = expected_identity
        self._file = None
        self._map = None
        self._cached_window = None
        try:
            if capture_file_identity(self.path) != self.identity:
                raise FileIdentityError("Capture differs from the pre-CLI identity")
            self._file = self.path.open("rb")
            if _stat_key(os.fstat(self._file.fileno())) != self._expected_stat():
                raise FileIdentityError("Capture replaced while opening")
            header = self._file.read(_HEADER_BYTES)
            self._parse_header(header)
            self._validate_result(result)
            self._map = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ)
            self._check_identity()
        except BaseException:
            self.close()
            raise

    def _expected_stat(self):
        i = self.identity
        return (i.device, i.inode, i.size, i.modified_ns, i.change_or_birth_ns)

    def _check_identity(self):
        if self._file is None or self._map is None:
            raise FileIdentityError("Waveform provider is closed")
        try:
            named = self.path.lstat()
            opened = os.fstat(self._file.fileno())
            if (not stat.S_ISREG(named.st_mode) or
                    _stat_key(named) != self._expected_stat() or
                    _stat_key(opened) != self._expected_stat() or
                    hashlib.sha256(self._map[:_HEADER_BYTES]).hexdigest() != self.identity.header_sha256):
                raise FileIdentityError("Capture was modified or replaced after CLI verification")
        except OSError as exc:
            raise FileIdentityError("Capture is no longer available: " + str(exc)) from exc

    def _parse_header(self, header):
        _require(len(header) == _HEADER_BYTES, "Truncated capture header")
        stream = header[:8] == b"CAPSTRM1"
        _require(stream or header[:8] == b"CAPDDR2\0", "Wrong CAPDDR2/CAPSTRM1 magic")
        (version, hbytes, rbytes, state, rate, clock, requested, count, payload,
         error, _hw, _dma, dropped, _ppr) = struct.unpack_from("<14I", header, 8)
        first_tick, last_tick = struct.unpack_from("<QQ", header, 64)
        channel_map, adc_range = struct.unpack_from("<Ii", header, 80)
        header_crc, = struct.unpack_from("<I", header, 100)
        checked = bytearray(header)
        checked[100:104] = b"\0" * 4
        _require(zlib.crc32(checked if stream else checked[:128]) == header_crc, "Header CRC mismatch")
        _require((version, hbytes, rbytes) == ((1 if stream else 2), 4096, 32), "Unsupported capture layout")
        _require(state == 2 and error == 0 and dropped == 0, "Capture is not READY/successful")
        _require(count > 0 and requested == count, "Incomplete record count")
        _require(payload == count * 32 and self.identity.size == 4096 + payload and
                 self.identity.size <= _MAX_FILE_BYTES, "Capture file length mismatch")
        _require(clock == 50_000_000 and 0 < rate <= 100_000 and clock % rate == 0,
                 "Unsupported capture timebase")
        _require(channel_map == 0x50301 and 0 < adc_range <= 10_000_000,
                 "Unsupported ADC map or range")
        self.global_first_index = 0
        self.segment_index = 0
        self.session_total_records = 0
        if stream:
            self.global_first_index, = struct.unpack_from("<Q", header, 104)
            self.segment_index, self.session_total_records = struct.unpack_from("<QQ", header, 128)
            target, flags, coverage = struct.unpack_from("<III", header, 144)
            _require(coverage == 4096 and not flags & ~1 and any(header[112:128]) and
                     not any(header[156:]), "Unsupported stream header extension")
            _require(0 < target <= rate * 40 and count <= target, "Invalid stream segment target")
            _require(self.global_first_index + count <= _U64_MAX and
                     (not self.session_total_records or self.global_first_index + count <= self.session_total_records),
                     "Invalid stream session record range")
        else:
            _require(not any(header[104:128]), "Nonzero reserved CAPDDR2 header field")
        self._step = clock // rate
        _require(first_tick + (count - 1) * self._step <= _U64_MAX and
                 last_tick == first_tick + (count - 1) * self._step, "Invalid capture timestamps")
        self.format = "CAPSTRM1" if stream else "CAPDDR2"
        self.protocol_version = version
        self.sample_rate_hz = rate
        self.clock_hz = clock
        self.record_count = count
        self.payload_bytes = payload
        self.first_tick, self.last_tick = first_tick, last_tick
        self.adc_range_uv = adc_range
        self.voltage_scale = adc_range / 1e6 / 32768.0
        self.domain = (0.0, (count - 1) / rate)
        self.duration_s = count / rate

    def _validate_result(self, result):
        _require(isinstance(result, dict) and result.get("schema") == "zynq_vibration.analysis.v1" and
                 result.get("integrity") == "passed", "Successful complete CLI analysis is required")
        source = result.get("source_path")
        _require(isinstance(source, str) and os.path.normcase(os.path.abspath(source)) ==
                 os.path.normcase(str(self.path)), "CLI analysis belongs to another path")
        expected = dict(protocol_version=self.protocol_version, sample_rate_hz=self.sample_rate_hz,
                        clock_hz=self.clock_hz, records=self.record_count, payload_bytes=self.payload_bytes,
                        first_tick=self.first_tick, last_tick=self.last_tick, adc_range_uv=self.adc_range_uv,
                        format=self.format, global_first_index=self.global_first_index,
                        segment_index=self.segment_index, session_total_records=self.session_total_records)
        header = result.get("header")
        _require(isinstance(header, dict) and all(type(header.get(k)) is int and header[k] == v
                 if isinstance(v, int) else header.get(k) == v for k, v in expected.items()),
                 "CLI analysis header does not match capture")
        _require(result.get("channel_adc_inputs") == [1, 3, 5], "CLI channel map does not match capture")
        cfg = result.get("configuration", {})
        _require(isinstance(cfg, dict), "Invalid CLI conversion configuration")
        sensitivity, gain = cfg.get("sensor_sensitivity_v_per_g"), cfg.get("analog_gain")
        _require(self._finite(sensitivity) and sensitivity > 0 and self._finite(gain) and abs(gain) > 1e-12,
                 "Invalid CLI gain or sensitivity")
        self.analysis_scale = self.voltage_scale / (gain * sensitivity)
        _require(math.isfinite(self.analysis_scale) and abs(self.analysis_scale) <= 1e6,
                 "CLI amplitude conversion exceeds numerical range")
        self.amplitude_unit = result.get("amplitude_unit")
        _require(self.amplitude_unit in ("g", "g_nominal_uncalibrated") and
                 (self.amplitude_unit == "g") == (cfg.get("calibration_confirmed") is True),
                 "CLI amplitude unit/calibration mismatch")

    @staticmethod
    def _finite(value):
        return type(value) in (int, float) and math.isfinite(value)

    def _record(self, local_index):
        record = _RECORD.unpack_from(self._map, 4096 + local_index * 32)
        index, tick, _c1, _c2, _c3, flags, period, _edge = record
        _require(index == self.global_first_index + local_index, "Record index discontinuity")
        _require(tick == self.first_tick + local_index * self._step, "Record timestamp discontinuity")
        _require(flags & 1 and not flags & ~31 and
                 (not flags & 2 or period > 0 and not flags & 4), "Invalid record flags/speed status")
        return record

    def window(self, x0, x1, pixel_width, *, channels=(0, 1, 2), unit="analysis"):
        """Return only samples whose local times are in inclusive [x0,x1].

        Out-of-file portions are clipped. The output has at most 2*pixels raw
        samples/channel or pixels independent min/max intervals/channel.
        Each envelope min/max is measured over ALL samples in that time bucket.
        """
        _require(self._finite(x0) and self._finite(x1) and x0 <= x1 and
                 math.isfinite(x1 - x0), "Invalid waveform time range")
        _require(type(pixel_width) is int and 1 <= pixel_width <= 32768, "Invalid pixel width")
        channels = tuple(channels)
        _require(len(channels) <= 3 and len(set(channels)) == len(channels) and
                 all(type(ch) is int and ch in (0, 1, 2) for ch in channels), "Invalid channel selection")
        _require(unit in ("analysis", "V"), "Unit must be analysis or V")
        self._check_identity()
        key = (x0, x1, pixel_width, channels, unit)
        if self._cached_window is not None and self._cached_window[0] == key:
            return self._cached_window[1]
        fs = self.sample_rate_hz
        # Permit one ulp of multiplication noise at an exact sample boundary.
        lo = (0 if x0 <= 0 else self.record_count if x0 > self.domain[1] else
              math.ceil(math.nextafter(x0 * fs, -math.inf)))
        hi = (0 if x1 < 0 else self.record_count if x1 >= self.domain[1] else
              math.floor(math.nextafter(x1 * fs, math.inf)) + 1)
        hi = max(lo, hi)
        scale = self.analysis_scale if unit == "analysis" else self.voltage_scale
        label = self.amplitude_unit if unit == "analysis" else "V"
        points = [[] for _ in channels]
        intervals = [[] for _ in channels]
        mode = "raw" if hi - lo <= pixel_width * 2 else "interval_envelope"
        if mode == "raw":
            for i in range(lo, hi):
                record = self._record(i)
                for out, channel in zip(points, channels):
                    out.append(Sample(i / fs, record[2 + channel] * scale, record[0], record[1]))
        else:
            # Integer boundaries are the exact inverse of the requested pixel
            # time intervals; unlike M4 point reduction these are NOT a polyline.
            width = x1 - x0
            start = lo
            for column in range(pixel_width):
                boundary = x0 + width * (column + 1) / pixel_width
                stop = (hi if column == pixel_width - 1 or boundary > self.domain[1] else
                        start if boundary <= 0 else max(start, min(hi,
                            math.ceil(math.nextafter(boundary * fs, -math.inf)))))
                if stop == start:
                    continue
                extrema = [[None, None, 0, 0] for _ in channels]
                for i in range(start, stop):
                    record = self._record(i)
                    for ext, channel in zip(extrema, channels):
                        value = record[2 + channel] * scale
                        if ext[0] is None or value < ext[0]: ext[0], ext[2] = value, i
                        if ext[1] is None or value > ext[1]: ext[1], ext[3] = value, i
                for out, (minimum, maximum, imin, imax) in zip(intervals, extrema):
                    out.append(EnvelopeInterval(start / fs, (stop - 1) / fs, minimum, maximum,
                        self.global_first_index + start, self.global_first_index + stop - 1,
                        self.global_first_index + imin, self.global_first_index + imax,
                        imin / fs, imax / fs, stop - start))
                start = stop
        self._check_identity()
        window = WaveformWindow(mode, label, tuple(ChannelWindow(ch, tuple(p), tuple(e))
            for ch, p, e in zip(channels, points, intervals)), lo, hi, (float(x0), float(x1)), pixel_width)
        self._cached_window = (key, window)
        return window

    def close(self):
        self._cached_window = None
        if self._map is not None:
            self._map.close()
            self._map = None
        if self._file is not None:
            self._file.close()
            self._file = None

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()
