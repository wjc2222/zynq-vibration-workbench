#!/usr/bin/env python3
"""QtCore-only r9 transport regression. Refuses physical capture devices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser();p.add_argument('--probe',required=True);p.add_argument('--service',required=True);p.add_argument('--evidence')
    a=p.parse_args()
    if Path('/dev/ax7z020-capture').exists():raise RuntimeError('Host QA refuses physical device')
    evidence_temp=tempfile.TemporaryDirectory(prefix='capture-worker-report-') if not a.evidence else None
    evidence=Path(a.evidence) if a.evidence else Path(evidence_temp.name)/'evidence';evidence.mkdir(parents=True,exist_ok=False)
    initial={x:sha(x) for x in [a.probe,a.service]}
    cases=[]
    negative=['schema','id','duplicate','escaped_duplicate','long','broken','disconnect_active','preview_start_plan','preview_bad_ppr','preview_missing','preview_crc','preview_padbits','preview_unicode','preview_fractional','preview_schema','preview_type','preview_extra','preview_bad_source','preview_duplicate_key','preview_duplicate','preview_generation','preview_source_change','preview_split','preview_truncated','preview_index','preview_tick','preview_flags']
    for worker in [False,True]:
        cases += [(worker,'service',rate) for rate in [50000,25000,10000,5000]]
        cases += [(worker,mode,5000) for mode in negative]
        cases += [(worker,'preview_status_terminal',5000)]
        cases += [(worker,mode,50000) for mode in ['missing','cancel','eof','destroy','reconnect']]
    cases += [(True,'slow',50000),(True,'slow',5000),(True,'overflow',50000),(True,'destroy_signal',50000),(True,'preview_stubborn',5000),(True,'silent',5000)]
    results=[]
    with tempfile.TemporaryDirectory(prefix='capture-worker-r9-') as temp:
        for index,(worker,mode,rate) in enumerate(cases):
            directory=Path(temp)/str(index);directory.mkdir()
            run=subprocess.run([a.probe,a.service,sys.executable,str(Path(__file__).with_name('fake_capture_peer.py')),str(directory),mode,str(rate),'1' if worker else '0'],env=dict(os.environ,AX7_CAPTURE_PROFILE='1'),capture_output=True,timeout=20)
            name=f'{index:03d}-w{int(worker)}-{mode}-{rate}'
            (evidence/(name+'.stderr')).write_bytes(run.stderr)
            (evidence/(name+'.json')).write_bytes(run.stdout)
            try:report=json.loads(run.stdout)
            except Exception:raise AssertionError((name,run.returncode,run.stdout[:500],run.stderr[:500]))
            assert run.returncode==0 and report['passed'],(name,report,run.stderr.decode(errors='replace'))
            assert b'QThread: Destroyed while thread is still running' not in run.stderr and b'QSocketNotifier:' not in run.stderr and b'QObject::' not in run.stderr
            if mode in ['slow','overflow']:
                output=report['profile']['service_profile']['output']['profile']['counters']
                assert output.get('replaced_previews',0)==0,(name,'worker failed to decouple pipe drain',output)
            results.append({'case':name,'passed':True,'frames':report['frames'],'gap_records':report['gap_records'],'wall_ms':report['wall_ms'],'stop_and_destroy_ms':report['stop_and_destroy_ms']})
            print(name,'PASS',report['frames'],report['gap_records'],flush=True)
    assert initial=={x:sha(x) for x in initial}
    report={'schema':'bounded-capture-worker-host-QA-v1','passed':True,'cases':results,'binaries_sha256':initial,'scope':'QtCore actual service test-fixture and adversarial child; no physical capture device, UI rendering or ARM acceptance. Worker opt-in exact AX7_CAPTURE_WORKER=1; default path also exercised.'}
    (evidence/'summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    if evidence_temp:evidence_temp.cleanup()


if __name__=='__main__':main()
