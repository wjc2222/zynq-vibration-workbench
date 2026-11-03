#!/usr/bin/env python3
"""Synthetic host storage/ABI protocol QA; no SD, ADC or physical board claims."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import zlib
from datetime import datetime, timezone

ARGS = None
SCHEMA = "vibration.stream.v1"
BACKPRESSURE_CASES = []


class Client:
    def __init__(self, delay=1, fault=None, fixture=True):
        args = [ARGS.service]
        if fixture:
            args += ["--test-fixture", "--fixture-chunk-ms=" + str(delay)]
        if fault:
            args += ["--fixture-fault=" + fault]
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        self.q = queue.Queue()
        self.messages = []
        self.sequence = 0
        def read():
            for line in self.process.stdout:
                value = json.loads(line)
                self.messages.append(value)
                self.q.put(value)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def wait(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        for message in self.messages:
            if predicate(message):
                return message
        while time.monotonic() < deadline:
            message = self.q.get(timeout=max(0.01, deadline - time.monotonic()))
            if predicate(message):
                return message
        raise AssertionError("No matching message")

    def send(self, command, **fields):
        self.sequence += 1
        request = "r" + str(self.sequence)
        self.process.stdin.write((json.dumps(dict(schema=SCHEMA, request_id=request,
                                                 command=command, **fields)) + "\n").encode())
        self.process.stdin.flush()
        return self.wait(lambda m: m.get("type") == "response" and m.get("request_id") == request)

    def done(self, job):
        return self.wait(lambda m: m.get("event") == "session_done" and m.get("job_id") == job)

    def close(self):
        if self.process.poll() is None:
            try:
                self.send("shutdown")
            finally:
                self.process.stdin.close()
                self.process.wait(timeout=10)
        self.reader.join(timeout=2)
        error = self.process.stderr.read()
        self.process.stdout.close()
        self.process.stderr.close()
        assert self.process.returncode == 0, error


def plan(directory, mode="timed", seconds=3, rate=5000, segment=1):
    value = dict(mode=mode, sample_rate_hz=rate, pulses_per_rev=2,
                 output_directory=str(directory), segment_seconds=segment)
    if mode == "timed":
        value["duration_seconds"] = seconds
    return value


def validate_file(path):
    data = Path(path).read_bytes()
    header, raw = bytearray(data[:4096]), data[4096:]
    assert header[:8] == b"CAPSTRM1"
    expected_header_crc = struct.unpack_from("<I", header, 100)[0]
    struct.pack_into("<I", header, 100, 0)
    assert zlib.crc32(header) == expected_header_crc
    assert zlib.crc32(raw) == struct.unpack_from("<I", header, 96)[0]
    rate, count = struct.unpack_from("<I", header, 24)[0], struct.unpack_from("<I", header, 36)[0]
    first = struct.unpack_from("<Q", header, 104)[0]
    tick = struct.unpack_from("<Q", header, 64)[0]
    assert len(raw) == count * 32
    for i in range(count):
        assert struct.unpack_from("<QQ", raw, i * 32) == (first + i, tick + i * (50000000 // rate))
    return header, raw


class Tests(unittest.TestCase):
    def test_full_control_queue_cannot_deadlock_tail_or_shutdown(self):
        for close_mode in ("eof", "shutdown", "idle"):
            with self.subTest(close_mode=close_mode), tempfile.TemporaryDirectory(prefix="stream-full-controls-") as folder:
                sentinel = Path(folder, "user-original.txt")
                sentinel.write_text("preserve original")
                p = subprocess.Popen([ARGS.service, "--test-fixture", "--fixture-chunk-ms=5"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                start = dict(schema=SCHEMA, request_id="blocked-start", command="start",
                             plan=plan(folder, mode="continuous", segment=10))
                p.stdin.write((json.dumps(start) + "\n").encode())
                p.stdin.flush()
                deadline = time.monotonic() + 5
                while not any(Path(folder).glob(".*.partial")) and time.monotonic() < deadline:
                    time.sleep(.005)
                self.assertTrue(any(Path(folder).glob(".*.partial")), "Real raw writer must be active before flooding controls")
                requests = [dict(schema=SCHEMA, request_id="full-" + str(i), command="status") for i in range(350)]
                if close_mode == "shutdown":
                    requests.append(dict(schema=SCHEMA, request_id="shutdown", command="shutdown"))
                began = time.monotonic()
                try:
                    # No stdout reader is started: fill all 256 control slots,
                    # not merely the much smaller kernel stdout pipe.
                    p.stdin.write("".join(json.dumps(x) + "\n" for x in requests).encode())
                    p.stdin.flush()
                except BrokenPipeError:
                    pass  # An already-failed transport may exit proactively.
                if close_mode == "eof":
                    try:
                        p.stdin.close()
                    except BrokenPipeError:
                        pass
                p.wait(timeout=8)
                self.assertLess(time.monotonic() - began, 7)
                output, error = p.stdout.read(), p.stderr.read()
                self.assertEqual(p.returncode, 2, error)
                self.assertIn(b"TRANSPORT_BACKPRESSURE", error)
                self.assertIn(b"reliable control delivery is unconfirmed", error)
                # stdout can contain an interrupted preview line; a failure
                # exit plus an immutable SD error notice must make that loss
                # explicit instead of inventing a delivered successful final.
                self.assertGreater(len(output), 0)
                notices = list(Path(folder).glob("*.transport-error.json"))
                self.assertEqual(len(notices), 1)
                notice = json.loads(notices[0].read_text())
                self.assertEqual(notice["state"], "failed")
                self.assertFalse(notice["session_complete"])
                self.assertFalse(notice["control_delivery_confirmed"])
                self.assertEqual(notice["transport"]["high_controls"], 256)
                self.assertTrue(notice["all_received_records_saved"])
                files = sorted(Path(folder).glob("*/data.BIN"))
                self.assertGreaterEqual(len(files), 1)
                durable = sum(len(validate_file(path)[1]) // 32 for path in files)
                self.assertGreater(durable, 0)
                self.assertEqual(durable, int(notice["durable_records"]))
                self.assertEqual(durable, int(notice["records_received"]))
                self.assertEqual(sentinel.read_text(), "preserve original")
                self.assertFalse(any(Path(folder).glob(".*.partial")))
                BACKPRESSURE_CASES.append(dict(close_mode=close_mode, stdout_read_during_capture=False,
                                               exit_code=p.returncode, elapsed_seconds=time.monotonic() - began,
                                               control_high_water=notice["transport"]["high_controls"],
                                               durable_records=durable, tail_segments=len(files),
                                               session_complete=notice["session_complete"],
                                               control_delivery_confirmed=notice["control_delivery_confirmed"],
                                               error_code=notice["transport"]["error_code"],
                                               failure_notice_sha256=hashlib.sha256(notices[0].read_bytes()).hexdigest()))
                if not p.stdin.closed:
                    try:
                        p.stdin.close()
                    except BrokenPipeError:
                        pass
                p.stdout.close()
                p.stderr.close()

    def test_hardware_test_is_explicit_and_never_a_host_fixture(self):
        rejected = subprocess.run([ARGS.service, "--test-fixture", "--hardware-test"], capture_output=True)
        self.assertEqual(rejected.returncode, 2)
        request = dict(schema=SCHEMA, request_id="hardware-test-caps", command="capabilities")
        queried = subprocess.run([ARGS.service, "--hardware-test"], input=(json.dumps(request) + "\n").encode(), capture_output=True)
        self.assertEqual(queried.returncode, 0)
        caps = json.loads(queried.stdout)["capabilities"]
        self.assertEqual(caps["backend"], "linux_ring")
        self.assertEqual(caps["source"], "hardware_test")
        self.assertTrue(caps["synthetic"])
        if not Path("/dev/ax7z020-capture").exists():
            self.assertFalse(caps["available"])
            self.assertFalse(caps["hardware_confirmed"])

    def test_timed_session_exceeds_legacy_ddr_capacity(self):
        with tempfile.TemporaryDirectory(prefix="stream-long-") as folder:
            c = Client(delay=0)
            try:
                start = c.send("start", plan=plan(folder, seconds=45, rate=50000, segment=10))
                done = c.done(start["job"]["job_id"])
                self.assertEqual(done["state"], "completed")
                self.assertEqual(done["durable_records"], "2250000")
                files = sorted(Path(folder).glob("*/data.BIN"))
                self.assertEqual(len(files), 5)
                self.assertGreater(sum(p.stat().st_size for p in files), 64 * 1024 * 1024)
                for i, segment in enumerate(files):
                    with segment.open("rb") as stream:
                        header = stream.read(4096)
                    self.assertEqual(struct.unpack_from("<Q", header, 104)[0], i * 500000)
                    self.assertLessEqual(segment.stat().st_size, 64 * 1024 * 1024)
                h, raw = validate_file(files[-1])
                self.assertEqual(len(raw) // 32, 250000)
                self.assertEqual(struct.unpack_from("<Q", raw)[0], 2000000)
            finally:
                c.close()

    def test_timed_cross_segments_and_original_indices(self):
        with tempfile.TemporaryDirectory(prefix="stream-timed-") as folder:
            c = Client()
            try:
                caps = c.send("capabilities")["capabilities"]
                self.assertTrue(caps["available"])
                self.assertEqual(caps["preview_max_records"], 2000)
                started = c.send("start", plan=plan(folder))
                self.assertTrue(started["ok"], started)
                job = started["job"]["job_id"]
                done = c.done(job)
                self.assertEqual(done["state"], "completed")
                self.assertTrue(done["session_complete"])
                self.assertTrue(done["target_complete"])
                self.assertEqual(done["records_persisted"], "15000")
                segments = sorted(Path(folder).glob("*/data.BIN"))
                self.assertEqual(len(segments), 3)
                previous_end = None
                session = None
                for segment in segments:
                    h, raw = validate_file(segment)
                    first = struct.unpack_from("<Q", h, 104)[0]
                    if previous_end is not None:
                        self.assertEqual(first, previous_end)
                    previous_end = first + len(raw) // 32
                    if session is not None:
                        self.assertEqual(h[112:128], session)
                    session = h[112:128]
                    meta = json.loads(segment.with_suffix(".meta.json").read_text())
                    self.assertTrue(meta["synthetic"])
                    analyzed = subprocess.run([ARGS.analyze, str(segment)], capture_output=True)
                    self.assertEqual(analyzed.returncode, 0, analyzed.stderr)
                previews = [m for m in c.messages if m.get("event") == "preview"]
                self.assertGreater(len(previews), 0)
                for m in previews:
                    raw = base64.b64decode(m["raw_base64"], validate=True)
                    self.assertEqual(zlib.crc32(raw), m["crc32"])
                    self.assertEqual(struct.unpack_from("<QQ", raw), (int(m["global_first_index"]), int(m["first_tick"])))
                    self.assertEqual(m["source"], "test_fixture")
                    self.assertLessEqual(m["record_count"], 2000)
                self.assertFalse(any(Path(folder).glob(".*partial")))
            finally:
                c.close()

    def test_continuous_stop_publishes_partial_tail(self):
        with tempfile.TemporaryDirectory(prefix="stream-stop-") as folder:
            c = Client(delay=10)
            try:
                started = c.send("start", plan=plan(folder, mode="continuous", segment=10))
                self.assertTrue(started["ok"])
                job = started["job"]["job_id"]
                c.wait(lambda m: m.get("event") == "preview" and int(m["global_first_index"]) >= 400)
                self.assertTrue(c.send("stop")["ok"])
                done = c.done(job)
                self.assertEqual(done["state"], "stopped")
                self.assertTrue(done["session_complete"])
                self.assertEqual(done["records_received"], done["durable_records"])
                files = list(Path(folder).glob("*/data.BIN"))
                self.assertEqual(len(files), 1)
                h, raw = validate_file(files[0])
                self.assertGreater(len(raw), 0)
                self.assertLess(len(raw) // 32, 50000)
                self.assertEqual(struct.unpack_from("<Q", h, 136)[0], 0)
            finally:
                c.close()

    def test_eof_preserves_tail(self):
        with tempfile.TemporaryDirectory(prefix="stream-eof-") as folder:
            c = Client(delay=10)
            started = c.send("start", plan=plan(folder, mode="continuous", segment=10))
            c.wait(lambda m: m.get("event") == "preview")
            c.process.stdin.close()
            c.process.wait(timeout=10)
            c.reader.join(timeout=2)
            done = c.done(started["job"]["job_id"])
            self.assertEqual(done["state"], "stopped")
            self.assertTrue(done["session_complete"])
            self.assertEqual(len(list(Path(folder).glob("*/data.BIN"))), 1)
            c.close()

    def test_storage_failure_preserves_published_prefix(self):
        with tempfile.TemporaryDirectory(prefix="stream-space-") as folder:
            sentinel = Path(folder, "user-original.txt")
            sentinel.write_text("keep original")
            c = Client(fault="storage_after_first")
            try:
                started = c.send("start", plan=plan(folder))
                done = c.done(started["job"]["job_id"])
                self.assertEqual(done["state"], "failed")
                self.assertEqual(done["error_code"], "LOW_SPACE")
                self.assertFalse(done["session_complete"])
                self.assertEqual(done["durable_records"], "5000")
                files = list(Path(folder).glob("*/data.BIN"))
                self.assertEqual(len(files), 1)
                validate_file(files[0])
                self.assertEqual(sentinel.read_text(), "keep original")
                self.assertFalse(any(Path(folder).glob(".*partial")))
            finally:
                c.close()

    def test_short_write_never_publishes_incomplete_pair(self):
        with tempfile.TemporaryDirectory(prefix="stream-short-") as folder:
            c = Client(fault="short_write")
            try:
                started = c.send("start", plan=plan(folder))
                done = c.done(started["job"]["job_id"])
                self.assertEqual(done["state"], "failed")
                self.assertEqual(done["durable_records"], "0")
                self.assertEqual(list(Path(folder).iterdir()), [])
            finally:
                c.close()

    def test_gap_is_error_not_reindexed(self):
        with tempfile.TemporaryDirectory(prefix="stream-gap-") as folder:
            c = Client(fault="device_gap")
            try:
                started = c.send("start", plan=plan(folder))
                done = c.done(started["job"]["job_id"])
                self.assertEqual(done["state"], "failed")
                self.assertEqual(done["error_code"], "DATA_ERROR")
                self.assertEqual(done["durable_records"], "5000")
            finally:
                c.close()

    def test_extended_header_crc_tamper_and_recomputed_index_tamper(self):
        with tempfile.TemporaryDirectory(prefix="stream-crc-") as folder:
            c = Client()
            try:
                started = c.send("start", plan=plan(folder, seconds=2))
                c.done(started["job"]["job_id"])
                segment = sorted(Path(folder).glob("*/data.BIN"))[1]
                original = bytearray(segment.read_bytes())
                for offset in (104, 112, 128, 136, 144, 148, 152, 300):
                    corrupt = bytearray(original)
                    corrupt[offset] ^= 1
                    target = Path(folder, "tamper.BIN")
                    target.write_bytes(corrupt)
                    result = subprocess.run([ARGS.analyze, str(target)], capture_output=True)
                    self.assertNotEqual(result.returncode, 0, offset)
                corrupt = bytearray(original)
                struct.pack_into("<Q", corrupt, 104, 0)
                struct.pack_into("<I", corrupt, 100, 0)
                struct.pack_into("<I", corrupt, 100, zlib.crc32(corrupt[:4096]))
                target.write_bytes(corrupt)
                result = subprocess.run([ARGS.analyze, str(target)], capture_output=True)
                self.assertNotEqual(result.returncode, 0)
            finally:
                c.close()

    def test_bounds_low_space_symlink_and_long_duration(self):
        with tempfile.TemporaryDirectory(prefix="stream-plan-") as folder:
            c = Client(fault="low_space")
            try:
                self.assertEqual(c.send("start", plan=plan(folder))["error_code"], "LOW_SPACE")
            finally:
                c.close()
            c = Client(delay=10)
            try:
                linked = Path(folder, "linked")
                linked.symlink_to(folder, target_is_directory=True)
                self.assertFalse(c.send("start", plan=plan(linked))["ok"])
                for override in ({"duration_seconds": 86401}, {"segment_seconds": 41},
                                 {"mode": "continuous"}, {"sample_rate_hz": 12345}):
                    p = plan(folder)
                    p.update(override)
                    self.assertFalse(c.send("start", plan=p)["ok"], p)
                result = c.send("start", plan=plan(folder, seconds=86400, rate=50000))
                self.assertTrue(result["ok"])
                self.assertEqual(result["job"]["total_records"], "4320000000")
                c.send("stop")
                self.assertEqual(c.done(result["job"]["job_id"])["state"], "stopped")
            finally:
                c.close()

    def test_realtime_fixture_25hz_and_preview_backpressure(self):
        with tempfile.TemporaryDirectory(prefix="stream-backpressure-") as folder:
            c = Client(delay=40)
            try:
                before = time.monotonic()
                result = c.send("start", plan=plan(folder, seconds=1, rate=50000))
                done = c.done(result["job"]["job_id"])
                elapsed = time.monotonic() - before
                self.assertGreaterEqual(elapsed, .9)
                previews = [m for m in c.messages if m.get("event") == "preview"]
                self.assertGreaterEqual(len(previews), 20)
                self.assertEqual(done["durable_records"], "50000")
            finally:
                c.close()
            # Do not consume stdout until capture has finished. Full stdout
            # pipes can replace UI frames but must not stall raw persistence.
            p = subprocess.Popen([ARGS.service, "--test-fixture", "--fixture-chunk-ms=1"],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            request = dict(schema=SCHEMA, request_id="blocked", command="start", plan=plan(folder, seconds=3))
            p.stdin.write((json.dumps(request) + "\n").encode())
            p.stdin.flush()
            deadline = time.monotonic() + 5
            while len(list(Path(folder).glob("*/data.BIN"))) < 4 and time.monotonic() < deadline:
                time.sleep(.02)
            self.assertEqual(len(list(Path(folder).glob("*/data.BIN"))), 4)
            stdout, stderr = p.communicate((json.dumps(dict(schema=SCHEMA, request_id="end", command="shutdown")) + "\n").encode(), timeout=10)
            self.assertEqual(p.returncode, 0, stderr)
            events = [json.loads(line) for line in stdout.splitlines()]
            self.assertEqual(len([m for m in events if m.get("event") == "segment_ready"]), 3)
            self.assertTrue(any(m.get("event") == "session_done" and m["session_complete"] for m in events))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", required=True)
    parser.add_argument("--analyze", required=True)
    parser.add_argument("--report")
    ARGS, remaining = parser.parse_known_args()
    program = unittest.main(argv=[__file__] + remaining, exit=False)
    if ARGS.report:
        root = Path(__file__).resolve().parents[3]
        sources = ["software/streaming/stream_service.cpp", "software/streaming/stream_output.cpp",
                   "software/streaming/stream_output.hpp", "software/streaming/CMakeLists.txt",
                   "software/streaming/tests/test_stream.py", "software/core/src/core.cpp",
                   "software/core/include/vibration/core.hpp", "hardware/combined/include/ax7z020_capture_stream.h"]
        report = dict(schema="vibration.stream.host-qa.v1", synthetic=True,
                      recorded_at=datetime.now(timezone.utc).isoformat(),
                      success=program.result.wasSuccessful(), tests_run=program.result.testsRun,
                      failures=[dict(test=str(test), traceback=trace) for test, trace in program.result.failures],
                      errors=[dict(test=str(test), traceback=trace) for test, trace in program.result.errors],
                      control_saturation_cases=BACKPRESSURE_CASES,
                      service_sha256=hashlib.sha256(Path(ARGS.service).read_bytes()).hexdigest(),
                      analyze_sha256=hashlib.sha256(Path(ARGS.analyze).read_bytes()).hexdigest(),
                      source_sha256={name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in sources},
                      unverified=["physical SD throughput and power-loss behavior", "physical ADC and sensor inputs",
                                  "UART preview rate", "HDMI and touch", "calibration"])
        Path(ARGS.report).write_text(json.dumps(report, indent=2) + "\n")
    raise SystemExit(0 if program.result.wasSuccessful() else 1)
