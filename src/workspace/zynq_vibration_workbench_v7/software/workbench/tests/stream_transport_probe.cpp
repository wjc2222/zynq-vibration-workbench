#include "capture_client.hpp"
#include "live_preview_model.hpp"
#include "stream_preview_contract.hpp"
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace vibration::workbench;
namespace {
void put32(unsigned char* p,quint32 n){for(int i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(8*i));}
void put64(unsigned char* p,quint64 n){put32(p,quint32(n));put32(p+4,quint32(n>>32));}
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void modelChecks(const QJsonObject& original){
    auto shifted=original;auto raw=QByteArray::fromBase64(original.value("raw_base64").toString().toLatin1());
    const quint64 first=4294967401ULL,step=50000000u/original.value("sample_rate_hz").toInt();
    for(int i=0;i<raw.size()/32;++i){auto* p=reinterpret_cast<unsigned char*>(raw.data())+i*32;put64(p,first+quint64(i));put64(p+8,1000000+(first+quint64(i))*step);}
    shifted["global_first_index"]=QString::number(first);shifted["first_tick"]=QString::number(1000000+first*step);shifted["last_tick"]=QString::number(1000000+(first+raw.size()/32-1)*step);
    shifted["frame_seq"]=QString::number(9007199254741009ULL);shifted["total_records"]="0";shifted["raw_base64"]=QString::fromLatin1(raw.toBase64());shifted["crc32"]=double(vibration::crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()),std::size_t(raw.size())));
    LivePreviewModel wide;require(wide.begin(shifted),"Model must accept original global uint64 indices beyond 2^32 and sequence beyond 2^53");
    require(wide.info().retainedRecords==std::size_t(raw.size()/32),"Model retains bounded original records");
    require(wide.view().endTime>4294967295.0/original.value("sample_rate_hz").toInt(),"Global time must not truncate to uint32");
    for(const auto& key:QStringList{"crc32","global_first_index","first_tick","last_tick","record_count","frame_seq","source","raw_base64"}){
        auto bad=original;
        if(key=="crc32")bad[key]=double(quint32(original.value(key).toDouble())^1u);
        else if(key=="record_count")bad[key]=1.5;
        else if(key=="source")bad[key]="adc_unconfirmed";
        else if(key=="raw_base64")bad[key]=original.value(key).toString()+"=";
        else bad[key]="18446744073709551616";
        bool rejected=false;try{(void)stream_contract::decode(bad);}catch(const std::exception&){rejected=true;}
        require(rejected,"Invalid stream payload/identity must be rejected");
    }
    LivePreviewModel quarantined;require(quarantined.begin(original),"Initial trusted frame");
    auto changed=original;changed["source"]="hardware_test";changed["hardware_confirmed"]=true;
    require(!quarantined.accept(changed)&&quarantined.info().quarantined,"Source changes quarantine this job");
}
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc!=4||QFileInfo::exists("/dev/ax7z020-capture"))return 2;
    const QString service=QString::fromUtf8(argv[1]),mode=QString::fromUtf8(argv[2]);const bool worker=QString::fromUtf8(argv[3])=="1";
    qputenv("AX7_CAPTURE_WORKER",worker?"1":"0");QTemporaryDir directory("/tmp/stream-transport-XXXXXX");if(!directory.isValid())return 3;
    std::unique_ptr<CaptureClient> client(new CaptureClient);client->setStreaming(true);client->setCommand(service,{"--test-fixture","--fixture-chunk-ms=5"});
    LivePreviewModel model;QJsonObject terminal,process,profile;QString error;quint64 frames=0,segments=0;bool stopped=false,modelChecked=false,blocked=false;QString job;
    const auto fail=[&](const QString& detail){if(error.isEmpty())error=detail;app.quit();};
    QObject::connect(client.get(),&CaptureClient::connectionFailed,&app,fail);
    QObject::connect(client.get(),&CaptureClient::requestRejected,&app,[&](const QString&,const QString& code,const QString& message){fail(code+": "+message);});
    QObject::connect(client.get(),&CaptureClient::previewRejected,&app,[&](const QString& text,bool){fail(text);});
    QObject::connect(client.get(),&CaptureClient::responseReceived,&app,[&](const QString& command,const QJsonObject& data){
        if(command=="capabilities"){
            if(!data.value("available").toBool()||data.value("backend")!="test_fixture"){fail("Wrong backend capabilities");return;}
            const bool timed=mode=="timed"||mode=="continuity";
            QJsonObject plan{{"mode",timed?"timed":"continuous"},{"sample_rate_hz",5000},{"pulses_per_rev",2},{"output_directory",directory.path()},{"segment_seconds",1}};
            if(timed)plan["duration_seconds"]=3;
            client->request("start",{{"plan",plan}});
        }else if(command=="start")job=data.value("job_id").toString();
    });
    QObject::connect(client.get(),&CaptureClient::liveFrame,&app,[&](const QJsonObject& frame){
        try{
            require(QThread::currentThread()==app.thread(),"Live delivery must stay on GUI/main thread");
            require(frame.value("schema")=="vibration.stream.v1","Stream schema preserved");
            require(frame.value("global_first_index").isString(),"Global indices remain decimal strings");
            if(!modelChecked){modelChecks(frame);modelChecked=true;}
            require(frames?model.accept(frame):model.begin(frame),"Independent live model must validate and render raw stream");++frames;
            if(mode=="continuity"&&!blocked){blocked=true;QThread::msleep(150);}
            if(mode=="stop"&&!stopped&&stream_contract::decimal(frame.value("global_first_index"))>=400){stopped=true;client->request("stop");}
            if(mode=="close"&&!stopped){stopped=true;client->stop();process=client->serviceProcessInfo();app.quit();}
        }catch(const std::exception& e){fail(QString::fromUtf8(e.what()));}
    });
    QObject::connect(client.get(),&CaptureClient::jobUpdated,&app,[&](const QJsonObject& data){
        if(data.value("event")=="segment_ready")++segments;
        if(data.value("event")=="session_done"){terminal=data;if(mode!="close")app.quit();}
    });
    QTimer::singleShot(15000,&app,[&]{fail("Stream transport watchdog expired");});client->connectService();app.exec();
    client->stop();process=client->serviceProcessInfo();profile=client->transportProfile();client.reset();quint64 persisted=0,files=0;
    try{
        QDirIterator entries(directory.path(),{"*.BIN"},QDir::Files,QDirIterator::Subdirectories);
        QStringList paths;while(entries.hasNext())paths.append(entries.next());paths.sort();
        for(const auto& path:paths){auto capture=vibration::load_capture(path.toUtf8().toStdString());require(capture.header.stream_segment&&capture.source_metadata.synthetic,"Stored segments must retain source and format");require(capture.records.front().index==persisted,"Adjacent stored segment indices");persisted+=capture.records.size();++files;}
        require(error.isEmpty(),qPrintable(error));require(frames>0&&modelChecked&&files>0,"Actual service preview and durable tail required");
        require(process.value("process_not_running").toBool()&&process.value("session_done_observed").toBool(),"Close must await validated session_done and child exit");
        if(mode!="close"){
            require(terminal.value("session_complete").toBool(),"Captured prefix durably saved");
            require(stream_contract::decimal(terminal.value("durable_records"))==persisted,"Durable status matches disk");
            require(terminal.value("state")==QJsonValue(mode=="timed"||mode=="continuity"?"completed":"stopped"),"Terminal state distinguishes timed/manual stop");
            require(segments==files,"Every atomic segment control event delivered");
        }
        if(mode=="timed"||mode=="continuity")require(persisted==15000&&files==3,"Timed record total and segments");
        if(mode=="continuity"){
            const auto box=profile.value("worker").toObject();require(worker&&box.value("frame_limit")==64&&box.value("high_frames").toInt()<=50&&box.value("mailbox_dropped_records").toDouble()==0&&model.info().previewDroppedRecords==0,"A 150ms GUI stall must retain contiguous bounded preview chunks");
            require(box.value("discarded_controls").toDouble()==0&&segments==3,"Batch ingest must retain every reliable segment control");
        }
    }catch(const std::exception& e){if(error.isEmpty())error=QString::fromUtf8(e.what());}
    const QJsonObject report{{"passed",error.isEmpty()},{"synthetic",true},{"mode",mode},{"worker",worker},{"frames",QString::number(frames)},{"segments",QString::number(files)},{"persisted",QString::number(persisted)},{"error",error},{"process",process},{"transport_profile",profile}};
    const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Compact);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);std::fputc('\n',stdout);return error.isEmpty()?0:1;
}
