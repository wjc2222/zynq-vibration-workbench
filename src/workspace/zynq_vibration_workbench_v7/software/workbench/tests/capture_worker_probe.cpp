#include "capture_client.hpp"
#include <vibration/core.hpp>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <memory>
using vibration::workbench::CaptureClient;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc!=8)return 2;
    if(QFileInfo::exists("/dev/ax7z020-capture"))return 3;
    const QString service=QString::fromUtf8(argv[1]),python=QString::fromUtf8(argv[2]),peer=QString::fromUtf8(argv[3]),directory=QString::fromUtf8(argv[4]),mode=QString::fromUtf8(argv[5]);
    const int rate=QString::fromUtf8(argv[6]).toInt();const bool worker=QString::fromUtf8(argv[7])=="1";
    qputenv("AX7_CAPTURE_WORKER",worker?"1":"0");
    std::unique_ptr<CaptureClient> client(new CaptureClient);QString error;QJsonObject terminal,profile,process;
    QJsonArray rejectionMessages;QStringList jobs;quint64 frames=0,records=0,gap=0,end=0;int rejected=0,late=0,wrongThread=0,capabilities=0;
    bool finished=false,blocked=false,reconnected=false,destroyed=false;quint64 pid=0;QElapsedTimer wall;wall.start();
    const bool fake=mode.startsWith("preview_")||mode=="disconnect_active"||QStringList{"schema","id","duplicate","escaped_duplicate","long","broken","silent"}.contains(mode);
    if(mode=="missing")client->setCommand("/tmp/nonexistent-capture-worker-r9");
    else if(fake)client->setCommand(python,{"-B",peer,mode,"/tmp/transport-only-never-created.BIN"});
    else client->setCommand(service,{"--test-fixture"});
    const int duration=mode=="overflow"?5:(mode=="slow"?3:(QStringList{"cancel","eof","destroy","reconnect"}.contains(mode)?40:1));
    QObject::connect(client.get(),&CaptureClient::responseReceived,&app,[&](const QString& command,const QJsonObject& data){
        if(QThread::currentThread()!=app.thread())++wrongThread;
        if(command=="capabilities"){
            ++capabilities;process=client->serviceProcessInfo();pid=quint64(process.value("pid").toDouble());
            client->request("start",{{"preview",true},{"plan",QJsonObject{{"sample_rate_hz",rate},{"duration_seconds",reconnected?1:duration},{"pulses_per_rev",1},{"output_directory",directory}}}});
        }
        if(command=="start")jobs.append(data.value("job_id").toString());
        if(command=="status"&&mode=="preview_status_terminal"){terminal=data;finished=true;QTimer::singleShot(30,&app,&QCoreApplication::quit);}
    });
    QObject::connect(client.get(),&CaptureClient::liveFrame,&app,[&](const QJsonObject& f){
        if(QThread::currentThread()!=app.thread())++wrongThread;
        if(finished)++late;
        const auto first=quint64(f.value("first_record").toDouble()),count=quint64(f.value("record_count").toDouble());
        if(first<end){error="overlapping delivery";app.quit();return;}
        gap+=first-end;end=first+count;records+=count;++frames;
        if(mode=="preview_status_terminal")client->request("status",{{"job_id",f.value("job_id")}});
        if(mode=="destroy_signal"){profile=client->transportProfile();client.reset();destroyed=true;QTimer::singleShot(10,&app,&QCoreApplication::quit);return;}
        if(!blocked&&mode=="preview_stubborn"){blocked=true;QTimer::singleShot(20,&app,[&]{client->stop();app.quit();});}
        if(!blocked&&(mode=="slow"||mode=="overflow")){blocked=true;QThread::msleep(mode=="slow"?1000:3300);}
        if(!blocked&&QStringList{"cancel","eof","destroy","reconnect"}.contains(mode)){
            blocked=true;QTimer::singleShot(150,&app,[&]{
                if(mode=="cancel")client->request("cancel",{{"job_id",jobs.last()}});
                else if(mode=="eof"){client->stop();profile=client->transportProfile();process=client->serviceProcessInfo();app.quit();}
                else if(mode=="destroy"){profile=client->transportProfile();client.reset();destroyed=true;QTimer::singleShot(10,&app,&QCoreApplication::quit);}
                else{client->stop();reconnected=true;end=0;client->connectService();}
            });
        }
    });
    QObject::connect(client.get(),&CaptureClient::previewRejected,&app,[&](const QString& why,bool quarantine){++rejected;rejectionMessages.append(QJsonObject{{"reason",why},{"quarantined",quarantine}});});
    QObject::connect(client.get(),&CaptureClient::connectionFailed,&app,[&](const QString& text){error=text;app.quit();});
    QObject::connect(client.get(),&CaptureClient::jobUpdated,&app,[&](const QJsonObject& data){
        if(QThread::currentThread()!=app.thread())++wrongThread;
        if(QStringList{"completed","failed","cancelled"}.contains(data.value("state").toString())){
            terminal=data;finished=true;QTimer::singleShot(30,&app,&QCoreApplication::quit);
        }
    });
    QTimer::singleShot(14000,&app,[&]{error="probe timeout";app.quit();});client->connectService();app.exec();
    QElapsedTimer stop;stop.start();if(client){client->stop();profile=client->transportProfile();process=client->serviceProcessInfo();client.reset();}
    const double stopMs=stop.nsecsElapsed()/1e6;
    const bool released=pid>0&&!QFileInfo::exists(QString("/proc/%1").arg(pid));
    bool passed=wrongThread==0&&late==0&&stopMs<3600;
    const auto w=profile.value("worker").toObject();
    if(worker)passed=passed&&w.value("enabled").toBool()&&w.value("high_records").toDouble()<=rate*2&&w.value("high_frames").toInt()<=64&&w.value("high_accounted_payload_bytes").toDouble()<=16777216&&w.value("max_frames_per_drain").toInt()<=2;
    if(mode=="preview_stubborn")passed=passed&&error.isEmpty()&&frames>0&&released&&stopMs<3600;
    else if(fake){
        const bool fatal=QStringList{"preview_start_plan","preview_truncated","disconnect_active","schema","id","duplicate","escaped_duplicate","long","broken","silent"}.contains(mode);
        passed=passed&&(fatal?!error.isEmpty():(error.isEmpty()&&terminal.value("state")=="completed"));
        if(!fatal&&mode!="preview_split"&&mode!="preview_status_terminal")passed=passed&&rejected>0;
        if(mode=="preview_split")passed=passed&&frames==2&&records==4000;
        if(mode=="preview_status_terminal")passed=passed&&frames==1&&late==0;
    }else if(mode=="missing")passed=passed&&!error.isEmpty()&&frames==0;
    else if(mode=="eof"||mode=="destroy"||mode=="destroy_signal")passed=passed&&error.isEmpty()&&frames>0&&terminal.isEmpty()&&released;
    else{
        passed=passed&&error.isEmpty()&&released;
        if(mode=="cancel")passed=passed&&terminal.value("state")=="cancelled"&&terminal.value("output_path").toString().isEmpty();
        else{
            passed=passed&&terminal.value("state")=="completed";
            try{const auto capture=vibration::load_capture(terminal.value("output_path").toString().toStdString());passed=passed&&capture.header.completed_records==quint64(rate*(reconnected?1:duration));}catch(const std::exception& e){error=e.what();passed=false;}
            if(mode=="slow"&&worker)passed=passed&&gap==0&&records==quint64(rate*duration)&&w.value("mailbox_dropped_records").toInt()==0;
            if(mode=="overflow"&&worker)passed=passed&&gap>0&&w.value("mailbox_dropped_records").toDouble()>0;
            if(mode=="reconnect")passed=passed&&jobs.size()==2&&jobs[0]!=jobs[1]&&capabilities==2;
        }
    }
    const QJsonObject report{{"passed",passed},{"mode",mode},{"rate",rate},{"worker",worker},{"frames",double(frames)},{"records",double(records)},{"gap_records",double(gap)},{"late",late},{"wrong_signal_thread",wrongThread},{"rejected",rejected},{"rejections",rejectionMessages},{"error",error},{"terminal",terminal},{"jobs",QJsonArray::fromStringList(jobs)},{"process",process},{"child_pid",double(pid)},{"child_released",released},{"destroyed_in_callback",destroyed},{"stop_and_destroy_ms",stopMs},{"wall_ms",wall.nsecsElapsed()/1e6},{"profile",profile}};
    const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Compact);std::fwrite(bytes.constData(),1,bytes.size(),stdout);return passed?0:1;
}
