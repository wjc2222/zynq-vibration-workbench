#!/usr/bin/env python3
"""Compare two gzip/newc rootfs archives without extracting or opening devices."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path

def entries(path):
    data = gzip.decompress(path.read_bytes())
    result, cursor = {}, 0
    while cursor + 110 <= len(data):
        header = data[cursor:cursor + 110]
        if header[:6] not in (b'070701', b'070702'):
            raise RuntimeError(f'Invalid newc header at {cursor}')
        values = [int(header[6 + index*8:14 + index*8],16) for index in range(13)]
        size, name_size = values[6], values[11]
        name = data[cursor+110:cursor+110+name_size-1].decode().removeprefix('./')
        cursor = (cursor+110+name_size+3) & ~3
        if name == 'TRAILER!!!':
            return result
        payload = data[cursor:cursor+size]
        result[name] = {'bytes':size,'sha256':hashlib.sha256(payload).hexdigest(),
                        'mode':values[1],'uid':values[2],'gid':values[3],
                        'rdevmajor':values[9],'rdevminor':values[10]}
        cursor = (cursor+size+3) & ~3
    raise RuntimeError('Missing newc trailer')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('baseline',type=Path)
parser.add_argument('candidate',type=Path)
parser.add_argument('output',type=Path)
args = parser.parse_args()
before, after = entries(args.baseline), entries(args.candidate)
added, removed = sorted(after.keys()-before.keys()), sorted(before.keys()-after.keys())
changed = {name:{'before':before[name],'after':after[name]} for name in sorted(before.keys()&after.keys()) if before[name] != after[name]}
report = {'scope':'Archive payload/Unix metadata comparison only; no filesystem extraction or device access',
          'baseline':str(args.baseline),'candidate':str(args.candidate),
          'baseline_sha256':hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
          'candidate_sha256':hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
          'before_entries':len(before),'after_entries':len(after),
          'added':{name:after[name] for name in added},'removed':{name:before[name] for name in removed},'changed':changed}
args.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'added':added,'removed':removed,'changed':list(changed)}))
