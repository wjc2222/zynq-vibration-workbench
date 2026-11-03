#!/usr/bin/env python3
"""Post-rename sync ordering and failure gates on isolated host fixtures.

This observes real service syscalls with LD_PRELOAD. It is not a FAT crash,
physical SD, power-cut, or board acceptance test.
"""
import argparse
import errno
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
from datetime import datetime, timezone


def run_case(args, kind, phase="", error=errno.EIO):
    with tempfile.TemporaryDirectory(prefix="vibration-sync-qa-") as temporary:
        root = Path(temporary)
        output = root / "output"
        output.mkdir()
        sentinel = output / "user-original.txt"
        sentinel.write_text("unchanged user data", encoding="utf-8")
        trace = root / "sync.log"
        env = dict(os.environ, LD_PRELOAD=args.preload, AX7_QA_SYNC_ROOT=str(output),
                   AX7_QA_SYNC_TRACE=str(trace), AX7_QA_SYNC_FAIL=phase,
                   AX7_QA_SYNC_ERRNO=str(error))
        argv = [args.stream if kind == "stream" else args.capture, "--test-fixture"]
        if kind == "stream":
            argv.append("--fixture-chunk-ms=0")
        process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, env=env)
        messages, incoming = [], queue.Queue()

        def read():
            for line in process.stdout:
                incoming.put(json.loads(line))

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        schema = "vibration.stream.v1" if kind == "stream" else "vibration.capture.v2"
        plan = dict(sample_rate_hz=5000, duration_seconds=1, pulses_per_rev=2,
                    output_directory=str(output))
        if kind == "stream":
            plan.update(mode="timed", segment_seconds=1)
        request = dict(schema=schema, request_id="start_sync_qa", command="start", plan=plan)
        try:
            process.stdin.write((json.dumps(request) + "\n").encode())
            process.stdin.flush()
            while True:
                message = incoming.get(timeout=15)
                messages.append(message)
                if message.get("type") == "response" and not message.get("ok"):
                    raise AssertionError(message)
                if kind == "stream" and message.get("event") == "session_done":
                    terminal = message
                    break
                if kind == "capture" and message.get("event") == "job_status" and message.get("state") in ("completed", "failed", "cancelled"):
                    terminal = message
                    break
            process.stdin.write((json.dumps(dict(schema=schema, request_id="shutdown_sync_qa", command="shutdown")) + "\n").encode())
            process.stdin.close()
            process.wait(timeout=10)
            reader.join(timeout=2)
            assert process.returncode == 0, process.stderr.read().decode()
            observed = trace.read_text().splitlines() if trace.exists() else []
            if phase:
                assert phase in observed, (kind, phase, observed)
                assert terminal["state"] == "failed", terminal
                assert not any(m.get("state") == "completed" for m in messages), messages
                if kind == "stream":
                    assert terminal["durable_records"] == "0" and not terminal["session_complete"], terminal
                    assert len(list(output.glob("*/data.BIN"))) == 1, list(output.iterdir())
                    assert len(list(output.glob("*/data.meta.json"))) == 1
                else:
                    assert terminal["output_path"] == "", terminal
                    assert list(output.iterdir()) == [sentinel], list(output.iterdir())
            else:
                assert terminal["state"] == "completed", terminal
                expected = ["renamed_directory", "parent"] if kind == "stream" else ["renamed_metadata", "parent", "renamed_bin", "parent"]
                assert observed == expected, (kind, observed, expected)
                if kind == "stream":
                    assert terminal["durable_records"] == "5000" and terminal["session_complete"], terminal
            assert sentinel.read_text(encoding="utf-8") == "unchanged user data"
            return dict(kind=kind, fault=phase or "none", error=error if phase else None,
                        sync_order=observed, terminal=terminal["state"], passed=True)
        finally:
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdout.close()
            process.stderr.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stream", required=True)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--preload", required=True)
    parser.add_argument("--report")
    args = parser.parse_args()
    assert not Path("/dev/ax7z020-capture").exists(), "Host fixture refuses a physical capture device"
    results = [run_case(args, "stream"), run_case(args, "capture")]
    for kind, phase in (("stream", "renamed_directory"), ("stream", "parent"),
                        ("capture", "renamed_metadata"), ("capture", "renamed_bin"), ("capture", "parent")):
        results.append(run_case(args, kind, phase))
    for error in (errno.EROFS, errno.ENOSPC):
        results.append(run_case(args, "stream", "renamed_directory", error))
        results.append(run_case(args, "capture", "renamed_bin", error))
    report = dict(scope=__doc__, passed=True, cases=results,
                  utc=datetime.now(timezone.utc).isoformat(),
                  binaries_sha256={path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                                   for path in (args.stream, args.capture, args.preload)})
    rendered = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.report:
        Path(args.report).write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
