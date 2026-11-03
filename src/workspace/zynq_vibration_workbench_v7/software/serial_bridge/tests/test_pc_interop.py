#!/usr/bin/env python3
"""Use the actual PC PacketDecoder/RecordDownload against the host bridge."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', required=True)
    parser.add_argument('--service', required=True)
    parser.add_argument('--pc-protocol', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('serial_pc_protocol_under_test', args.pc_protocol)
    pc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pc)
    process = subprocess.Popen([args.bridge, '--service', args.service, '--test-fixture'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    decoder = pc.PacketDecoder()
    sequence = 0
    events = []
    request_counter = 0

    def packet():
        line = process.stdout.readline(131073)
        assert line, process.stderr.read().decode()
        decoded = decoder.feed(line)
        assert len(decoded) == 1
        item = decoded[0]
        if 'event' in item:
            events.append(item)
        return item

    def call(command, **fields):
        nonlocal request_counter
        request_counter += 1
        request_id = 'interop_%d' % request_counter
        process.stdin.write(pc.request_bytes(request_id, command, **fields))
        process.stdin.flush()
        while True:
            item = packet()
            if item.get('id') == request_id:
                assert item['ok'], item
                return item['data']

    ready = set()
    while ready != {'service_ready', 'records_ready'}:
        item = packet()
        if item.get('event') in {'service_ready', 'records_ready'}:
            ready.add(item['event'])
    hello = call('hello')
    job = call('start', plan={'sample_rate_hz': 5000, 'duration_seconds': 1, 'pulses_per_rev': 1})['job_id']
    while True:
        item = packet()
        if item.get('event') == 'analysis' and item['data']['job_id'] == job:
            analysis = item['data']
            break
    manifest = call('download_begin', record_id=analysis['record_id'])
    with tempfile.TemporaryDirectory(prefix='vibration-pc-interop-') as directory:
        transfer = pc.RecordDownload(Path(directory), manifest)
        while True:
            chunk = transfer.next_chunk()
            if chunk is None:
                break
            transfer.accept(call('download_chunk', **chunk))
        binary_path = transfer.finish()
        assert binary_path.exists()
        metadata = json.loads(binary_path.with_suffix('.meta.json').read_text())
        assert metadata['synthetic'] and metadata['actual_source'] == 'host_fixture'
        receipt_path = binary_path.parent / 'transfer-receipt.json'
        receipt = json.loads(receipt_path.read_text())
        assert receipt['transfer_integrity'] == 'passed'
        retained_hashes = {file['kind']: hashlib.sha256((binary_path.parent / file['name']).read_bytes()).hexdigest()
                           for file in manifest['files']}
    call('shutdown')
    process.wait(timeout=6)
    assert process.returncode == 0
    report = {'schema': 'vibration.serial.pc-wire-interop.v1', 'result': 'PASS',
              'pc_protocol_sha256': hashlib.sha256(Path(args.pc_protocol).read_bytes()).hexdigest(),
              'pc_decoder_used_for_every_packet': True, 'pc_record_download_used': True,
              'packets_decoded': decoder.sequence, 'requests': request_counter, 'record_id': analysis['record_id'],
              'source': 'host_fixture', 'hardware_confirmed': False, 'file_sha256': retained_hashes,
              'temporary_pc_download_removed': True, 'physical_serial_or_JTAG_used': False}
    Path(args.output).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
