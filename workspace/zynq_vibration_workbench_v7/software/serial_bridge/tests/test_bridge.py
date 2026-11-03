#!/usr/bin/env python3
"""Linux host fixture QA, using only console pipes and the public wire contract.

Does not open hardware, a serial port, JTAG, Qt Widgets or the SD card.
Outputs retained evidence; synthetic captures remain under the volatile /tmp
managed root so restart recovery is exercised without deleting user records.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import threading
import time
import uuid
import zlib

SCHEMA = 'vibration.serial.v1'
ROOT = Path('/tmp/vibration-serial-records')


def decode(line, previous):
    assert len(line) <= 131072 and line.endswith(b'\n'), len(line)
    packet = json.loads(line)
    assert packet['schema'] == SCHEMA and packet['type'] == 'packet'
    assert packet['codec'] == 'qcompress-base64'
    assert packet['seq'] > previous
    compressed = base64.b64decode(packet['payload'], validate=True)
    assert len(compressed) >= 5
    size, = struct.unpack('>I', compressed[:4])
    assert size <= 2097152
    inflater = zlib.decompressobj()
    raw = inflater.decompress(compressed[4:], size + 1)
    assert len(raw) == size and inflater.eof and not inflater.unused_data
    assert hashlib.sha256(raw).hexdigest() == packet['sha256']
    return packet['seq'], json.loads(raw), len(compressed)


class Client:
    def __init__(self, binary, service, *extra, heartbeat=True):
        self.process = subprocess.Popen([binary, '--service', service, '--test-fixture', *extra],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.queue = queue.Queue()
        self.messages = []
        self.closed = threading.Event()
        self.paused = threading.Event()
        self.lock = threading.Lock()
        self.counter = 0
        self.max_line = self.max_compressed = self.max_preview_compressed = 0
        self.reader_error = None

        def reader():
            seq = 0
            try:
                while not self.closed.is_set():
                    while self.paused.is_set() and not self.closed.is_set():
                        time.sleep(.02)
                    line = self.process.stdout.readline(131073)
                    if not line:
                        break
                    seq, message, compressed = decode(line, seq)
                    self.max_line = max(self.max_line, len(line))
                    self.max_compressed = max(self.max_compressed, compressed)
                    if message.get('event') == 'preview':
                        self.max_preview_compressed = max(self.max_preview_compressed, compressed)
                    self.messages.append(message)
                    self.queue.put(message)
            except BaseException as error:
                self.reader_error = repr(error)

        self.reader = threading.Thread(target=reader, daemon=True)
        self.reader.start()
        if heartbeat:
            def beat():
                while not self.closed.wait(3):
                    if self.process.poll() is not None:
                        return
                    try:
                        self.send('ping')
                    except (BrokenPipeError, OSError):
                        return
            threading.Thread(target=beat, daemon=True).start()

    def send(self, command, **fields):
        with self.lock:
            self.counter += 1
            request_id = 'qa_%d' % self.counter
            message = dict(schema=SCHEMA, id=request_id, command=command, **fields)
            self.process.stdin.write(json.dumps(message, separators=(',', ':')).encode() + b'\n')
            self.process.stdin.flush()
            return request_id

    def raw(self, data):
        with self.lock:
            self.process.stdin.write(data + b'\n')
            self.process.stdin.flush()

    def wait(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert self.reader_error is None, self.reader_error
            for message in self.messages:
                if predicate(message):
                    return message
            if self.process.poll() is not None:
                raise AssertionError(('bridge exited', self.process.returncode, self.process.stderr.read().decode(), self.messages[-5:]))
            try:
                self.queue.get(timeout=.05)
            except queue.Empty:
                pass
        raise AssertionError(('timeout', self.messages[-5:]))

    def response(self, request_id, timeout=15):
        return self.wait(lambda m: m.get('id') == request_id, timeout)

    def call(self, command, **fields):
        response = self.response(self.send(command, **fields))
        assert response['ok'], response
        return response['data']

    def ready(self):
        self.wait(lambda m: m.get('event') == 'service_ready')
        ready = self.wait(lambda m: m.get('event') == 'records_ready')
        assert ready['data']['ok'], ready
        return self.call('hello')

    def close(self):
        if self.process.poll() is None:
            self.paused.clear()
            self.call('shutdown')
            self.process.wait(timeout=8)
            assert self.process.returncode == 0, self.process.stderr.read().decode()
        self.closed.set()
        self.reader.join(timeout=2)
        assert self.reader_error is None, self.reader_error


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', required=True)
    parser.add_argument('--service', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    evidence = {'schema': 'vibration.serial.host-fixture-qa.v1', 'source': 'test_fixture',
                'hardware_confirmed': False, 'physical_COM_or_JTAG_used': False, 'tests': []}
    cases = evidence['tests']

    def passed(name, **detail):
        cases.append(dict(case=name, result='PASS', **detail))
        print(name + '=PASS', flush=True)

    client = Client(args.bridge, args.service)
    hello = client.ready()
    assert hello['baud'] == 115200 and hello['volatile'] is True
    assert hello['servicecaps']['backend'] == 'test_fixture'
    assert hello['servicecaps']['hardware_confirmed'] is False
    passed('hello_source_is_explicit_fixture', hello=hello)
    client.raw(b'{"schema":"vibration.serial.v1","id":"dupkey","command":"ping","command":"start"}')
    rejection = client.wait(lambda m: m.get('error', {}).get('code') == 'DUPLICATE_KEY')
    passed('duplicate_keys_rejected', response=rejection)
    client.raw(b'{' + b'x' * 9000 + b'}')
    client.wait(lambda m: m.get('error', {}).get('code') == 'LINE_TOO_LONG')
    client.raw(b'\xff')
    client.wait(lambda m: m.get('error', {}).get('code') == 'INVALID_JSON')
    passed('bounded_input_and_invalid_utf8')
    bad = client.response(client.send('start', plan={'sample_rate_hz': 5000, 'duration_seconds': 1,
                                                   'pulses_per_rev': 1, 'output_directory': '/etc'}))
    assert not bad['ok'] and bad['error']['code'] == 'INVALID_REQUEST'
    passed('caller_output_path_rejected')
    unknown = client.response(client.send('download_begin', record_id='../../etc/passwd'))
    assert unknown['error']['code'] == 'UNKNOWN_RECORD'
    passed('arbitrary_download_path_rejected')

    # Real service fixture streams raw32-byte records; bridge validates and
    # reduces them. A three-second window is long enough to observe spectra.
    started = client.call('start', plan={'sample_rate_hz': 50000, 'duration_seconds': 3, 'pulses_per_rev': 1})
    job = started['job_id']
    preview = client.wait(lambda m: m.get('event') == 'preview' and m['data']['job_id'] == job)['data']
    assert preview['synthetic'] and not preview['hardware_confirmed']
    assert preview['source'] == 'test_fixture' and preview['ppr_confirmed']
    assert preview['diagnosis']['classification'] == 'not_classified'
    assert len(preview['voltage']) == 3 and all(len(ch) <= 160 for ch in preview['voltage'])
    assert all(len(point) == 2 for ch in preview['voltage'] for point in ch)
    assert preview['start_s'] <= preview['end_s']
    assert all(preview['start_s'] <= p[0] <= preview['end_s'] + 1e-8 for ch in preview['voltage'] for p in ch)
    analysis = client.wait(lambda m: m.get('event') == 'analysis' and m['data']['job_id'] == job, 25)['data']
    assert analysis['scope'] == 'complete_record'
    assert analysis['source_metadata']['actual_source'] == 'host_fixture'
    assert analysis['result']['source_metadata']['synthetic'] is True
    assert analysis['result']['diagnosis']['classification'] == 'not_classified'
    assert all(s['fft_size'] == 65536 and len(s['frequency_hz']) <= 256 for s in analysis['result']['spectra'])
    assert all(s['original_bin_count'] >= len(s['frequency_hz']) for s in analysis['result']['spectra'])
    terminal_index = next(i for i, m in enumerate(client.messages) if m.get('event') == 'job_status' and m['data'].get('job_id') == job and m['data']['state'] == 'completed')
    assert not any(m.get('event') == 'preview' and m['data']['job_id'] == job for m in client.messages[terminal_index + 1:])
    passed('validated_preview_and_full_record_default_fft', job_id=job, record_id=analysis['record_id'],
           preview=preview, analysis_summary={k: analysis['result'][k] for k in ('source_metadata', 'diagnosis')},
           max_preview_compressed=client.max_preview_compressed)
    # Compare against independently launched shared-core CLI before any display
    # reduction. Peak bins must survive; statistics and diagnosis must be exact.
    analyzer = str(Path(args.bridge).parent / 'core' / 'vibration_analyze')
    bin_name = next(f['name'] for f in analysis['files'] if f['kind'] == 'bin')
    capture_path = ROOT / analysis['record_id'].rsplit('/', 1)[0] / bin_name
    reference = json.loads(subprocess.check_output([analyzer, str(capture_path), '--waveform-points', '320'], timeout=15))
    for key in ('header', 'time_stats', 'order_bands', 'speed', 'diagnosis', 'waveform'):
        assert reference[key] == analysis['result'][key], key
    for full, shown in zip(reference['spectra'], analysis['result']['spectra']):
        assert max(full['peak_amplitude']) == max(shown['peak_amplitude'])
        assert shown['original_bin_count'] == len(full['frequency_hz'])
        for key in ('fft_size', 'segment_count', 'bin_width_hz', 'enbw_hz'):
            assert full[key] == shown[key], key
    passed('shared_core_statistics_and_full_resolution_peak_equivalence')

    record_id = analysis['record_id']
    records = client.call('list_records')['records']
    assert record_id in [r['record_id'] for r in records]
    files = client.call('download_begin', record_id=record_id)['files']
    hashes = {}
    for file in files:
        data = bytearray()
        while len(data) < file['bytes']:
            chunk = client.call('download_chunk', record_id=record_id, kind=file['kind'], offset=len(data),
                                length=min(3072, file['bytes'] - len(data)))
            raw = base64.b64decode(chunk['data'], validate=True)
            assert chunk['length'] == len(raw) and chunk['offset'] == len(data)
            assert zlib.crc32(raw) & 0xffffffff == chunk['crc32']
            data.extend(raw)
        assert hashlib.sha256(data).hexdigest() == file['sha256']
        hashes[file['kind']] = file['sha256']
        if file['kind'] == 'meta':
            assert json.loads(data)['actual_source'] == 'host_fixture'
    assert set(hashes) == {'bin', 'meta'}
    bad_range = client.response(client.send('download_chunk', record_id=record_id, kind='bin', offset=-1, length=3072))
    assert bad_range['error']['code'] == 'INVALID_RANGE'
    bad_kind = client.response(client.send('download_chunk', record_id=record_id, kind='../etc', offset=0, length=16))
    assert bad_kind['error']['code'] == 'INVALID_REQUEST'
    passed('complete_bin_metadata_download_crc_sha256', files=files, hashes=hashes)
    # The pair is one download transaction: a changed metadata file invalidates
    # even a BIN chunk. Restore the original bytes before reconnect recovery.
    owned_meta = ROOT / record_id.rsplit('/', 1)[0] / next(f['name'] for f in files if f['kind'] == 'meta')
    original_meta = owned_meta.read_bytes()
    try:
        owned_meta.write_bytes(original_meta + b'\n')
        changed = client.response(client.send('download_chunk', record_id=record_id, kind='bin', offset=0, length=16))
        assert changed['error']['code'] == 'RECORD_CHANGED'
    finally:
        owned_meta.write_bytes(original_meta)
    assert hashlib.sha256(owned_meta.read_bytes()).hexdigest() == hashes['meta']
    passed('sidecar_mutation_invalidates_entire_download_pair')
    request_id = client.send('ping')
    client.response(request_id)
    client.raw(json.dumps(dict(schema=SCHEMA, id=request_id, command='ping')).encode())
    client.wait(lambda m: m.get('id') == request_id and m.get('error', {}).get('code') == 'DUPLICATE_REQUEST')
    passed('duplicate_request_ids_rejected')
    client.close()

    # A restarted bridge must rediscover validated complete records.
    restarted = Client(args.bridge, args.service)
    restarted.ready()
    assert record_id in [r['record_id'] for r in restarted.call('list_records')['records']]
    passed('reconnect_recovers_complete_records', record_id=record_id)
    reread = restarted.call('analyze_record', record_id=record_id)
    assert reread['scope'] == 'complete_record' and reread['record_id'] == record_id
    passed('explicit_complete_record_reanalysis')
    restarted.close()

    # Slow stdout reader cannot stall the service or prevent cancel ingress.
    slow = Client(args.bridge, args.service)
    slow_hello = slow.ready()
    start = slow.call('start', plan={'sample_rate_hz': 50000, 'duration_seconds': 10, 'pulses_per_rev': 1})
    slow.wait(lambda m: m.get('event') == 'preview')
    slow.paused.set()
    time.sleep(2.5)
    before = time.monotonic()
    cancel_id = slow.send('cancel', job_id=start['job_id'])
    time.sleep(1)
    assert not list((ROOT / slow_hello['session_id']).glob('*.[bB][iI][nN]'))
    slow.paused.clear()
    cancel = slow.response(cancel_id, timeout=5)
    assert cancel['ok'] and cancel['data']['accepted']
    ending = slow.wait(lambda m: m.get('event') == 'job_status' and m['data'].get('job_id') == start['job_id'] and m['data']['state'] == 'cancelled', 5)
    assert time.monotonic() - before < 3
    assert not list((ROOT / slow_hello['session_id']).glob('*.[bB][iI][nN]'))
    passed('slow_reader_cancellation_stays_responsive', observed_seconds=time.monotonic() - before,
           terminal=ending['data'], max_wire_line=slow.max_line)
    slow.close()

    # A cancellation that arrives in the same ingress batch as start cannot be
    # acknowledged as idle and then silently start acquisition afterward.
    race = Client(args.bridge, args.service)
    race_hello = race.ready()
    start_request = race.send('start', plan={'sample_rate_hz': 5000, 'duration_seconds': 10, 'pulses_per_rev': 1})
    cancel_request = race.send('cancel')
    start_reply = race.response(start_request)
    cancel_reply = race.response(cancel_request)
    assert start_reply['ok'] and cancel_reply['ok'] and cancel_reply['data']['accepted']
    race.wait(lambda m: m.get('event') == 'job_status' and m['data']['state'] == 'cancelled')
    assert not list((ROOT / race_hello['session_id']).glob('*.[bB][iI][nN]'))
    passed('cancel_during_start_ack_is_not_lost')
    race.close()

    # Closing stdin also cancels the one owned child without a shell or reset.
    eof = Client(args.bridge, args.service)
    eof_hello = eof.ready()
    eof.call('start', plan={'sample_rate_hz': 5000, 'duration_seconds': 10, 'pulses_per_rev': 1})
    eof.process.stdin.close()
    eof.process.wait(timeout=6)
    assert eof.process.returncode == 0
    assert not list((ROOT / eof_hello['session_id']).glob('*.[bB][iI][nN]'))
    eof.closed.set()
    passed('input_eof_cancels_active_capture')

    # A corrupt preview must not be replaced by an attractive fake trace.
    corrupt = Client(args.bridge, args.service, '--fixture-fault=preview_crc')
    corrupt.ready()
    start = corrupt.call('start', plan={'sample_rate_hz': 5000, 'duration_seconds': 2, 'pulses_per_rev': 1})
    rejection = corrupt.wait(lambda m: m.get('event') == 'preview_rejected')
    corrupt.wait(lambda m: m.get('event') == 'analysis' and m['data']['job_id'] == start['job_id'], 20)
    assert not any(m.get('event') == 'preview' for m in corrupt.messages)
    passed('bad_preview_crc_rejected_without_fake_trace', rejection=rejection)
    corrupt.close()

    # Recovery excludes corrupt CRCs, missing metadata and symlink pairs.
    record_path = ROOT / record_id.rsplit('/', 1)[0] / next(file['name'] for file in files if file['kind'] == 'bin')
    meta_path = ROOT / (record_id + '.meta.json')
    qa_dir = ROOT / uuid.uuid4().hex
    qa_dir.mkdir(mode=0o700)
    shutil.copyfile(record_path, qa_dir / 'badcrc.bin')
    shutil.copyfile(meta_path, qa_dir / 'badcrc.meta.json')
    with (qa_dir / 'badcrc.bin').open('r+b') as stream:
        stream.seek(-1, os.SEEK_END)
        old = stream.read(1)
        stream.seek(-1, os.SEEK_END)
        stream.write(bytes([old[0] ^ 1]))
    shutil.copyfile(record_path, qa_dir / 'missingmeta.bin')
    (qa_dir / 'symlink.bin').symlink_to(record_path)
    shutil.copyfile(meta_path, qa_dir / 'symlink.meta.json')
    guard = Client(args.bridge, args.service)
    guard.ready()
    visible = [r['record_id'] for r in guard.call('list_records')['records']]
    assert not any(r.startswith(qa_dir.name + '/') for r in visible)
    passed('recovery_excludes_crc_missing_meta_and_symlink', qa_directory=str(qa_dir))
    guard.close()

    # Production default is never silently promoted to a test source.
    production = subprocess.Popen([args.bridge, '--service', args.service], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    production.stdin.write((json.dumps(dict(schema=SCHEMA, id='hello', command='hello')) + '\n').encode())
    production.stdin.flush()
    observed = []
    previous = 0
    for _ in range(8):
        previous, packet, _ = decode(production.stdout.readline(), previous)
        observed.append(packet)
        if packet.get('event') == 'service_ready':
            caps = packet['data']['servicecaps']
            assert caps['requested_source'] == 'adc' and not caps['available']
            break
    else:
        raise AssertionError(observed)
    production.stdin.write((json.dumps(dict(schema=SCHEMA, id='shutdown', command='shutdown')) + '\n').encode())
    production.stdin.flush()
    production.communicate(timeout=8)
    assert production.returncode == 0
    passed('production_default_no_fixture_fallback', capabilities=caps)

    expired = Client(args.bridge, args.service, heartbeat=False)
    expired_hello = expired.ready()
    expired.call('start', plan={'sample_rate_hz': 5000, 'duration_seconds': 20, 'pulses_per_rev': 1})
    expired.process.wait(timeout=19)
    assert expired.process.returncode == 2
    assert not list((ROOT / expired_hello['session_id']).glob('*.[bB][iI][nN]'))
    expired.closed.set()
    passed('heartbeat_loss_closes_owned_service', exit_code=expired.process.returncode)
    evidence['result'] = 'PASS'
    evidence['test_count'] = len(cases)
    evidence['max_wire_line'] = max(client.max_line, restarted.max_line, slow.max_line, corrupt.max_line)
    evidence['max_preview_compressed'] = max(client.max_preview_compressed, slow.max_preview_compressed)
    evidence['not_verified'] = ['physical 115200 COM port', 'actual board acquisition', 'physical ADC vibration', 'HDMI/touch']
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('ALL_PASS tests=%d report=%s' % (len(cases), output))


if __name__ == '__main__':
    main()
