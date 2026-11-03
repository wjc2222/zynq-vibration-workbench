"""Real bridge -> real streaming service integration; synthetic host files only."""
import argparse
import base64
import fcntl
import hashlib
import json
import math
import os
import pty
from pathlib import Path
import queue
import struct
import select
import subprocess
import sys
import tempfile
import threading
import time
import tty
import unittest
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("--bridge", required=True)
parser.add_argument("--service", required=True)
ARGS, REST = parser.parse_known_args()
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "serial_pc"))
from protocol import PacketDecoder, request_bytes, RecordDownload
from preview_payload import decode_detail, decode_envelope, decode_spectrum


class Session:
    def __init__(self, root, delay=0, baud=921600, extra=(), proxy_fault=None):
        self.root = Path(root)
        self.counter = 0
        self.messages = []
        self.q = queue.Queue()
        self.pause = threading.Event()
        self.pause.set()
        self.errors = tempfile.TemporaryFile()
        self.proxy = None
        service = ARGS.service
        environment = os.environ.copy()
        if proxy_fault:
            self.proxy = tempfile.TemporaryDirectory(prefix="stream-service-fault-proxy-")
            proxy_path = Path(self.proxy.name) / "service-proxy"
            proxy_path.write_bytes(Path(__file__).with_name("service_fault_proxy.py").read_bytes())
            proxy_path.chmod(0o700)
            service = str(proxy_path)
            environment["VIBRATION_BRIDGE_TEST_REAL_SERVICE"] = ARGS.service
            environment["VIBRATION_BRIDGE_TEST_FAULT"] = proxy_fault
        self.p = subprocess.Popen([ARGS.bridge, "--service", service,
            "--output-root", str(root), "--baud", str(baud), "--test-fixture",
            "--fixture-chunk-ms=" + str(delay), *extra], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.errors, bufsize=0, env=environment)

        def read():
            decoder = PacketDecoder()
            try:
                while True:
                    self.pause.wait()
                    raw = self.p.stdout.readline()
                    if not raw:
                        break
                    for value in decoder.feed(raw):
                        self.q.put(value)
            except BaseException as exc:
                self.q.put(exc)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.wait(lambda m: m.get("event") == "service_ready")

    def send(self, command, **fields):
        self.counter += 1
        ident = "test_" + str(self.counter)
        self.p.stdin.write(request_bytes(ident, command, **fields))
        self.p.stdin.flush()
        return ident

    def wait(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                item = self.q.get(timeout=min(1, deadline - time.monotonic()))
            except queue.Empty:
                if self.p.poll() is not None:
                    self.errors.seek(0)
                    raise AssertionError("Bridge exited: " + self.errors.read().decode(errors="replace"))
                continue
            if isinstance(item, BaseException):
                raise item
            self.messages.append(item)
            if predicate(item):
                return item
        raise AssertionError("Timed out waiting for bridge message; last=" + repr(self.messages[-3:]))

    def call(self, command, timeout=15, **fields):
        ident = self.send(command, **fields)
        return self.wait(lambda m: m.get("id") == ident, timeout)

    def ok(self, command, **fields):
        reply = self.call(command, **fields)
        if not reply.get("ok"):
            raise AssertionError(reply)
        return reply["data"]

    def close(self):
        self.pause.set()
        if self.p.poll() is None:
            try:
                self.ok("shutdown")
                self.p.wait(timeout=20)
            except (BrokenPipeError, AssertionError):
                self.p.stdin.close()
                self.p.wait(timeout=20)
        self.p.stdout.close()
        if not self.p.stdin.closed:
            self.p.stdin.close()
        self.reader.join(timeout=2)
        self.errors.close()
        if self.proxy:
            self.proxy.cleanup()


def plan(mode="timed", duration=3, rate=5000, segment=1):
    p = dict(mode=mode, sample_rate_hz=rate, pulses_per_rev=1, segment_seconds=segment)
    if mode == "timed":
        p["duration_seconds"] = duration
    return p


def check_segments(test, root, expected=None):
    total = 0
    previous_index = 0
    files = sorted(Path(root).glob("*/data.BIN"))
    for path in files:
        raw = path.read_bytes()
        h = bytearray(raw[:4096])
        test.assertEqual(h[:8], b"CAPSTRM1")
        records, payload, first = struct.unpack_from("<I", h, 36)[0], raw[4096:], struct.unpack_from("<Q", h, 104)[0]
        test.assertEqual(len(payload), records * 32)
        test.assertEqual(first, previous_index)
        test.assertEqual(zlib.crc32(payload), struct.unpack_from("<I", h, 96)[0])
        crc = struct.unpack_from("<I", h, 100)[0]
        h[100:104] = b"\0" * 4
        test.assertEqual(zlib.crc32(h), crc)
        test.assertEqual(struct.unpack_from("<I", h, 148)[0], 1)
        for i in range(records):
            test.assertEqual(struct.unpack_from("<Q", payload, i * 32)[0], first + i)
        meta = json.loads(path.with_suffix(".meta.json").read_text())
        test.assertTrue(meta["synthetic"])
        test.assertEqual(meta["source"], "test_fixture")
        previous_index = first + records
        total += records
    if expected is not None:
        test.assertEqual(total, expected)
    return total, files


def operator_label(path, label="normal"):
    """Create an actual bounded sidecar for an explicit synthetic QA segment.

    The C++ deletion test additionally uses the production saveRecordLabel
    writer, so this protocol fixture cannot be its only compatibility oracle.
    """
    path = Path(path)
    header = path.read_bytes()[:4096]
    entry = dict(label=label, source="operator", updated_at="2026-11-01T00:00:00.000Z",
                 clock_valid=True, session_id=header[112:128].hex())
    value = dict(entry, schema="vibration.operator-record-label.v1",
                 record=dict(file_name=path.name, bytes=str(path.stat().st_size),
                             header_crc32=str(struct.unpack_from("<I", header, 100)[0]),
                             payload_crc32=str(struct.unpack_from("<I", header, 96)[0])),
                 history=[entry], purpose="dataset_organization_only", inference_input=False,
                 synthetic_record=True, training_use="prohibited_synthetic",
                 clock_notice="fixed_synthetic_qa_timestamp_not_measurement_date")
    target = path.with_suffix(".labels.json")
    target.write_text(json.dumps(value), encoding="utf-8")
    return target


class BridgeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="stream-bridge-")
        self.s = None

    def tearDown(self):
        if self.s:
            self.s.close()
        self.tmp.cleanup()

    def session(self, **options):
        self.s = Session(self.tmp.name, **options)
        return self.s

    def test_timed_paginated_integrity_download_and_analysis(self):
        s = self.session()
        hello = s.ok("hello")
        self.assertFalse(hello["volatile"])
        self.assertEqual(hello["storage"], "sd")
        self.assertTrue(hello["servicecaps"]["available"])
        start = s.ok("start", plan=plan())
        self.assertTrue(start["job_id"])
        done = s.wait(lambda m: m.get("event") == "session_done")["data"]
        self.assertEqual(done["state"], "completed")
        self.assertEqual(done["durable_records"], "15000")
        self.assertTrue(done["session_complete"])
        self.assertTrue(done["target_complete"])
        self.assertEqual(len([m for m in s.messages if m.get("event") == "segment_ready"]), 3)
        check_segments(self, self.tmp.name, 15000)
        # Listing immediately after completion must wait through a running
        # background segment task instead of failing the user's SD dialog.
        first = s.ok("list_records", limit=2, cursor="")
        background = next((m for m in s.messages if m.get("event") in ("analysis", "analysis_failed")), None)
        if background is None:
            background = s.wait(lambda m: m.get("event") in ("analysis", "analysis_failed"))
        self.assertEqual(background["event"], "analysis", background)
        self.assertEqual(background["data"]["scope"], "completed_segment")
        self.assertEqual(len(first["records"]), 2)
        self.assertTrue(first["next_cursor"])
        self.assertNotIn("sha256", first["records"][0]["files"][0])
        second = s.ok("list_records", limit=2, cursor=first["next_cursor"])
        self.assertEqual(len(second["records"]), 1)
        self.assertFalse(second["next_cursor"])
        record = first["records"][0]["record_id"]
        analysis = s.ok("analyze_record", record_id=record)
        self.assertEqual(analysis["scope"], "complete_record")
        self.assertTrue(analysis["segment_only"])
        self.assertEqual(analysis["result"]["integrity"], "passed")
        result = analysis["result"]
        detail, bounds, rate = decode_detail(result["display_detail"])
        self.assertEqual(bounds[0], 0)
        self.assertEqual(len(detail[0]), 512)
        self.assertEqual(len(decode_envelope(result["display_envelope"])[0][0]), 128)
        self.assertEqual(result["display_spectrum"]["unit"], "V")
        self.assertEqual(len(decode_spectrum(result["display_spectrum"])[0]), 1639)
        self.assertTrue(result["display_decimated"])
        self.assertFalse(result["display_spectrum"]["display_decimated"])
        later = s.ok("analyze_record", record_id=second["records"][0]["record_id"])["result"]
        later_detail, later_bounds, _ = decode_detail(later["display_detail"])
        self.assertEqual(later_bounds[0], 0)
        self.assertGreater(int(later["display_detail"]["time_origin_index"]), 0)
        self.assertEqual(later["display_detail"]["first_index"], later["display_detail"]["time_origin_index"])
        manifest = s.ok("download_begin", record_id=record)
        with tempfile.TemporaryDirectory(prefix="stream-download-") as destination:
            download = RecordDownload(Path(destination), manifest)
            while True:
                request = download.next_chunk()
                if request is None:
                    break
                download.accept(s.ok("download_chunk", **request))
            result = download.finish()
            self.assertEqual(hashlib.sha256(result.read_bytes()).hexdigest(), manifest["files"][0]["sha256"])
        path = Path(self.tmp.name) / record / "data.BIN"
        with path.open("ab") as handle:
            handle.write(b"changed")
        changed = s.call("download_chunk", record_id=record, kind="bin", offset=0, length=10)
        self.assertFalse(changed["ok"])
        self.assertEqual(changed["error"]["code"], "RECORD_CHANGED")

    def test_live_preview_raw_stats_and_segment_analysis_keep_session_active(self):
        s = self.session(delay=40, baud=115200)
        s.ok("start", plan=plan("continuous"))
        preview = s.wait(lambda m: m.get("event") == "preview", timeout=8)["data"]
        self.assertEqual(preview["source"], "test_fixture")
        self.assertTrue(preview["synthetic"])
        self.assertEqual(preview["diagnosis"]["classification"], "not_classified")
        self.assertEqual(preview["voltage"], [])
        detail, detail_extent, rate = decode_detail(preview["waveform_detail"])
        self.assertEqual(len(detail[0]), 512)
        self.assertEqual(rate, 5000)
        first_detail = int(preview["waveform_detail"]["first_index"])
        for channel in range(3):
            for i, (t, value) in enumerate(detail[channel]):
                self.assertAlmostEqual(t, (first_detail+i)/5000)
                self.assertEqual(value, int(12000*math.sin(2*math.pi*(31+channel*17)*(first_detail+i)/5000))*5/32768)
        envelope, extent = decode_envelope(preview["waveform_envelope"])
        self.assertEqual(len(envelope[0]), 32)
        spectrum = decode_spectrum(preview["spectrum"])
        self.assertEqual(len(spectrum[0]), 410)
        self.assertEqual(preview["spectrum"]["channels"][0]["fft_size"], 1024)
        self.assertAlmostEqual(preview["mean_rpm"], 6000)
        stats = preview["amplitude_stats"]
        self.assertTrue(stats["derived_before_display_decimation"])
        first = int(preview["global_first_index"])
        for channel, entry in enumerate(stats["channels"]):
            values = [int(12000 * math.sin(2 * math.pi * (31 + channel * 17) * n / 5000)) * 5 / 32768
                      for n in range(first, first + entry["sample_count"])]
            self.assertAlmostEqual(entry["rms"], math.sqrt(sum(x*x for x in values) / len(values)), places=10)
        analysis = s.wait(lambda m: m.get("event") == "analysis", timeout=8)
        self.assertEqual(analysis["data"]["scope"], "completed_segment")
        self.assertNotIn("waveform", analysis["data"]["result"])
        self.assertNotIn("spectra", analysis["data"]["result"])
        self.assertTrue(s.ok("ping")["active"])
        self.assertTrue(s.ok("stop")["accepted"])
        done = s.wait(lambda m: m.get("event") == "session_done")["data"]
        self.assertEqual(done["state"], "stopped")
        check_segments(self, self.tmp.name, int(done["durable_records"]))

    def test_selected_record_display_fft_is_complete_and_control_survives_bulk_reply(self):
        s = self.session(delay=0, baud=115200)
        s.ok("start", plan=plan(duration=10, rate=5000, segment=10))
        s.wait(lambda m: m.get("event") == "session_done")
        record = s.ok("list_records")["records"][0]["record_id"]
        analysis_id = s.send("analyze_record", record_id=record)
        reply = s.wait(lambda m: m.get("id") == analysis_id, timeout=20)
        self.assertTrue(reply["ok"], reply)
        display = reply["data"]["result"]["display_spectrum"]
        self.assertEqual(display["unit"], "V")
        decoded = decode_spectrum(display)
        self.assertEqual(len(decoded[0]), 3277)
        self.assertEqual(display["channels"][0]["fft_size"], 8192)
        self.assertGreater(display["channels"][0]["segment_count"], 1)
        self.assertAlmostEqual(display["end_s"], 49999/5000)
        self.assertFalse(s.ok("ping")["active"])

    def test_continuous_manual_stop_saves_tail(self):
        s = self.session(delay=5)
        s.ok("start", plan=plan("continuous", segment=2))
        s.wait(lambda m: m.get("event") == "segment_ready")
        self.assertTrue(s.ok("stop")["accepted"])
        done = s.wait(lambda m: m.get("event") == "session_done")["data"]
        self.assertEqual(done["state"], "stopped")
        self.assertEqual(done["total_records"], "0")
        self.assertTrue(done["session_complete"])
        self.assertEqual(done["records_received"], done["durable_records"])
        count, files = check_segments(self, self.tmp.name, int(done["durable_records"]))
        self.assertGreaterEqual(count, 10000)
        self.assertGreaterEqual(len(files), 1)

    def test_shutdown_ack_follows_final_tail_publication(self):
        s = self.session(delay=5)
        s.ok("start", plan=plan("continuous", segment=10))
        time.sleep(.15)
        reply = s.ok("shutdown")
        self.assertTrue(reply["accepted"])
        self.assertTrue(reply["durability_confirmed"])
        self.assertTrue(any(m.get("event") == "session_done" for m in s.messages))
        self.assertEqual(s.p.wait(timeout=20), 0)
        count, files = check_segments(self, self.tmp.name)
        self.assertGreater(count, 0)
        self.assertEqual(len(files), 1)

    def test_eof_stops_and_saves_tail(self):
        s = self.session(delay=5)
        s.ok("start", plan=plan("continuous", segment=10))
        time.sleep(.15)
        s.p.stdin.close()
        self.assertEqual(s.p.wait(timeout=20), 0)
        count, files = check_segments(self, self.tmp.name)
        self.assertGreater(count, 0)
        self.assertEqual(len(files), 1)

    def test_64bit_timed_total_and_plan_validation(self):
        s = self.session(delay=40)
        result = s.ok("start", plan=plan(duration=86400, rate=50000))
        self.assertEqual(result["total_records"], "4320000000")
        s.ok("stop")
        done = s.wait(lambda m: m.get("event") == "session_done")["data"]
        self.assertFalse(done["target_complete"])
        self.assertEqual(done["state"], "stopped")
        wrong = plan("continuous")
        wrong["duration_seconds"] = 1
        self.assertEqual(s.call("start", plan=wrong)["error"]["code"], "INVALID_PLAN")
        wrong = plan()
        wrong["output_directory"] = "/tmp"
        self.assertEqual(s.call("start", plan=wrong)["error"]["code"], "UNSAFE_STORAGE")

    def test_storage_failure_preserves_published_prefix(self):
        s = self.session(delay=1, extra=("--fixture-fault=storage_after_first",))
        s.ok("start", plan=plan(duration=3))
        done = s.wait(lambda m: m.get("event") == "session_done")["data"]
        self.assertEqual(done["state"], "failed")
        self.assertFalse(done["session_complete"])
        count, files = check_segments(self, self.tmp.name, int(done["durable_records"]))
        self.assertEqual(count, 5000)
        self.assertEqual(len(files), 1)

    def test_idle_startup_no_card_hash_or_record_recovery(self):
        # A malformed huge sparse payload is never loaded/hashed at startup.
        segment = Path(self.tmp.name) / ("a" * 32 + "-000000")
        segment.mkdir()
        with (segment / "data.BIN").open("wb") as handle:
            handle.truncate(64 * 1024 * 1024)
        (segment / "data.meta.json").write_text("{}")
        s = self.session()
        self.assertTrue(s.ok("hello")["ready"])
        page = s.ok("list_records")
        self.assertEqual(page["records"], [])
        self.assertEqual(page["rejected_records"], 1)
        self.assertTrue((segment / "data.BIN").exists())

    def test_slow_uart_drops_preview_but_keeps_all_raw(self):
        s = self.session(delay=10, baud=115200)
        s.ok("start", plan=plan("continuous"))
        s.pause.clear()
        time.sleep(3.3)
        # Direct observation while the UART consumer is stalled: SD segments
        # continue to publish independently of bridge writes and analysis.
        before = sum(p.stat().st_size - 4096 for p in Path(self.tmp.name).glob("*/data.BIN")) // 32
        self.assertGreater(before, 30000)
        ident = s.send("stop")
        time.sleep(.3)
        s.pause.set()
        reply = s.wait(lambda m: m.get("id") == ident)
        self.assertTrue(reply["ok"])
        done = s.wait(lambda m: m.get("event") == "session_done")["data"]
        self.assertEqual(done["records_received"], done["durable_records"])
        self.assertEqual(done["state"], "stopped")
        check_segments(self, self.tmp.name, int(done["durable_records"]))
        counts = s.ok("ping")
        self.assertGreater(int(counts["preview_dropped_frames"]) + int(counts["analysis_skipped_segments"]), 0)

    def test_stdin_closed_before_service_start_is_graceful(self):
        p = subprocess.Popen([ARGS.bridge, "--service", ARGS.service,
            "--output-root", self.tmp.name, "--test-fixture"], stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        _, err = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 0, err.decode())

    def test_symlink_and_request_validation(self):
        s = self.session()
        reply = s.call("analyze_record", record_id="../../etc/passwd")
        self.assertEqual(reply["error"]["code"], "UNKNOWN_RECORD")
        segment = Path(self.tmp.name) / ("b" * 32 + "-000000")
        segment.symlink_to("/tmp", target_is_directory=True)
        page = s.ok("list_records")
        self.assertEqual(page["rejected_records"], 1)
        self.assertEqual(page["records"], [])
        s.p.stdin.write(b'{"schema":"vibration.serial.v1","id":"dup","command":"ping","command":"shutdown"}\n')
        bad = s.wait(lambda m: m.get("ok") is False)
        self.assertEqual(bad["error"]["code"], "DUPLICATE_KEY")
        self.assertTrue(s.ok("ping")["time_ms"])

    def test_first_preview_is_bound_to_accepted_source_plan_and_integer_count(self):
        s = self.session(delay=10, proxy_fault="bad_first_previews")
        s.ok("start", plan=plan(duration=3))
        s.wait(lambda m: m.get("event") == "session_done")
        self.assertGreaterEqual(int(s.ok("ping")["preview_rejected_frames"]), 3)
        check_segments(self, self.tmp.name, 15000)

    def test_foreign_terminal_status_is_rejected_and_raw_stops_gracefully(self):
        s = self.session(delay=10, proxy_fault="foreign_terminal")
        s.ok("start", plan=plan("continuous", segment=10))
        failed = s.wait(lambda m: m.get("event") == "service_error")
        self.assertIn("accepted session", failed["data"]["message"])
        self.assertEqual(s.p.wait(timeout=20), 2)
        count, _ = check_segments(self, self.tmp.name)
        self.assertGreater(count, 0)

    def test_missing_session_done_cannot_claim_shutdown_durability(self):
        s = self.session(delay=10, proxy_fault="missing_done")
        s.ok("start", plan=plan("continuous", segment=10))
        time.sleep(.1)
        reply = s.call("shutdown")
        self.assertFalse(reply["ok"])
        self.assertEqual(reply["error"]["code"], "SERVICE_LOST")
        self.assertEqual(s.p.wait(timeout=20), 2)
        count, _ = check_segments(self, self.tmp.name)
        self.assertGreater(count, 0)

    def test_user_file_queue_is_bounded_and_preserves_response_ids(self):
        s = self.session()
        identities = ["file_queue_0", "file_queue_1", "file_queue_2"]
        # One readInput turn sees all three: the first owns the file worker,
        # the second must be queued, and only the third exceeds the bound.
        s.p.stdin.write(b"".join(request_bytes(i, "list_records", cursor="", limit=1) for i in identities))
        s.p.stdin.flush()
        replies = {}
        while len(replies) < len(identities):
            value = s.wait(lambda m: m.get("id") in identities and m.get("id") not in replies)
            replies[value["id"]] = value
        self.assertTrue(replies[identities[0]]["ok"])
        self.assertTrue(replies[identities[1]]["ok"])
        self.assertFalse(replies[identities[2]]["ok"])
        self.assertEqual(replies[identities[2]]["error"]["code"], "BUSY")

    def saved_records(self, s, duration=3):
        s.ok("start", plan=plan(duration=duration))
        s.wait(lambda m: m.get("event") == "session_done")
        records = s.ok("list_records")["records"]
        deadline = time.monotonic() + 15
        while s.ok("ping")["analysis_busy"]:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)
        return records

    def test_delete_explicit_selected_pairs_and_preserve_unselected_originals(self):
        s = self.session()
        records = self.saved_records(s)
        sentinel = Path(self.tmp.name) / "user-original.txt"
        sentinel.write_text("never delete user originals")
        chosen = records[:2]
        expected = sum(sum(f["bytes"] for f in record["files"]) for record in chosen)
        result = s.ok("delete_records", record_ids=[r["record_id"] for r in chosen])
        self.assertEqual(result["deleted_records"], 2)
        self.assertEqual(result["deleted_bytes"], expected)
        self.assertEqual(result["requested_records"], 2)
        for item in result["results"]:
            self.assertTrue(item["deleted"])
            self.assertTrue(item["durability_confirmed"])
            self.assertEqual(item["files_removed"], 2)
            self.assertFalse((Path(self.tmp.name) / item["record_id"]).exists())
        self.assertEqual(sentinel.read_text(), "never delete user originals")
        self.assertEqual([r["record_id"] for r in s.ok("list_records")["records"]], [records[2]["record_id"]])
        self.assertFalse(list(Path(self.tmp.name).glob(".deleting-*")))
        # The simulated global capture lease is released when the task ends.
        with (Path(self.tmp.name) / ".vibration-fixture-device.lock").open("r+") as lease:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_delete_selected_operator_label_preserves_other_labels_and_reference_library(self):
        s = self.session()
        records = self.saved_records(s)
        root = Path(self.tmp.name)
        chosen, kept = records[:2]
        chosen_bin = root / chosen["record_id"] / "data.BIN"
        chosen_label = operator_label(chosen_bin)
        operator_label(root / kept["record_id"] / "data.BIN", "looseness")
        kept_before = {p.name: p.read_bytes() for p in (root / kept["record_id"]).iterdir()}
        reference = root / "reference-library.json"
        reference.write_text(json.dumps(dict(source=str(chosen_bin))), encoding="utf-8")
        reference_before = reference.read_bytes()
        # Listing/download contracts stay the existing BIN/meta pair.
        listed = s.ok("list_records")["records"]
        self.assertTrue(all(len(r["files"]) == 2 for r in listed))
        expected = sum(f["bytes"] for f in chosen["files"]) + chosen_label.stat().st_size
        result = s.ok("delete_records", record_ids=[chosen["record_id"]])
        item = result["results"][0]
        self.assertTrue(item["deleted"] and item["durability_confirmed"])
        self.assertEqual(item["files_removed"], 3)
        self.assertEqual(result["deleted_bytes"], expected)
        self.assertFalse(chosen_bin.parent.exists())
        self.assertEqual({p.name: p.read_bytes() for p in (root / kept["record_id"]).iterdir()}, kept_before)
        self.assertEqual(reference.read_bytes(), reference_before)
        self.assertFalse(s.call("analyze_record", record_id=chosen["record_id"])["ok"])
        self.assertFalse(list(root.glob(".deleting-*")))

    def test_delete_rejects_wrong_label_identity_unknown_file_and_label_symlink(self):
        s = self.session()
        records = self.saved_records(s)
        root = Path(self.tmp.name)
        first, second, third = (root / r["record_id"] for r in records)
        wrong = operator_label(first / "data.BIN")
        value = json.loads(wrong.read_text())
        value["record"]["payload_crc32"] = "1"
        wrong.write_text(json.dumps(value), encoding="utf-8")
        operator_label(second / "data.BIN")
        (second / "user-original.txt").write_text("preserve unrelated original")
        outside = root / "outside-original-label.json"
        operator_label(third / "data.BIN").rename(outside)
        (third / "data.labels.json").symlink_to(outside)
        before = {str(p.relative_to(root)): p.read_bytes() for directory in (first, second, third)
                  for p in directory.iterdir()}
        outside_before = outside.read_bytes()
        result = s.ok("delete_records", record_ids=[r["record_id"] for r in records])
        self.assertEqual(result["deleted_records"], 0)
        self.assertEqual(result["deleted_bytes"], 0)
        for item in result["results"]:
            self.assertFalse(item["deleted"])
            self.assertEqual(item["files_removed"], 0)
            self.assertIn("code", item["error"])
        self.assertEqual({str(p.relative_to(root)): p.read_bytes() for directory in (first, second, third)
                          for p in directory.iterdir()}, before)
        self.assertEqual(outside.read_bytes(), outside_before)
        self.assertFalse(list(root.glob(".deleting-*")))

    def test_delete_rejects_active_capture_analysis_and_active_download(self):
        s = self.session(delay=10)
        records = self.saved_records(s, duration=1)
        chosen = records[0]["record_id"]
        s.ok("start", plan=plan("continuous"))
        self.assertEqual(s.call("delete_records", record_ids=[chosen])["error"]["code"], "BUSY")
        s.ok("stop")
        s.wait(lambda m: m.get("event") == "session_done")
        while s.ok("ping")["analysis_busy"]:
            time.sleep(.02)
        ids = ["analysis_ref", "delete_analysis_ref"]
        s.p.stdin.write(request_bytes(ids[0], "analyze_record", record_id=chosen) + request_bytes(ids[1], "delete_records", record_ids=[chosen]))
        s.p.stdin.flush()
        replies = {}
        while len(replies) < 2:
            value = s.wait(lambda m: m.get("id") in ids and m.get("id") not in replies)
            replies[value["id"]] = value
        self.assertTrue(replies[ids[0]]["ok"])
        self.assertEqual(replies[ids[1]]["error"]["code"], "BUSY")
        s.ok("download_begin", record_id=chosen)
        self.assertTrue(s.ok("ping")["download_active"])
        self.assertEqual(s.call("delete_records", record_ids=[chosen])["error"]["code"], "BUSY")
        self.assertTrue(s.ok("download_end")["accepted"])
        result = s.ok("delete_records", record_ids=[chosen])
        self.assertTrue(result["results"][0]["deleted"])

    def test_delete_download_reference_auto_releases_after_both_complete_files(self):
        s = self.session()
        record = self.saved_records(s, duration=1)[0]
        manifest = s.ok("download_begin", record_id=record["record_id"])
        for file in manifest["files"]:
            for offset in range(0, file["bytes"], 3072):
                s.ok("download_chunk", record_id=record["record_id"], kind=file["kind"], offset=offset, length=min(3072, file["bytes"]-offset))
        self.assertFalse(s.ok("ping")["download_active"])
        self.assertTrue(s.ok("delete_records", record_ids=[record["record_id"]])["results"][0]["deleted"])

    def test_delete_rejects_unterminated_session_even_after_terminal_job_status(self):
        s = self.session(delay=5, proxy_fault="missing_done")
        s.ok("start", plan=plan(duration=1))
        s.wait(lambda m: m.get("event") == "job_status" and m["data"].get("state") == "completed")
        records = s.ok("list_records")["records"]
        while s.ok("ping")["analysis_busy"]:
            time.sleep(.02)
        self.assertEqual(s.call("delete_records", record_ids=[records[0]["record_id"]])["error"]["code"], "BUSY")
        self.assertTrue((Path(self.tmp.name) / records[0]["record_id"] / "data.BIN").exists())

    def test_delete_rejects_foreign_files_symlinks_legacy_and_reports_each_item(self):
        s = self.session()
        records = self.saved_records(s)
        root = Path(self.tmp.name)
        first, second, third = (r["record_id"] for r in records)
        (root / first / "user-original.txt").write_text("preserve unknown")
        outside = root / "outside-original.txt"
        outside.write_text("preserve symlink target")
        (root / second / "data.BIN").rename(root / second / "original.BIN")
        (root / second / "data.BIN").symlink_to(outside)
        missing = "f"*32 + "-999999"
        result = s.ok("delete_records", record_ids=[first, second, "../../outside-original.txt", "legacy-session/finite", missing, third])
        self.assertEqual(len(result["results"]), 6)
        self.assertEqual(result["deleted_records"], 1)
        self.assertTrue(result["results"][-1]["deleted"])
        for item in result["results"][:-1]:
            self.assertFalse(item["deleted"])
            self.assertEqual(item["bytes"], 0)
            self.assertIn("code", item["error"])
        self.assertEqual((root / first / "user-original.txt").read_text(), "preserve unknown")
        self.assertEqual(outside.read_text(), "preserve symlink target")
        self.assertTrue((root / second / "original.BIN").exists())
        self.assertFalse(list(root.glob(".deleting-*")))

    def test_delete_batch_bounds_and_cross_process_capture_lease(self):
        s = self.session()
        chosen = self.saved_records(s, duration=1)[0]["record_id"]
        for ids in ([], [chosen, chosen], ["a"*32+"-"+str(i).zfill(6) for i in range(101)]):
            self.assertEqual(s.call("delete_records", record_ids=ids)["error"]["code"], "INVALID_REQUEST")
        lock = Path(self.tmp.name) / ".vibration-fixture-device.lock"
        with lock.open("w+") as external_capture:
            fcntl.flock(external_capture, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertEqual(s.call("delete_records", record_ids=[chosen])["error"]["code"], "BUSY")
            self.assertTrue((Path(self.tmp.name) / chosen / "data.BIN").exists())
        # A rejected selected ID still releases the lease in every path.
        self.assertFalse(s.ok("delete_records", record_ids=["legacy/record"])["results"][0]["deleted"])
        with lock.open("r+") as external_capture:
            fcntl.flock(external_capture, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertTrue(s.ok("delete_records", record_ids=[chosen])["results"][0]["deleted"])

    def test_fixture_lock_symlink_is_rejected_and_cannot_be_used_in_production(self):
        s = self.session()
        chosen = self.saved_records(s, duration=1)[0]["record_id"]
        target = Path(self.tmp.name) / "user-lock-target"
        target.write_text("unchanged")
        (Path(self.tmp.name) / ".vibration-fixture-device.lock").symlink_to(target)
        self.assertEqual(s.call("delete_records", record_ids=[chosen])["error"]["code"], "UNSAFE_STORAGE")
        self.assertEqual(target.read_text(), "unchanged")
        self.assertTrue((Path(self.tmp.name) / chosen / "data.BIN").exists())
        result = subprocess.run([ARGS.bridge, "--service", ARGS.service, "--output-root", self.tmp.name, "--fixture-device-lock", str(target)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"without --test-fixture", result.stderr)

    def test_pipe_diagnostics_remain_on_stderr(self):
        s = self.session(proxy_fault="service_stderr_notice")
        s.ok("shutdown")
        self.assertEqual(s.p.wait(timeout=10), 0)
        s.errors.seek(0)
        diagnostic = s.errors.read()
        self.assertIn(b"TEST_SERVICE_STDERR_NOTICE", diagnostic)
        self.assertIn(b"Client requested shutdown", diagnostic)

    def test_shared_uart_tty_contains_only_complete_packets_through_shutdown(self):
        # Model the board shell's one physical UART: all three standard fds
        # point to the same real tty. A bare stderr diagnostic would be parsed
        # as protocol JSON and fail this test, as in the physical raw trace.
        master, slave = pty.openpty()
        tty.setraw(slave)
        os.set_blocking(master, False)
        with tempfile.TemporaryDirectory(prefix="tty-service-proxy-") as proxy_folder:
            proxy = Path(proxy_folder) / "service-proxy"
            proxy.write_bytes(Path(__file__).with_name("service_fault_proxy.py").read_bytes())
            proxy.chmod(0o700)
            environment = os.environ.copy()
            environment.update(VIBRATION_BRIDGE_TEST_REAL_SERVICE=ARGS.service,
                               VIBRATION_BRIDGE_TEST_FAULT="service_stderr_notice")
            process = subprocess.Popen([ARGS.bridge, "--service", str(proxy),
                "--output-root", self.tmp.name, "--baud", "921600", "--test-fixture",
                "--fixture-chunk-ms=10"], stdin=slave, stdout=slave, stderr=slave,
                close_fds=True, env=environment)
            os.close(slave)
            decoder = PacketDecoder()
            messages, trace = [], bytearray()

            def until(predicate, timeout=15):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if not select.select([master], [], [], .2)[0]:
                        continue
                    try:
                        block = os.read(master, 65536)
                    except OSError:
                        break
                    trace.extend(block)
                    for message in decoder.feed(block):
                        messages.append(message)
                        if predicate(message):
                            return message
                raise AssertionError("Missing PTY protocol response; trace=" + repr(trace[-300:]))

            try:
                until(lambda m: m.get("event") == "service_ready")
                os.write(master, request_bytes("tty-start", "start", plan=plan("continuous", segment=10)))
                until(lambda m: m.get("id") == "tty-start")
                time.sleep(.12)
                os.write(master, request_bytes("tty-shutdown", "shutdown"))
                ack = until(lambda m: m.get("id") == "tty-shutdown")
                self.assertTrue(ack["ok"])
                self.assertTrue(ack["data"]["durability_confirmed"])
                self.assertTrue(any(m.get("event") == "session_done" for m in messages))
                self.assertEqual(process.wait(timeout=10), 0)
                self.assertFalse(decoder.buffer)
                self.assertNotIn(b"TEST_SERVICE_STDERR_NOTICE", trace)
                self.assertNotIn(b"stream_bridge:", trace)
                count, _ = check_segments(self, self.tmp.name)
                self.assertGreater(count, 0)
            finally:
                os.close(master)
                if process.poll() is None:
                    process.wait(timeout=20)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *REST])
