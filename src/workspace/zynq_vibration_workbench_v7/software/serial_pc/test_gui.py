"""Actual Qt/child-process end-to-end QA. Hardware use requires --port explicitly."""
import argparse
import hashlib
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication,QFileDialog
from app import Window,STYLE,CaptureDialog,configure_application


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--analyzer",required=True,type=Path)
    parser.add_argument("--port")
    parser.add_argument("--baud",type=int,default=921600)
    parser.add_argument("--bridge",default="/tmp/vibration-serial-bridge-baud-v2/host/vibration_serial_bridge")
    parser.add_argument("--service",default="/tmp/vibration-serial-bridge-baud-v2/service/software/acquisition/vibration_capture_service")
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    app=QApplication([]);configure_application(app)
    window=Window(args.analyzer);window.show()
    checks=[];messages=[];failure=[];previews=[]
    window.session.message.connect(lambda value:messages.append(value))
    window.session.message.connect(lambda value:previews.append((time.monotonic(),value["data"])) if value.get("event")=="preview" else None)
    window.session.failed.connect(failure.append)
    def check(condition,name):
        checks.append(dict(name=name,passed=bool(condition)))
        if not condition:raise AssertionError(name)
    def wait(condition,seconds=30):
        until=time.monotonic()+seconds
        while not condition():
            if time.monotonic()>until:raise TimeoutError(window.status.text()+" | "+window.notice.text()+" | "+repr(failure))
            QTest.qWait(25)
    def click(button):QTest.mouseClick(button,Qt.MouseButton.LeftButton)
    def grab(name):
        QTest.qWait(80);check(window.grab().save(str(args.output/name)),"screenshot "+name)
    outcome="failed"
    try:
        with tempfile.TemporaryDirectory(prefix="vibration-pc-qa-") as temporary:
            window.save_directory=Path(temporary)
            window.plan=dict(sample_rate_hz=5000,duration_seconds=5,pulses_per_rev=2)
            capture_plan=dict(window.plan)
            baud_index=window.baud.findData(args.baud)
            check(baud_index>=0,"requested baud is available in selector")
            window.baud.setCurrentIndex(baud_index)
            if args.port:window.session.open(args.port,args.baud)
            else:
                window.session.baud=args.baud
                window.session.open_process(["wsl.exe","-d","FPGA-Zynq-Build","-u","fpga","--",
                    args.bridge,"--service",args.service,
                    "--test-fixture","--baud",str(args.baud)])
            wait(lambda:window.session.connected and window.available,60)
            check(window.baud.currentData()==window.session.baud==args.baud,"selector and session baud agree")
            wait(lambda:any(m.get("event")=="records_ready" and m["data"].get("ok") for m in messages))
            check(window.start_btn.isEnabled(),"production start entry enabled after caps")
            click(window.start_btn);wait(lambda:bool(window.last_preview),15)
            check(window.active,"live preview during active capture")
            from plots import Plot
            check(len(window.waveforms.findChildren(Plot))==1,"one time canvas, no stacked rows")
            check(window.waveforms._stats_scope=="raw_window","live amplitude comes from raw-window bridge statistics")
            check(all(target.live_mode for target in window.plot_targets),"all plots enter live mode")
            grab("01-live-1280.png")
            click(window.pause_btn);before=repr(window.waveforms.data);pause_start_packets=len(previews)
            wait(lambda:len(previews)>pause_start_packets and window.active,3)
            paused_new_previews=len(previews)-pause_start_packets
            check(paused_new_previews>0,"new preview arrived while display was paused")
            check(repr(window.waveforms.data)==before,"pause freezes display only")
            click(window.pause_btn)
            window.waveforms.zoom(.5)
            check(window.paused and all(target.paused for target in window.plot_targets),"time navigation pauses every plot")
            window.tabs.setCurrentIndex(1);QTest.qWait(30)
            check(window.paused,"tab switch preserves frozen view")
            window.spectrum_view.resume_live()
            check(not window.paused and all(not target.paused for target in window.plot_targets),"latest resumes all plots without recursion")
            window.tabs.setCurrentIndex(0)
            window.waveforms.set_y_range(-1.25,1.25)
            click(window.waveforms.channel_boxes[1]);click(window.waveforms.channel_boxes[2])
            check(len(window.waveforms.plot.series)==3,"three checkbox-selected channels overlaid")
            click(window.waveforms.speed_box)
            check(window.waveforms.plot.y_label=="rpm" and len(window.waveforms.plot.series)==1,"speed uses independent rpm axis")
            click(window.waveforms.speed_button);QTest.qWait(30)
            check(window.waveforms.speed_dialog.isVisible(),"speed value button opens readout")
            window.waveforms.speed_dialog.close();click(window.waveforms.speed_box)
            check(len(window.waveforms.plot.series)==3,"leaving speed restores vibration selection")
            wait(lambda:window.result is not None,45)
            check(window.result["header"]["records"]==25000,"full record count 25000")
            check(window.result["speed"]["pulses_per_rev"]==2,"capture PPR2 carried into analysis")
            check(window.result["diagnosis"]["classification"]=="not_classified","no fabricated fault class")
            check(not window.remote_busy,"complete analysis unlocks controls")
            check(bool(window.record_id),"complete record offered for download")
            check(window.waveforms._stats_scope=="complete_record","offline amplitude uses full raw record stats")
            check(all(not target.live_mode for target in window.plot_targets),"completed record leaves live mode")
            check(window.waveforms.viewport.y_range is None,"new full-record units reset manual live amplitude")
            window.waveforms.zoom(.5);window.waveforms.pan(.25)
            check(window.waveforms.viewport.x_range is not None,"offline zoom and horizontal movement")
            window.waveforms.set_y_range(-2,2);window.waveforms.reset_view()
            check(window.waveforms.viewport.x_range is None and window.waveforms.viewport.y_range is None,"restore view resets manual axes")
            grab("02-complete-1280.png")
            window.tabs.setCurrentIndex(1);grab("03-spectrum-1280.png")
            window.tabs.setCurrentIndex(2);grab("04-detection-1280.png")
            window.resize(1024,650);window.tabs.setCurrentIndex(0);grab("05-time-1024.png")
            dialog=CaptureDialog(window.plan,window);dialog.show();QTest.qWait(100)
            check(dialog.grab().save(str(args.output/"06-parameters.png")),"parameter actual Qt render");dialog.close()
            original_picker=QFileDialog.getExistingDirectory
            QFileDialog.getExistingDirectory=lambda *_:temporary
            try:click(window.download_btn)
            finally:QFileDialog.getExistingDirectory=original_picker
            check(window.download_pending or window.download is not None,"download marks pending immediately")
            check(not window.start_btn.isEnabled(),"capture disabled during pending download")
            wait(lambda:window.download is None and not window.download_pending and window.local_path is not None and window.analysis_process is None,180 if args.port else 45)
            check(window.result is not None,"download automatically locally analyzed")
            check(window.result["speed"]["pulses_per_rev"]==2,"known PPR2 overrides GUI fallback1 without conflict")
            check(window.local_path.with_suffix(".meta.json").is_file(),"paired metadata downloaded")
            receipt=json.loads((window.local_path.parent/"transfer-receipt.json").read_text(encoding="utf-8"))
            check(receipt["transfer_integrity"]=="passed","pair SHA256 verified before publication")
            retained=args.output/"downloaded-record";retained.mkdir()
            independently_verified=[]
            for item in receipt["files"]:
                source=window.local_path.parent/item["name"]
                actual=hashlib.sha256(source.read_bytes()).hexdigest()
                check(source.stat().st_size==item["bytes"] and actual==item["sha256"],"independent downloaded "+item["kind"]+" SHA256")
                destination=retained/item["name"];shutil.copyfile(source,destination)
                check(hashlib.sha256(destination.read_bytes()).hexdigest()==actual,"retained "+item["kind"]+" evidence hash")
                independently_verified.append(dict(item,verified_sha256=actual,retained_path="downloaded-record/"+item["name"]))
            shutil.copyfile(window.local_path.parent/"transfer-receipt.json",retained/"transfer-receipt.json")
            (args.output/"download-proof.json").write_text(json.dumps(dict(record_id=receipt["record_id"],files=independently_verified,header=window.result["header"],source=window.result["source_metadata"]),indent=2),encoding="utf-8")
            # An explicit new start must clear old diagnosis before receiving data.
            window.plan["duration_seconds"]=8;click(window.start_btn)
            check(window.result is None,"new capture clears old result")
            wait(lambda:window.active and bool(window.current_job) and window.last_preview is not None,15)
            clicked=time.monotonic();click(window.cancel_btn);wait(lambda:not window.active,15)
            check(window.result is None and not window.record_id,"cancel does not publish complete record")
            cancel_seconds=time.monotonic()-clicked
            check(window.start_btn.isEnabled(),"cancel allows restart")
            grab("07-cancelled.png")
            window.session.disconnect();wait(lambda:window.session.phase=="closed",8)
            shutdown_id=window.session.shutdown_request_id
            shutdown_responses=[m for m in messages if m.get("command")=="shutdown" and m.get("id")==shutdown_id]
            check(shutdown_id is not None and len(shutdown_responses)==1 and shutdown_responses[0].get("ok") is True and shutdown_responses[0].get("data",{}).get("accepted") is True,"matching successful shutdown response received")
            check(window.session.shutdown_acknowledged,"session closed only after shutdown acknowledgement")
            check(not window.active and not window.remote_busy and window.download is None,"disconnect clears busy state")
            check(not failure,"no session failures including shutdown")
            outcome="passed"
    finally:
        window.closing_window=True
        if window.download:window.download.abort();window.download=None
        window.session.disconnect()
        end=time.monotonic()+4
        while window.session.phase!="closed" and time.monotonic()<end:QTest.qWait(25)
        window.close()
        if failure:outcome="failed"
        report=dict(outcome=outcome,physical_port=args.port,baud=args.baud,checks=checks,
            failure=failure,preview_packets=len(previews),preview_intervals=[previews[i][0]-previews[i-1][0] for i in range(1,len(previews)) if previews[i][1]["job_id"]==previews[i-1][1]["job_id"]],
            messages=[{k:v for k,v in m.items() if k!="data"} for m in messages],
            source_sha256={name:hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest() for name in ("app.py","single_waveform.py","session.py","serial_device.py","plots.py","protocol.py","test_gui.py")},
            shutdown=dict(request_id=window.session.shutdown_request_id,acknowledged=window.session.shutdown_acknowledged))
        if "cancel_seconds" in locals():report["observed_cancel_seconds"]=cancel_seconds
        if "paused_new_previews" in locals():report["paused_new_previews"]=paused_new_previews
        if "capture_plan" in locals():report["capture_plan"]=capture_plan
        (args.output/"report.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps({"outcome":outcome,"checks":len(checks),"preview_packets":len(previews)},ensure_ascii=False))
    if outcome!="passed":raise AssertionError("GUI QA did not pass; see report.json")


if __name__=="__main__":main()
