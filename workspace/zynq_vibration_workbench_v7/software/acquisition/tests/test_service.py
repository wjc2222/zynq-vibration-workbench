#!/usr/bin/env python3
"""Host-only integration QA. Never opens a real capture device.

All generated BINs are synthetic fixtures in TemporaryDirectory. Real driver,
board, ADC, SPEED, SD and touch acceptance remain separate.
"""
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

SCHEMA = "vibration.capture.v2"
ARGS = None


class Client:
    def __init__(self, fixture=True, fault=None, hardware_self_test=False):
        argv = [ARGS.service]
        if fixture:
            argv.append("--test-fixture")
        if fault:
            argv.append("--fixture-fault=" + fault)
        if hardware_self_test:
            argv.append("--hardware-self-test")
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE)
        self.q = queue.Queue()
        self.events = []
        self.all = []
        self.sequence = 0
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.p.stdout:
            try:
                value = json.loads(line)
                assert value["schema"] == SCHEMA
                assert value["type"] in ("response", "event")
            except Exception as error:
                value = {"bad_stdout": repr(line), "error": str(error)}
            self.all.append(value)
            self.q.put(value)

    def raw(self, data):
        self.p.stdin.write(data)
        self.p.stdin.flush()

    def send(self, command, **fields):
        self.sequence += 1
        request_id = "r" + str(self.sequence)
        self.raw((json.dumps(dict(schema=SCHEMA, request_id=request_id,
                                  command=command, **fields)) + "\n").encode())
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            message = self.q.get(timeout=max(0.1, deadline - time.monotonic()))
            if message.get("request_id") == request_id:
                return message
            assert "bad_stdout" not in message, message
            self.events.append(message)
        raise AssertionError("No response")

    def terminal(self, job_id):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            for message in self.events:
                if (message.get("job_id") == job_id and
                    message.get("state") in ("completed", "cancelled", "failed")):
                    self.events.remove(message)
                    return message
            message = self.q.get(timeout=max(0.1, deadline - time.monotonic()))
            assert "bad_stdout" not in message, message
            self.events.append(message)
        raise AssertionError("No terminal event")

    def close(self):
        if self.p.poll() is None:
            self.send("shutdown")
            self.p.stdin.close()
        self.p.wait(timeout=10)
        self.reader.join(timeout=2)
        errors = self.p.stderr.read().decode(errors="replace")
        assert self.p.returncode == 0, errors
        assert not [x for x in self.all if "bad_stdout" in x], self.all


class CaptureServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="capture-service-qa-")
        self.directory = Path(self.temp.name)
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            client.close()
        self.temp.cleanup()

    def client(self, **kwargs):
        c = Client(**kwargs)
        self.clients.append(c)
        return c

    def plan(self, **overrides):
        return dict(sample_rate_hz=5000, duration_seconds=1, pulses_per_rev=1,
                    output_directory=str(self.directory), **overrides)

    def start(self, c, plan=None, preview=False):
        response = c.send("start", plan=plan or self.plan(), **({"preview":True} if preview else {}))
        self.assertTrue(response["ok"], response)
        return response["data"]["job_id"]

    def assert_clean(self):
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_01_no_device_never_becomes_fixture(self):
        if Path("/dev/ax7z020-capture").exists():
            self.skipTest("No real device access authorized in host QA")
        c = self.client(fixture=False)
        caps = c.send("capabilities")["data"]
        self.assertFalse(caps["available"])
        self.assertFalse(caps["hardware_confirmed"])
        self.assertFalse(caps["preview_available"])
        self.assertEqual(caps["backend"], "linux_device")
        validation = c.send("validate_plan", plan=self.plan())["data"]
        self.assertTrue(validation["valid"])
        self.assertFalse(validation["ready"])
        result = c.send("start", plan=self.plan())
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"]["code"], "UNAVAILABLE")
        self.assert_clean()

    def test_02_success_four_rates_unique_files_crc_and_locked_source(self):
        c = self.client()
        caps = c.send("capabilities")["data"]
        self.assertTrue(caps["available"])
        self.assertFalse(caps["hardware_confirmed"])
        self.assertTrue(caps["synthetic"])
        self.assertTrue(caps["preview_available"])
        self.assertEqual(caps["preview_line_max_bytes"], 196608)
        paths = []
        for rate in (50000, 25000, 10000, 5000):
            plan = self.plan()
            plan["sample_rate_hz"] = rate
            plan["pulses_per_rev"] = 3
            job = self.start(c, plan, preview=True)
            result = c.terminal(job)
            self.assertEqual(result["state"], "completed", result)
            self.assertFalse(result["hardware_confirmed"])
            self.assertTrue(result["synthetic"])
            path = Path(result["output_path"])
            self.assertNotIn(path, paths)
            paths.append(path)
            blob = path.read_bytes()
            self.assertEqual(len(blob), 4096 + rate * 32)
            self.assertEqual(blob[:8], b"CAPDDR2\0")
            self.assertEqual(struct.unpack_from("<I", blob, 24)[0], rate)
            self.assertEqual(struct.unpack_from("<I", blob, 60)[0], 3)
            head = bytearray(blob[:128])
            expected_head = struct.unpack_from("<I", head, 100)[0]
            head[100:104] = bytes(4)
            self.assertEqual(zlib.crc32(head), expected_head)
            self.assertEqual(zlib.crc32(blob[4096:]), struct.unpack_from("<I", blob, 96)[0])
            meta = json.loads(path.with_suffix(".meta.json").read_text())
            self.assertTrue(meta["synthetic"])
            self.assertEqual(meta["source_kind"], "synthetic")
            self.assertFalse(meta["calibration_confirmed"])
            self.assertFalse(meta["mechanical_vibration_confirmed"])
            self.assertEqual(meta["capture_parameters"]["pulses_per_rev"], 3)
            frames = [x for x in c.all if x.get("event") == "preview" and x.get("job_id") == job]
            self.assertGreaterEqual(len(frames), 10, "40ms fixture produces continuous live frames")
            end = seq = 0
            generation = frames[0]["generation"]
            self.assertRegex(generation, r"^[1-9][0-9]{0,19}$")
            for frame in frames:
                raw = base64.b64decode(frame["raw_base64"], validate=True)
                first, count = frame["first_record"], frame["record_count"]
                self.assertEqual(len(raw), count * 32)
                self.assertLessEqual(count, 4096)
                self.assertEqual(frame["generation"], generation)
                self.assertGreater(frame["frame_seq"], seq)
                self.assertGreaterEqual(first, end)
                self.assertEqual(frame["dropped_preview_records"], first - end)
                self.assertEqual(frame["crc32"], zlib.crc32(raw))
                self.assertEqual(raw, blob[4096+first*32:4096+(first+count)*32])
                self.assertEqual(frame["source"], "test_fixture")
                self.assertTrue(frame["synthetic"])
                self.assertFalse(frame["hardware_confirmed"])
                self.assertEqual((frame["ppr"], frame["sample_rate_hz"], frame["requested_records"]), (3, rate, rate))
                self.assertEqual(frame["validity_flags"] & 27, 27)
                self.assertGreaterEqual(frame["committed_records"], first + count)
                self.assertLessEqual(frame["committed_records"], rate)
                self.assertLessEqual(len(json.dumps(frame).encode()), 196608)
                for i in range(count):
                    index, tick, ch1, ch2, ch3, flags, period, edges = struct.unpack_from("<QQhhhHII", raw, i*32)
                    self.assertEqual(index, first+i)
                    self.assertEqual(tick, 100000+(first+i)*(50000000//rate))
                    self.assertEqual(flags, 5)
                end, seq = first + count, frame["frame_seq"]
            terminal_index = next(i for i, x in enumerate(c.all) if x.get("job_id") == job and x.get("state") == "completed")
            self.assertFalse(any(x.get("event") == "preview" and x.get("job_id") == job for x in c.all[terminal_index+1:]))
        self.assertEqual(len(list(self.directory.iterdir())), 8)
        if ARGS.analyze:
            result = subprocess.run([ARGS.analyze, str(paths[-1]), "--signal", "vibration",
                                     "--calibrated", "--fft-size", "1024"],
                                    capture_output=True, check=True, timeout=20)
            analysis = json.loads(result.stdout)
            self.assertTrue(analysis["source_metadata"]["synthetic"])
            self.assertEqual(analysis["source_metadata"]["status"], "synthetic_sidecar")

    def test_03_invalid_parameters_and_schema(self):
        c = self.client()
        for field, values in {"sample_rate_hz": [12345, "5000", True, None],
                              "duration_seconds": [0, 41, 1.1, "1"],
                              "pulses_per_rev": [0, 4097, 1.5, False]}.items():
            for value in values:
                p = self.plan();p[field] = value
                r = c.send("validate_plan", plan=p)
                self.assertFalse(r["ok"], (field, value, r))
        p = self.plan();p["output_directory"] = "relative"
        self.assertFalse(c.send("start", plan=p)["ok"])
        p["output_directory"] = str(self.directory / "missing")
        self.assertFalse(c.send("start", plan=p)["ok"])
        p = self.plan();p["hardware_confirmed"] = True
        self.assertFalse(c.send("start", plan=p)["ok"])
        self.assertFalse(c.send("capabilities", extra=True)["ok"])
        for value in ("true", 1, None, [], {}):
            self.assertFalse(c.send("start", plan=self.plan(), preview=value)["ok"])
        self.assertFalse(c.send("validate_plan", plan=self.plan(), preview=True)["ok"])
        self.assertFalse(c.send("status", job_id="foreign")["ok"])
        self.assertFalse(c.send("cancel", job_id="foreign")["ok"])
        self.assert_clean()

    def test_04_duplicate_keys_ids_and_long_line_recovery(self):
        c = self.client()
        c.raw(b'{"schema":"vibration.capture.v2","request_id":"dup","command":"capabilities","command":"shutdown"}\n')
        self.assertEqual(c.q.get(timeout=5)["error"]["code"], "INVALID_REQUEST")
        self.assertTrue(c.send("capabilities")["ok"])
        c.raw(b'{"schema":"vibration.capture.v2","request_id":"r1","command":"capabilities"}\n')
        self.assertEqual(c.q.get(timeout=5)["error"]["code"], "DUPLICATE_REQUEST")
        c.raw(b"x" * 90000 + b"\n")
        self.assertEqual(c.q.get(timeout=5)["error"]["code"], "LINE_TOO_LONG")
        c.raw(b"{}broken\n")
        self.assertEqual(c.q.get(timeout=5)["error"]["code"], "INVALID_JSON")
        self.assertTrue(c.send("capabilities")["ok"])

    def test_05_concurrency_cancel_no_success_file_and_reuse(self):
        c = self.client()
        job = self.start(c)
        second = c.send("start", plan=self.plan())
        self.assertFalse(second["ok"])
        self.assertEqual(second["error"]["code"], "BUSY")
        self.assertEqual(c.send("status", job_id=job)["data"]["job_id"], job)
        self.assertTrue(c.send("cancel", job_id=job)["data"]["accepted"])
        result = c.terminal(job)
        self.assertEqual(result["state"], "cancelled", result)
        self.assertEqual(result["output_path"], "")
        self.assert_clean()
        self.assertEqual(c.terminal(self.start(c))["state"], "completed")

    def test_06_driver_record_crc_and_publication_failures(self):
        for fault in ("driver", "index", "tick", "flags", "short", "crc", "publication"):
            with self.subTest(fault=fault):
                c = self.client(fault=fault)
                result = c.terminal(self.start(c))
                self.assertEqual(result["state"], "failed", result)
                self.assertEqual(result["output_path"], "")
                self.assertEqual(result["error_code"], "DEVICE_ERROR" if fault == "driver" else
                                 "FILE_ERROR" if fault == "publication" else "INTEGRITY_ERROR")
                # The worker performs scoped cleanup before its terminal event.
                self.assert_clean()

    def test_07_symlink_and_original_file_protection(self):
        original = self.directory / "user-input.BIN"
        original.write_bytes(b"original-user-data")
        original_hash = hashlib.sha256(original.read_bytes()).hexdigest()
        link = self.directory / "linked-dir"
        link.symlink_to(self.directory, target_is_directory=True)
        c = self.client()
        p = self.plan();p["output_directory"] = str(link)
        self.assertFalse(c.send("start", plan=p)["ok"])
        p["output_directory"] = str(self.directory) + "/../" + self.directory.name
        self.assertFalse(c.send("start", plan=p)["ok"])
        self.assertEqual(c.terminal(self.start(c))["state"], "completed")
        self.assertTrue(link.is_symlink())
        self.assertEqual(hashlib.sha256(original.read_bytes()).hexdigest(), original_hash)

    def test_08_eof_cancels_and_joins(self):
        c = self.client()
        job = self.start(c)
        c.p.stdin.close()
        c.p.wait(timeout=10)
        result = c.terminal(job)
        self.assertEqual(result["state"], "cancelled")
        self.assert_clean()

    def test_09_uapi_copy_is_byte_identical(self):
        root = Path(__file__).resolve().parents[3]
        for name in ("ax7z020_capture.h", "ax7z020_capture_preview.h"):
            service_header = root / "software/acquisition/include/vibration" / name
            shared_header = root / "hardware/combined/include" / name
            self.assertEqual(service_header.read_bytes(), shared_header.read_bytes())

    def test_10_explicit_hardware_self_test_requires_real_device(self):
        invalid = subprocess.run([ARGS.service, "--test-fixture", "--hardware-self-test"],
                                 input=b"", capture_output=True, timeout=5)
        self.assertEqual(invalid.returncode, 2)
        self.assertEqual(invalid.stdout, b"")
        if Path("/dev/ax7z020-capture").exists():
            self.skipTest("No real device access authorized in host QA")
        c = self.client(fixture=False, hardware_self_test=True)
        caps = c.send("capabilities")["data"]
        self.assertEqual(caps["requested_source"], "hardware_self_test")
        self.assertEqual(caps["actual_source"], "unknown")
        self.assertFalse(caps["available"])
        self.assertFalse(caps["hardware_confirmed"])
        self.assertFalse(caps["synthetic"])
        result = c.send("start", plan=self.plan())
        self.assertEqual(result["error"]["code"], "UNAVAILABLE")
        self.assert_clean()

    def test_11_preview_unavailable_is_explicit_and_full_capture_still_works(self):
        c = self.client(fault="preview_unavailable")
        caps = c.send("capabilities")["data"]
        self.assertFalse(caps["preview_available"])
        self.assertTrue(caps["preview_reason"])
        job = self.start(c, preview=True)
        result = c.terminal(job)
        self.assertEqual(result["state"], "completed")
        self.assertFalse(result["preview_available"])
        self.assertFalse(any(x.get("event") == "preview" for x in c.all))
        blob = Path(result["output_path"]).read_bytes()
        self.assertEqual(zlib.crc32(blob[4096:]), struct.unpack_from("<I", blob, 96)[0])

    def test_12_preview_crc_fault_never_corrupts_saved_raw(self):
        c = self.client(fault="preview_crc")
        job = self.start(c, preview=True)
        result = c.terminal(job)
        self.assertEqual(result["state"], "completed")
        frames = [x for x in c.all if x.get("event") == "preview"]
        self.assertGreater(len(frames), 5)
        self.assertTrue(all(zlib.crc32(base64.b64decode(x["raw_base64"])) != x["crc32"] for x in frames))
        blob = Path(result["output_path"]).read_bytes()
        self.assertEqual(zlib.crc32(blob[4096:]), struct.unpack_from("<I", blob, 96)[0])

    def test_13_slow_stdout_skips_preview_without_losing_saved_records(self):
        # No reader drains the pipe for 650ms. The acquisition worker must keep
        # its own pace; latest-only preview replaces unsent frames in bounded RAM.
        p = subprocess.Popen([ARGS.service, "--test-fixture"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        plan = self.plan();plan.update(sample_rate_hz=50000, duration_seconds=2)
        p.stdin.write((json.dumps(dict(schema=SCHEMA, request_id="start", command="start", plan=plan, preview=True))+"\n").encode());p.stdin.flush()
        try:
            response = json.loads(p.stdout.readline())
            self.assertTrue(response["ok"])
            time.sleep(0.65)
            resident_kib = int(next(x.split()[1] for x in Path(f"/proc/{p.pid}/status").read_text().splitlines() if x.startswith("VmRSS:")))
            self.assertLess(resident_kib, 96*1024)
            messages = []
            while True:
                line = p.stdout.readline()
                self.assertLessEqual(len(line)-1, 196608)
                message = json.loads(line);messages.append(message)
                if message.get("state") in ("completed", "failed", "cancelled"):
                    break
            self.assertEqual(messages[-1]["state"], "completed")
            frames = [x for x in messages if x.get("event") == "preview"]
            self.assertTrue(any(x["dropped_preview_records"] > 0 for x in frames))
            end = 0
            for frame in frames:
                self.assertEqual(frame["dropped_preview_records"], frame["first_record"]-end)
                end = frame["first_record"]+frame["record_count"]
            blob = Path(messages[-1]["output_path"]).read_bytes()
            self.assertEqual(len(blob), 4096+100000*32)
            self.assertEqual(zlib.crc32(blob[4096:]), struct.unpack_from("<I", blob, 96)[0])
            self.assertEqual(struct.unpack_from("<Q", blob, 4096+99999*32)[0], 99999)
        finally:
            p.stdin.close();p.wait(timeout=5);p.stdout.close();p.stderr.close()

    def test_14_unread_stdout_does_not_block_commit_or_eof_shutdown(self):
        p = subprocess.Popen([ARGS.service, "--test-fixture"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        plan = self.plan();plan.update(sample_rate_hz=50000)
        p.stdin.write((json.dumps(dict(schema=SCHEMA, request_id="unread", command="start", plan=plan, preview=True))+"\n").encode());p.stdin.flush()
        try:
            self.assertTrue(json.loads(p.stdout.readline())["ok"])
            deadline = time.monotonic()+4
            while not list(self.directory.glob("*.BIN")) and time.monotonic()<deadline:
                time.sleep(0.02)
            paths = list(self.directory.glob("*.BIN"))
            self.assertEqual(len(paths), 1, "Commit must finish while stdout is blocked")
            self.assertEqual(paths[0].stat().st_size, 4096+50000*32)
            began = time.monotonic();p.stdin.close();p.wait(timeout=3)
            self.assertLess(time.monotonic()-began, 2)
            self.assertEqual(p.returncode, 0)
        finally:
            if not p.stdin.closed:p.stdin.close()
            if p.poll() is None:p.kill();p.wait(timeout=3)
            p.stdout.close();p.stderr.close()

    def test_15_v2_default_and_explicit_false_do_not_push_preview(self):
        c = self.client()
        for fields in ({}, {"preview":False}):
            response = c.send("start", plan=self.plan(), **fields)
            self.assertTrue(response["ok"])
            job = response["data"]["job_id"]
            self.assertFalse(response["data"]["preview_available"])
            result = c.terminal(job)
            self.assertEqual(result["state"], "completed")
            self.assertFalse(any(x.get("job_id")==job and x.get("event")=="preview" for x in c.all))
            self.assertTrue(all(len(json.dumps(x).encode())<=8192 for x in c.all if x.get("job_id")==job))
            blob = Path(result["output_path"]).read_bytes()
            self.assertEqual(zlib.crc32(blob[4096:]), struct.unpack_from("<I", blob, 96)[0])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", required=True)
    parser.add_argument("--analyze")
    parser.add_argument("--report")
    ARGS = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(CaptureServiceTests))
    report = dict(schema="capture-service-host-qa-v1", tests_run=result.testsRun,
                  failures=len(result.failures), errors=len(result.errors), skipped=len(result.skipped),
                  passed=result.wasSuccessful(),
                  service_sha256=hashlib.sha256(Path(ARGS.service).read_bytes()).hexdigest(),
                  boundary="Explicit synthetic fixture and missing-device host QA only; no physical hardware acceptance")
    report["created_utc"] = datetime.now(timezone.utc).isoformat()
    report["test_cases"] = sorted(name for name in dir(CaptureServiceTests) if name.startswith("test_"))
    report["rates_verified_hz"] = [50000, 25000, 10000, 5000]
    report["injected_failures"] = ["driver", "index", "tick", "flags", "short", "crc", "publication_after_metadata"]
    report["real_device_access"] = False
    report["contract_sha256"] = hashlib.sha256((Path(__file__).resolve().parents[1] / "contract-v2.json").read_bytes()).hexdigest()
    report["source_sha256"] = hashlib.sha256((Path(__file__).resolve().parents[1] / "capture_service.cpp").read_bytes()).hexdigest()
    if ARGS.report:
        Path(ARGS.report).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    raise SystemExit(0 if result.wasSuccessful() else 1)
