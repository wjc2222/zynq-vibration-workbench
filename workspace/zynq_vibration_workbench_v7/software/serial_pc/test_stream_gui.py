"""Real Qt and bridge/service process integration, explicit synthetic sources only.

--port optionally uses the real UART, launching the deployed PL hardware-test
mode explicitly. It does not represent physical sensors or mechanical faults.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time
import types
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM","offscreen")
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication,QFileDialog,QMessageBox
from app import Window,CaptureDialog,configure_application

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output",type=Path,required=True);p.add_argument("--analyzer",type=Path,required=True)
    p.add_argument("--bridge",default="/tmp/continuous-v4-bridge-host/software/stream_bridge/vibration_stream_bridge")
    p.add_argument("--service",default="/tmp/continuous-v4-bridge-host/software/streaming/vibration_stream_service")
    p.add_argument("--port");p.add_argument("--baud",type=int,default=921600)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    app=QApplication([]);configure_application(app);window=Window(a.analyzer);window.show()
    window.save_directory=a.output/"downloads"
    messages=[];failures=[];checks=[];outcome="failed"
    window.session.message.connect(messages.append);window.session.failed.connect(failures.append)
    def check(value,name):
        checks.append({"name":name,"passed":bool(value)})
        if not value:raise AssertionError(name)
    def wait(predicate,seconds=35):
        until=time.monotonic()+seconds
        while not predicate():
            if failures:raise RuntimeError(str(failures))
            if time.monotonic()>until:raise TimeoutError(window.status.text()+" | "+window.notice.text())
            QTest.qWait(20)
    def click(button):QTest.mouseClick(button,Qt.MouseButton.LeftButton)
    def snap(name):app.processEvents();check(window.grab().save(str(a.output/name)),"render "+name)
    def terminal(job):return next((m["data"] for m in reversed(messages) if m.get("event")=="session_done" and m["data"].get("job_id")==job),None)
    def raw_display(label, rate, local=False):
        context=window.waveforms._sampling_context
        check(not context.get("summary",False),label+": never substitute extrema summary for raw samples")
        if local:
            check(window.local_waveform is not None and callable(context.get("provider")),label+": raw file viewport provider retained")
        else:
            check(context.get("detail") is not None,label+": raw detail range present")
            check(all(len(channel)>32 for channel in window.waveforms.data),label+": all three channels retain contiguous sample window")
            check(all(math.isclose(b[0]-a[0],1/rate,rel_tol=1e-7,abs_tol=1e-12)
                      for channel in window.waveforms.data for a,b in zip(channel,channel[1:])),
                  label+": timestamp spacing equals actual sample interval")
    try:
        window.plan=dict(mode="timed",sample_rate_hz=5000,duration_seconds=12,pulses_per_rev=1)
        if a.port:
            # Test-only launch override; production BoardSession never enables this.
            def launch(session):
                session.console.clear();session._phase("launching");session.deadline=time.monotonic()+20
                command=("AX7_PC_TTY=$(stty -g); stty raw -echo "+str(session.baud)+"; sleep 1; "
                         "/usr/bin/vibration_stream_bridge --hardware-test --baud "+str(session.baud)+
                         " --service /usr/bin/vibration_stream_service; stty \"$AX7_PC_TTY\"; printf '\\nAX7_PC_RETURNED\\n'\r")
                session._write(command.encode("ascii"));session._later(180,lambda:session.port.setBaudRate(session.baud) if session.port else None)
                session._later(1350,session._bridge_ready)
            window.session._launch=types.MethodType(launch,window.session)
            window.session.open(a.port,a.baud)
        else:
            root=subprocess.check_output(["wsl.exe","-d","FPGA-Zynq-Build","-u","fpga","--","mktemp","-d","/tmp/stream-pc-qa-XXXXXXXX"],text=True).strip()
            if not root.startswith("/tmp/stream-pc-qa-"):raise ValueError("unexpected fixture directory")
            window.session.baud=a.baud
            window.session.open_process(["wsl.exe","-d","FPGA-Zynq-Build","-u","fpga","--",a.bridge,"--service",a.service,"--test-fixture","--output-root",root,"--baud",str(a.baud)])
        wait(lambda:window.session.connected and window.available,60)
        click(window.start_btn);wait(lambda:window.last_preview is not None)
        job=window.current_job;check(window.active,"live preview during acquisition")
        check(window.last_preview["synthetic"] is True,"synthetic provenance displayed")
        check(window.last_preview["source"]==("hardware_test" if a.port else "test_fixture"),"correct test source")
        check(window.waveforms._stats_scope=="raw_window","amplitude computed before decimation")
        raw_display("live preview",5000)
        click(window.waveforms.channel_boxes[1]);click(window.waveforms.channel_boxes[2])
        check(len(window.waveforms.plot.series)==3,"selected channels overlay")
        window.waveforms.zoom(.5);check(window.paused,"mouse navigation pauses display only")
        check(window.active,"capture remains active while viewing earlier window")
        click(window.pause_btn);check(not window.paused,"resume latest display")
        click(window.waveforms.speed_box);check(window.waveforms.plot.y_label=="rpm","independent speed axis")
        click(window.waveforms.speed_box)
        snap("01-live.png")
        wait(lambda:any(m.get("event")=="analysis" and m["data"].get("scope")=="completed_segment" for m in messages),35)
        # With a 10s segment and 12s target, first analysis must not terminate UI.
        first_analysis=next(m["data"] for m in messages if m.get("event")=="analysis")
        check(first_analysis["scope"]=="completed_segment","segment analysis is explicitly scoped")
        wait(lambda:terminal(job) is not None)
        done=terminal(job)
        check(done["state"]=="completed" and done["durable_records"]=="60000" and done["segment_count"]=="2","timed 12 seconds saved in two segments")
        check(not window.active and not window.remote_busy,"terminal save unlocks controls")
        wait(lambda:window.record_id and window.result is not None)
        check(window.result["diagnosis"]["classification"]=="not_classified","no invented fault category")
        raw_display("terminal and automatic segment analysis",5000)
        window.request_records();wait(lambda:window.record_dialog is not None)
        check(len(window.records)>=2,"SD catalog lists sealed records")
        window.record_dialog.close()
        chosen=next(r for r in window.records if r["record_id"].startswith(job+"-") and r["record_id"].endswith("000001"))
        window.remote_busy=True;window.remote_analysis_request_id=window.send("analyze_record",record_id=chosen["record_id"])
        wait(lambda:not window.remote_busy and window.record_id==chosen["record_id"])
        check(window.result["header"]["records"]==10000,"selected tail segment analyzed independently")
        raw_display("selected SD segment",5000)
        snap("02-saved-segment.png")
        window.save_directory.mkdir(exist_ok=True)
        original=QFileDialog.getExistingDirectory;QFileDialog.getExistingDirectory=lambda *_:str(window.save_directory)
        try:click(window.download_btn)
        finally:QFileDialog.getExistingDirectory=original
        wait(lambda:not window.download_pending and window.download is None and window.local_path is not None and window.analysis_process is None,180)
        check(window.result is not None and window.result["integrity"]=="passed","SD pair downloads and new Windows core verifies CAPSTRM1")
        raw_display("downloaded file analysis",5000,local=True)
        receipt=json.loads((window.local_path.parent/"transfer-receipt.json").read_text())
        for entry in receipt["files"]:
            check(hashlib.sha256((window.local_path.parent/entry["name"]).read_bytes()).hexdigest()==entry["sha256"],"download SHA "+entry["kind"])
        # Only remove this run's synthetic tail after a verified PC copy exists.
        # Never select prior SD records or a broad directory for cleanup.
        check(chosen["record_id"].startswith(job+"-") and chosen["synthetic"] is True,"delete target is this run's synthetic data only")
        with patch.object(QMessageBox,"exec",return_value=QMessageBox.StandardButton.No):
            check(not window.delete_records([chosen]),"cancel deletion sends no mutation")
        with patch.object(QMessageBox,"exec",return_value=QMessageBox.StandardButton.Yes):
            check(window.delete_records([chosen]),"confirmed selected SD deletion submitted")
        delete_id=window.delete_request_id
        wait(lambda:window.delete_request_id is None and window.record_dialog is not None)
        reply=next(m for m in messages if m.get("id")==delete_id)
        check(reply.get("ok") is True and reply["data"]["results"][0]["deleted"] is True,"backend deletes confirmed record")
        check(all(r["record_id"]!=chosen["record_id"] for r in window.records),"deleted SD record removed from list")
        check(any(r["record_id"]==job+"-000000" for r in window.records),"unselected session segment retained")
        check(window.local_path is not None and window.local_path.is_file(),"verified PC copy remains after SD deletion")
        snap("03-sd-delete.png");window.record_dialog.close()
        window.plan=dict(mode="continuous",sample_rate_hz=5000,pulses_per_rev=1)
        click(window.start_btn);wait(lambda:window.last_preview is not None);job=window.current_job
        click(window.cancel_btn);check(window.active and window.stop_pending,"Stop waits for tail durability")
        wait(lambda:terminal(job) is not None)
        done=terminal(job);check(done["state"]=="stopped" and done["session_complete"] and int(done["durable_records"])>0,"manual Stop retains all received data")
        snap("03-manual-stop.png")
        click(window.start_btn);wait(lambda:window.last_preview is not None);job=window.current_job
        window.session.disconnect();wait(lambda:window.session.phase=="closed",60)
        check(window.session.shutdown_acknowledged,"disconnect acknowledged after service exit")
        done=terminal(job);check(done is not None and done["session_complete"] and done["state"]=="stopped","disconnect preserves tail")
        check(not failures,"no connection or protocol failures")
        outcome="passed"
    finally:
        window.closing_window=True;window.session.disconnect()
        until=time.monotonic()+120
        while window.session.phase!="closed" and time.monotonic()<until:QTest.qWait(25)
        window.active=False;window.close()
        report={"outcome":outcome,"port":a.port,"baud":a.baud,"source":"PL test pattern through actual UART/DMA/SD" if a.port else "host synthetic fixture", "checks":checks,"failures":failures,
                "events":[m for m in messages if m.get("event") in ("job_status","session_done","segment_ready")],
                "sources":{n:hashlib.sha256(Path(__file__).with_name(n).read_bytes()).hexdigest() for n in ("app.py","session.py","test_stream_gui.py")}}
        if not a.port and "root" in locals():report["host_fixture_directory"]=root
        (a.output/"report.json").write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding="utf-8")
    print(json.dumps({"outcome":outcome,"checks":len(checks)}))

if __name__=="__main__":main()
