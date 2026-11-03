#!/usr/bin/python3
"""Test-only relay around the real service, injecting bounded protocol faults.

Acquisition/storage still run in the real explicitly selected fixture service.
The proxy is copied into a host temporary directory by test_bridge.py and is
never installed or included in production launch paths.
"""
import json
import os
import subprocess
import sys
import threading

child = subprocess.Popen([os.environ["VIBRATION_BRIDGE_TEST_REAL_SERVICE"], *sys.argv[1:]],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=sys.stderr)


def input_pump():
    try:
        # Keep the daemon off Python's buffered stdin lock while it waits.
        # Otherwise a fast child shutdown can abort interpreter finalization.
        while True:
            data = os.read(sys.stdin.fileno(), 4096)
            if not data:
                break
            child.stdin.write(data)
            child.stdin.flush()
    except (BrokenPipeError, OSError):
        pass
    finally:
        try:
            child.stdin.close()
        except OSError:
            pass


threading.Thread(target=input_pump, daemon=True).start()
fault = os.environ["VIBRATION_BRIDGE_TEST_FAULT"]
if fault == "service_stderr_notice":
    sys.stderr.write("TEST_SERVICE_STDERR_NOTICE: diagnostic must not enter UART framing\n")
    sys.stderr.flush()
frames = 0
injected = False
for line in child.stdout:
    value = json.loads(line)
    if fault == "missing_done" and value.get("event") == "session_done":
        continue
    if fault == "foreign_terminal" and value.get("event") == "job_status" and value.get("state") == "running" and not injected:
        value["job_id"] = "f" * 32
        value["session_id"] = "f" * 32
        value["state"] = "stopped"
        injected = True
    if fault == "bad_first_previews" and value.get("event") == "preview":
        frames += 1
        if frames == 1:
            value["record_count"] += .5
        elif frames == 2:
            value.update(source="hardware_test", synthetic=True, hardware_confirmed=True)
        elif frames == 3:
            value["ppr"] += 1
    sys.stdout.buffer.write(json.dumps(value, separators=(",", ":")).encode() + b"\n")
    sys.stdout.buffer.flush()
sys.exit(child.wait())
