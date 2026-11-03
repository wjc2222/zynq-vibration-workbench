#!/usr/bin/env python3
"""Configured bridge baud/timer QA over pipes; physical baud is not tested."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import statistics
import subprocess
import termios
import time
import tty

from test_bridge import Client, SCHEMA, decode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', required=True)
    parser.add_argument('--service', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    results = []
    for rate in (115200, 460800, 921600):
        client = Client(args.bridge, args.service, '--baud', str(rate))
        hello = client.ready()
        expected = 1000 if rate == 115200 else 250
        assert hello['baud'] == rate and hello['preview_interval_ms'] == expected
        started = client.call('start', plan={'sample_rate_hz': 5000, 'duration_seconds': 3, 'pulses_per_rev': 1})
        job = started['job_id']
        deadline = time.monotonic() + 20
        while not any(m.get('event') == 'analysis' and m['data']['job_id'] == job for m in client.messages):
            assert client.process.poll() is None
            assert time.monotonic() < deadline, client.messages[-4:]
            time.sleep(.03)
        previews = [m['data'] for m in client.messages if m.get('event') == 'preview' and m['data']['job_id'] == job]
        assert 1 <= len(previews) <= (4 if rate == 115200 else 14), len(previews)
        if rate != 115200:
            assert len(previews) >= 7, len(previews)
        differences = [b['end_s'] - a['end_s'] for a, b in zip(previews, previews[1:])]
        median = statistics.median(differences) if differences else None
        if median is not None:
            assert .65 <= median <= 1.35 if rate == 115200 else .12 <= median <= .42, median
        assert client.max_preview_compressed <= 6144
        assert all(p['source'] == 'test_fixture' and p['synthetic'] and not p['hardware_confirmed'] for p in previews)
        results.append({'baud': rate, 'preview_interval_ms': expected, 'preview_count_3s': len(previews),
                        'median_raw_window_end_delta_s': median, 'max_compressed_preview_bytes': client.max_preview_compressed,
                        'max_wire_line_bytes': client.max_line, 'result': 'PASS'})
        client.close()
    for value in ('9600', '230400', 'abc', '-1', '0921600'):
        result = subprocess.run([args.bridge, '--service', args.service, '--baud', value], capture_output=True, timeout=3)
        assert result.returncode == 2, (value, result.returncode)
    duplicate = subprocess.run([args.bridge, '--service', args.service, '--baud', '115200', '--baud', '921600'], capture_output=True, timeout=3)
    assert duplicate.returncode == 2
    master, slave = pty.openpty()
    original_termios = termios.tcgetattr(slave)
    original_flags = fcntl.fcntl(slave, fcntl.F_GETFL)
    process = None
    try:
        tty.setraw(slave)
        process = subprocess.Popen([args.bridge, '--service', args.service, '--test-fixture', '--baud', '921600'], stdin=slave, stdout=slave, stderr=subprocess.PIPE)
        os.write(master, (json.dumps(dict(schema=SCHEMA, id='pty_hello', command='hello')) + '\n').encode())
        buffer = b''
        messages = []
        sequence = 0
        deadline = time.monotonic() + 15
        while not any(m.get('id') == 'pty_hello' for m in messages):
            assert time.monotonic() < deadline
            if select.select([master], [], [], .1)[0]:
                buffer += os.read(master, 65536)
            while b'\n' in buffer:
                line, buffer = buffer.split(b'\n', 1)
                sequence, message, _ = decode(line + b'\n', sequence)
                messages.append(message)
        assert fcntl.fcntl(slave, fcntl.F_GETFL) & os.O_NONBLOCK
        os.write(master, (json.dumps(dict(schema=SCHEMA, id='pty_shutdown', command='shutdown')) + '\n').encode())
        process.wait(timeout=6)
        assert process.returncode == 0, process.stderr.read().decode()
        assert fcntl.fcntl(slave, fcntl.F_GETFL) == original_flags
    finally:
        if process is not None and process.poll() is None:
            process.stdin.close() if process.stdin else None
            process.terminate()
            process.wait(timeout=6)
        termios.tcsetattr(slave, termios.TCSANOW, original_termios)
        os.close(master)
        os.close(slave)
    report = {'schema': 'vibration.serial.configured-baud-qa.v1', 'result': 'PASS', 'cases': results,
              'invalid_values_rejected': 5, 'duplicate_option_rejected': True,
              'pseudo_terminal_nonblocking_flags_restored': True,
              'physical_baud_verified': False, 'hardware_or_COM_or_JTAG_used': False,
              'scope': 'Actual service fixture raw stream and bridge packet/timer over pipes; caller still owns physical serial configuration.'}
    Path(args.output).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
