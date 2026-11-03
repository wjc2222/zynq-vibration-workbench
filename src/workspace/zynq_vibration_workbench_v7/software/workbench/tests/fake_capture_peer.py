#!/usr/bin/env python3
"""Deliberately malformed child for bounded transport rejection QA only."""
import json
import base64
import os
import struct
import signal
import sys
import time
import zlib

SCHEMA = "vibration.capture.v2"

def send(value):
    print(json.dumps(value, separators=(",", ":")), flush=True)

def live_peer(mode):
    """Explicit adversarial transport only, no device or file creation."""
    plan = None
    job = "qa_live_job"
    terminal = False
    def response(request, data):
        send(dict(schema=SCHEMA, type="response", request_id=request["request_id"], command=request["command"], ok=True, data=data))
    def status(state="running"):
        return dict(schema=SCHEMA, type="event", event="job_status", job_id=job, state=state,
                    records_completed=5000 if state=="completed" else 400, requested_records=5000,
                    synthetic=True, hardware_confirmed=False, output_path=sys.argv[2] if state=="completed" else "")
    def frame(first=0, seq=1, count=200):
        raw = b"".join(struct.pack("<QQhhhHII", i, 100000+i*10000, 100, -50, 20, 5, 0, 0) for i in range(first, first+count))
        return dict(schema=SCHEMA,type="event",event="preview",job_id=job,generation="1",frame_seq=seq,
                    first_record=first,record_count=count,committed_records=first+count,requested_records=5000,
                    sample_rate_hz=5000,clock_hz=50000000,ppr=1,adc_range_uv=5000000,channel_map=0x50301,record_bytes=32,
                    source="test_fixture",hardware_confirmed=False,synthetic=True,source_flags=1,validity_flags=27,
                    raw_base64=base64.b64encode(raw).decode("ascii"),crc32=zlib.crc32(raw),dropped_preview_records=0)
    for line in sys.stdin:
        request=json.loads(line);command=request["command"]
        if command=="capabilities":
            response(request,dict(backend="test_fixture",requested_source="host_fixture",available=True,hardware_confirmed=False,synthetic=True,
                clock_hz=50000000,adc_range_uv=5000000,channel_map=0x50301,supported_rates_hz=[50000,25000,10000,5000],preview_available=True))
        elif command=="validate_plan":
            response(request,dict(valid=True,ready=True))
        elif command=="start":
            plan=dict(request["plan"]);plan["requested_records"]=plan["sample_rate_hz"]*plan["duration_seconds"]
            if mode=="preview_start_plan":plan["pulses_per_rev"]=2
            response(request,dict(job_id=job,state="preparing",plan=plan));send(status())
            if mode in ("preview_split","preview_truncated"):
                line=json.dumps(frame(0,1,2000),separators=(",", ":"))+"\n"
                if mode=="preview_truncated":
                    sys.stdout.write(line[:len(line)//2]);sys.stdout.flush();os._exit(0)
                for offset in range(0,len(line),1024):
                    sys.stdout.write(line[offset:offset+1024]);sys.stdout.flush();time.sleep(0.001)
                send(frame(2000,2,2000));send(status("completed"));terminal=True;send(frame(400,3));continue
            if mode=="disconnect_active":
                time.sleep(0.05);os._exit(0)
            if mode=="preview_status_terminal":send(frame());continue
            if mode=="preview_stubborn":
                signal.signal(signal.SIGTERM,signal.SIG_IGN);send(frame())
                while True:time.sleep(30)
            bad=frame()
            if mode=="preview_bad_ppr":bad["ppr"]=2
            elif mode=="preview_missing":del bad["raw_base64"]
            elif mode=="preview_crc":bad["crc32"]^=1
            elif mode=="preview_padbits":bad["raw_base64"]=bad["raw_base64"][:-3]+"B=="
            elif mode=="preview_unicode":bad["raw_base64"]="\u0141"+bad["raw_base64"][1:]
            elif mode=="preview_fractional":bad["clock_hz"]+=0.5
            elif mode=="preview_schema":bad["schema"]="wrong.preview"
            elif mode=="preview_type":bad["type"]="response"
            elif mode=="preview_extra":bad["extra"]=True
            elif mode=="preview_bad_source":bad.update(source="adc_unconfirmed",hardware_confirmed=True,synthetic=False,source_flags=0)
            elif mode in ("preview_index","preview_tick","preview_flags"):
                raw=bytearray(base64.b64decode(bad["raw_base64"]))
                if mode=="preview_index":struct.pack_into("<Q",raw,0,99)
                elif mode=="preview_tick":struct.pack_into("<Q",raw,40,110001)
                else:struct.pack_into("<H",raw,22,0x8001)
                bad["raw_base64"]=base64.b64encode(raw).decode("ascii");bad["crc32"]=zlib.crc32(raw)
            if mode=="preview_duplicate_key":
                text=json.dumps(bad);print(text[:-1]+',"crc32":'+str(bad["crc32"])+"}",flush=True)
            else:send(bad)
            if mode=="preview_duplicate":send(bad)
            if mode in ("preview_generation","preview_source_change"):
                changed=frame(200,2)
                if mode=="preview_generation":changed["generation"]="2"
                else:changed.update(source="adc_unconfirmed",hardware_confirmed=True,synthetic=False,source_flags=0)
                send(changed)
            send(frame(200,2));send(status("completed"));terminal=True
            # Valid data after a terminal job must never reach the renderer.
            send(frame(400,3))
        elif command=="status":
            result=status("completed" if terminal or mode=="preview_status_terminal" else "running");result.pop("schema");result.pop("type");result.pop("event");response(request,result)
            if mode=="preview_status_terminal":send(frame(200,2))
        elif command=="cancel":response(request,dict(job_id=job,accepted=True))
        elif command=="shutdown":response(request,dict(accepted=True));return

if len(sys.argv)>1 and (sys.argv[1].startswith("preview_") or sys.argv[1]=="disconnect_active"):
    live_peer(sys.argv[1]);raise SystemExit(0)

request = json.loads(sys.stdin.readline())
mode = sys.argv[1]
response = dict(schema="vibration.capture.v2", type="response", request_id=request["request_id"],
                command="capabilities", ok=True, data={})
if mode == "silent":
    time.sleep(30)
elif mode == "schema":
    response["schema"] = "wrong.version"
    print(json.dumps(response), flush=True)
elif mode == "id":
    response["request_id"] = "unknown_peer_request"
    print(json.dumps(response), flush=True)
elif mode == "duplicate":
    line = json.dumps(response)
    print(line[:-1] + ', "ok": false}', flush=True)
elif mode == "escaped_duplicate":
    response["data"]={"quotes":"a\\\"b", "slashes":"a\\\\", "unicode":"中"}
    line=json.dumps(response)
    print(line[:-1] + ', "o\\u006b": false}', flush=True)
elif mode == "long":
    print("x" * 9000, flush=True)
else:
    print("broken JSON", flush=True)
# Let the client close stdin as part of safe shutdown.
for unused in sys.stdin:
    pass
