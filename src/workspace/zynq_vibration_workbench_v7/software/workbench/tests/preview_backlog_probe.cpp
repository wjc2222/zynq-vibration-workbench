#include "capture_client.hpp"
#include "live_preview_model.hpp"
#include "stream_preview_contract.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace vibration::workbench;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc!=5||QFileInfo::exists("/dev/ax7z020-capture"))return 2;
    const QString service=QString::fromUtf8(argv[1]),output=QString::fromUtf8(argv[2]);bool ok=false;
    const int stallMs=QString::fromUtf8(argv[3]).toInt(&ok);if(!ok||stallMs<0||stallMs>4000)return 2;
    const QString expectation=QString::fromUtf8(argv[4]);if(expectation!="observe"&&expectation!="continuous"&&expectation!="overflow")return 2;
    qputenv("AX7_CAPTURE_WORKER","1");qputenv("AX7_CAPTURE_PROFILE","1");qputenv("AX7_LIVE_DEMAND_ANALYSIS","1");
    QTemporaryDir directory("/tmp/preview-backlog-XXXXXX");if(!directory.isValid())return 3;
    auto client=std::make_unique<CaptureClient>();client->setStreaming(true);client->setCommand(service,{"--test-fixture","--fixture-chunk-ms=40"});
    LivePreviewModel model;QJsonObject terminal;QJsonArray delivered;QString error;bool stalled=false;
    quint64 priorEnd=0,observedGap=0,maximumNaNs=0,frames=0;QElapsedTimer clock;clock.start();
    const auto fail=[&](const QString& text){if(error.isEmpty())error=text;app.quit();};
    QObject::connect(client.get(),&CaptureClient::connectionFailed,&app,fail);
    QObject::connect(client.get(),&CaptureClient::requestRejected,&app,[&](const QString&,const QString& code,const QString& text){fail(code+": "+text);});
    QObject::connect(client.get(),&CaptureClient::previewRejected,&app,[&](const QString& text,bool){fail(text);});
    QObject::connect(client.get(),&CaptureClient::responseReceived,&app,[&](const QString& command,const QJsonObject& value){
        if(command=="capabilities"){
            if(value.value("backend")!="test_fixture"){fail("Not a synthetic service fixture");return;}
            client->request("start",{{"plan",QJsonObject{{"mode","timed"},{"sample_rate_hz",5000},{"pulses_per_rev",2},
                {"output_directory",directory.path()},{"segment_seconds",1},{"duration_seconds",3}}}});
        }
    });
    QObject::connect(client.get(),&CaptureClient::liveFrame,&app,[&](const QJsonObject& value){
        try{
            const auto first=stream_contract::decimal(value.value("global_first_index"));const quint64 count=value.value("record_count").toInt();
            const quint64 gap=first>=priorEnd?first-priorEnd:0;observedGap+=gap;priorEnd=first+count;
            QString why;const bool accepted=frames?model.accept(value,&why):model.begin(value,false,&why);
            if(!accepted){fail(why);return;}++frames;
            delivered.append(QJsonObject{{"first_record",QString::number(first)},{"count",double(count)},
                {"gap_records",QString::number(gap)},{"arrival_ms",clock.elapsed()}});
            if(gap){quint64 nans=0;const auto view=model.view(2,true,false);for(const auto& point:view.voltage[0].points)nans+=!std::isfinite(point.y());maximumNaNs=std::max(maximumNaNs,nans);}
            if(!stalled){stalled=true;QThread::msleep(stallMs);}
        }catch(const std::exception& e){fail(QString::fromUtf8(e.what()));}
    });
    QObject::connect(client.get(),&CaptureClient::jobUpdated,&app,[&](const QJsonObject& value){if(value.value("event")=="session_done"){terminal=value;app.quit();}});
    QTimer::singleShot(12000,&app,[&]{fail("Synthetic backlog probe watchdog expired");});client->connectService();app.exec();
    client->stop();const auto profile=client->transportProfile();const auto process=client->serviceProcessInfo();client.reset();
    quint64 persisted=0;QJsonArray segments;QDirIterator it(directory.path(),{"*.BIN"},QDir::Files,QDirIterator::Subdirectories);QStringList paths;
    while(it.hasNext())paths.append(it.next());paths.sort();
    for(const auto& path:paths){
        try{const auto capture=vibration::load_capture(path.toUtf8().toStdString());if(capture.records.empty()||capture.records.front().index!=persisted||!capture.source_metadata.synthetic){error="Stored synthetic segments lost continuity";break;}persisted+=capture.records.size();QFile file(path);file.open(QIODevice::ReadOnly);segments.append(QJsonObject{{"records",double(capture.records.size())},{"sha256",QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex())}});}
        catch(const std::exception& e){error=QString::fromUtf8(e.what());}
    }
    const auto worker=profile.value("worker").toObject();
    bool passed=error.isEmpty()&&stalled&&frames>0&&persisted==15000&&segments.size()==3&&terminal.value("session_complete").toBool()&&process.value("process_not_running").toBool();
    if(expectation=="continuous")passed&=observedGap==0&&model.info().previewDroppedRecords==0&&worker.value("mailbox_dropped_records").toDouble()==0;
    if(expectation=="overflow")passed&=observedGap>0&&maximumNaNs>0&&worker.value("mailbox_dropped_records").toDouble()>0;
    QJsonObject report{{"passed",passed},{"expectation",expectation},{"synthetic",true},{"scope","Original host stream service and CaptureClient with a deliberate main-thread stall; no renderer, sensor or board access."},
        {"stall_ms",stallMs},{"service_chunk_period_ms",40},{"sample_rate_hz",5000},{"delivered_frames",double(frames)},
        {"observed_gap_records",double(observedGap)},{"model_preview_dropped_records",double(model.info().previewDroppedRecords)},
        {"maximum_visible_nan_markers",double(maximumNaNs)},{"durable_records",double(persisted)},{"segments",segments},{"delivered",delivered},
        {"terminal",terminal},{"transport_profile",profile},{"error",error}};
    const auto bytes=QJsonDocument(report).toJson();QFile out(output);if(!out.open(QIODevice::WriteOnly)||out.write(bytes)!=bytes.size())return 2;
    std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return passed?0:1;
}
