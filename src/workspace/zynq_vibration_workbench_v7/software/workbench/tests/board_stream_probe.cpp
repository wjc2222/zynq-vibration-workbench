// Actual ARM framebuffer + PL/DMA + SD runtime QA. There is no fixture mode.
#include "window.hpp"
#include "capture_page.hpp"
#include "capture_client.hpp"
#include "analysis_pane.hpp"
#include "single_waveform_view.hpp"
#include "stream_preview_contract.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTextCodec>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace vibration;
using namespace vibration::workbench;
namespace {
void require(bool ok,const QString& why){if(!ok)throw std::runtime_error(why.toStdString());}
template<class T>T* find(QObject& root,const char* name){auto* p=dynamic_cast<T*>(root.findChild<QObject*>(name));require(p,QString("Missing actual widget: ")+name);return p;}
QString decimal(quint64 n){return QString::number(n);}
bool waitFor(const std::function<bool()>& predicate,int timeout){
    if(predicate())return true;QEventLoop loop;QTimer timer,deadline;timer.setInterval(10);deadline.setSingleShot(true);
    QObject::connect(&timer,&QTimer::timeout,&loop,[&]{if(predicate())loop.quit();});QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    timer.start();deadline.start(timeout);loop.exec();return predicate();
}
QString sha256(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"Cannot hash "+path);QCryptographicHash h(QCryptographicHash::Sha256);require(h.addData(&file),"Cannot read hash input");return QString::fromLatin1(h.result().toHex());}
#ifdef Q_OS_UNIX
int openDirectory(const QString& path){
    require(QDir::isAbsolutePath(path)&&QDir::cleanPath(path)==path,"Directory must be canonical and absolute");int fd=::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);require(fd>=0,"Cannot open root directory");
    for(const auto& part:path.split('/',Qt::SkipEmptyParts)){const auto bytes=QFile::encodeName(part);const auto next=::openat(fd,bytes.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);::close(fd);require(next>=0,"Missing or linked directory component: "+path);fd=next;}return fd;
}
void makeExclusiveDirectory(const QString& path){
    const QFileInfo p(path);const auto fd=openDirectory(p.absolutePath());const auto bytes=QFile::encodeName(p.fileName());const auto result=::mkdirat(fd,bytes.constData(),0700);::close(fd);require(result==0,"QA directory must be new; refusing replacement: "+path);
}
#endif
QJsonObject inspectSegments(const QString& directory,const QString& job,quint64 expected){
    QDirIterator it(directory,{"data.BIN"},QDir::Files,QDirIterator::Subdirectories);QStringList files;while(it.hasNext())files.append(it.next());files.sort();
    require(files.size()>=2,"Capture must publish at least two complete SD segments");QJsonArray segments;quint64 next=0,lastTick=0;
    for(const auto& path:files){
        const auto capture=load_capture(path.toUtf8().toStdString());const auto& h=capture.header;
        require(capture.integrity_verified&&h.stream_segment&&h.stream_flags==1&&capture.source_metadata.synthetic,"Saved segment format/integrity/synthetic declaration mismatch");
        require(h.sample_rate_hz==50000&&h.pulses_per_rev==1&&h.global_first_index==next&&capture.records.front().index==next,"Saved segment global index/rate/PPR mismatch");
        require(!next||h.first_tick==lastTick+1000,"Adjacent segment raw ticks must stay continuous");
        require(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(h.session_id.data()),16).toHex())==job,"Saved session UUID differs from live job");
        const auto metaPath=QFileInfo(path).absolutePath()+"/data.meta.json";QFile meta(metaPath);require(meta.open(QIODevice::ReadOnly),"Missing SD metadata pair");const auto metadata=QJsonDocument::fromJson(meta.readAll()).object();
        require(metadata.value("source")=="hardware_test"&&metadata.value("synthetic").toBool()&&metadata.value("hardware_confirmed").toBool()&&metadata.value("session_id")==job,"Metadata is not a real PL hardware-test source");
        next+=h.completed_records;lastTick=h.last_tick;
        segments.append(QJsonObject{{"path",path},{"meta_path",metaPath},{"global_first_index",decimal(h.global_first_index)},{"records",int(h.completed_records)},{"first_tick",decimal(h.first_tick)},{"last_tick",decimal(h.last_tick)},{"bin_sha256",sha256(path)},{"meta_sha256",sha256(metaPath)}});
    }
    require(next==expected,"Durable terminal count differs from completely verified SD files");return {{"verified_records",decimal(next)},{"segment_count",files.size()},{"segments",segments}};
}
}
int main(int argc,char** argv){
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
    QJsonObject report{{"schema","vibration.stream.actual-arm-qpa-qa.v1"},{"source","hardware_test"},{"synthetic",true},{"host_fixture",false},{"fixture_fallback",false},
        {"physical_sensor_measurement",false},{"physical_display_presence_verified",false},{"physical_touch_verified",false},{"performance_threshold_prejudged",false},
        {"scope","Actual ARM QPA rendering, real PL test pattern through DMA, and SD files. QPA pixels do not prove an attached physical display, touch operation, sensors, calibration, or fault accuracy."}};
    QString evidence,captureDirectory,asyncError,job;CaptureClient* client=nullptr;std::unique_ptr<Window> window;bool outputCreated=false,passed=false;
    quint64 frames=0,previewGaps=0,previewEnd=0,maxAccepted=0,maxRendered=0,maxRetained=0;bool sawPlotData=false;
    QJsonObject capabilities,terminal;QJsonArray segmentEvents,previewErrors,shots;QElapsedTimer clock;clock.start();qint64 firstNs=0,lastNs=0;
    struct DeferredShot{QPixmap pixels;QJsonObject entry;};
    constexpr std::size_t maximumShots=6;
    constexpr quint64 maximumShotPixelBytes=24*1024*1024;
    std::vector<DeferredShot> deferredShots;deferredShots.reserve(maximumShots);quint64 shotPixelBytes=0;
    report["screenshot_policy"]=QJsonObject{{"capture","bounded GUI-thread QPixmap grab; existing synchronous UI assertions retained"},{"encoding","PNG encoding, file write and SHA256 deferred until capture has terminated and service client has stopped"},{"maximum_snapshots",int(maximumShots)},{"maximum_pixel_payload_bytes",decimal(maximumShotPixelBytes)}};
    try{
        require(argc>=3&&argc<=5,"Usage: board_stream_probe ABS_SERVICE /tmp/NEW_EVIDENCE_DIR [continuous|timed] [seconds]");
        const auto service=QDir::cleanPath(QString::fromUtf8(argv[1]));evidence=QDir::cleanPath(QString::fromUtf8(argv[2]));const QString mode=argc>=4?QString::fromUtf8(argv[3]):"continuous";
        bool secondsOk=true;const int seconds=argc>=5?QString::fromUtf8(argv[4]).toInt(&secondsOk):(mode=="timed"?12:15);
        require((mode=="continuous"||mode=="timed")&&secondsOk&&seconds>=12&&seconds<=60,"Use timed 12..60 s or continuous stop after 12..60 s");
        require(evidence.startsWith("/tmp/")&&QDir::isAbsolutePath(evidence),"Evidence must use a new /tmp directory");
#ifdef Q_OS_UNIX
        makeExclusiveDirectory(evidence);outputCreated=true;
#else
        require(false,"Actual ARM Linux runtime required");
#endif
        report["cpu_architecture"]=QSysInfo::currentCpuArchitecture();report["qpa_backend"]=QGuiApplication::platformName();report["mode"]=mode;report["requested_seconds"]=seconds;report["evidence_directory"]=evidence;
        require(QSysInfo::currentCpuArchitecture().startsWith("arm"),"Refusing host execution: actual ARM runtime required");
        require(QStringList{"ax7zfb","linuxfb"}.contains(QGuiApplication::platformName()),"Actual framebuffer QPA required; offscreen/minimal/desktop backends are refused");
        require(QDir::isAbsolutePath(service)&&QFileInfo(service).isExecutable(),"Use the real absolute stream-service executable");
#ifdef Q_OS_UNIX
        struct stat device{};require(::stat("/dev/ax7z020-capture",&device)==0&&S_ISCHR(device.st_mode),"Actual capture character device is required");
        const QString root="/mnt/capture-data/vibration";const auto rootFd=openDirectory(root);::close(rootFd);
        const QStorageInfo storage(root);const auto storageDevice=QString::fromLocal8Bit(storage.device());
        require(storage.isValid()&&storage.isReady()&&!storage.isReadOnly()&&QString::fromLocal8Bit(storage.fileSystemType())=="vfat"&&QRegularExpression("^/dev/mmcblk[0-9]+p[0-9]+$").match(storageDevice).hasMatch(),"Writable actual SD VFAT mount required; no RAM fallback");
        report["storage"]=QJsonObject{{"root",root},{"device",storageDevice},{"filesystem",QString::fromLocal8Bit(storage.fileSystemType())},{"mount",storage.rootPath()},{"bytes_available_before",decimal(quint64(storage.bytesAvailable()))}};
        const auto name="QA_HDMI_"+QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ")+'_'+QString::number(QCoreApplication::applicationPid())+'_'+QUuid::createUuid().toString(QUuid::Id128).left(8);
        captureDirectory=root+'/'+name;makeExclusiveDirectory(captureDirectory);report["capture_directory"]=captureDirectory;
#endif
        qputenv("AX7_CAPTURE_WORKER","1");qputenv("AX7_CAPTURE_PROFILE","1");qputenv("AX7_LIVE_DEMAND_ANALYSIS","1");report["runtime_font_files_loaded"]=QJsonArray::fromStringList(qa::configureFonts(app));report["font_audit"]=qa::fontAudit(app.font());
        report["probe_sha256"]=sha256(QCoreApplication::applicationFilePath());report["service_sha256"]=sha256(service);
        window=std::make_unique<Window>();window->resize(1280,720);auto* page=find<CapturePage>(*window,"capturePage");auto* analysis=find<AnalysisPane>(*window,"analysisPane");auto* wave=find<SingleWaveformView>(*window,"singleWaveformView");client=page->findChild<CaptureClient*>();require(client,"Actual window capture client missing");
        QObject::connect(client,&CaptureClient::connectionFailed,window.get(),[&](const QString& why){asyncError=why;});
        QObject::connect(client,&CaptureClient::requestRejected,window.get(),[&](const QString&,const QString& code,const QString& why){asyncError=code+": "+why;});
        QObject::connect(client,&CaptureClient::previewRejected,window.get(),[&](const QString& why,bool quarantined){previewErrors.append(QJsonObject{{"reason",why},{"quarantined",quarantined}});if(quarantined)asyncError=why;});
        QObject::connect(client,&CaptureClient::responseReceived,window.get(),[&](const QString& command,const QJsonObject& data){if(command=="capabilities")capabilities=data;});
        QObject::connect(client,&CaptureClient::jobUpdated,window.get(),[&](const QJsonObject& data){if(data.value("event")=="segment_ready")segmentEvents.append(data);if(data.value("event")=="session_done")terminal=data;});
        QObject::connect(page,&CapturePage::liveFrame,window.get(),[&](const QJsonObject& frame){try{
            const auto p=stream_contract::decode(frame);require(p.source=="hardware_test"&&p.hardware&&p.synthetic&&p.rate==50000&&p.ppr==1,"Live frame is not the requested real PL/DMA hardware test");
            if(job.isEmpty())job=p.job;require(job==p.job,"Live session identity changed");if(!frames)firstNs=clock.nsecsElapsed();lastNs=clock.nsecsElapsed();if(p.first>=previewEnd)previewGaps+=p.first-previewEnd;previewEnd=p.first+p.count;++frames;
        }catch(const std::exception& e){asyncError=QString::fromUtf8(e.what());}});
        QTimer modelSampler;modelSampler.setInterval(100);QObject::connect(&modelSampler,&QTimer::timeout,window.get(),[&]{
            maxAccepted=std::max(maxAccepted,analysis->property("liveAcceptedFrames").toULongLong());maxRendered=std::max(maxRendered,analysis->property("liveRenderedFrames").toULongLong());maxRetained=std::max(maxRetained,analysis->property("liveRetainedRecords").toULongLong());
            if(analysis->isLive()&&wave->isVisible()&&!wave->sourceSeries().isEmpty())for(const auto& point:wave->sourceSeries().first().points)if(std::isfinite(point.y())){sawPlotData=true;break;}
        });modelSampler.start();
        const auto shot=[&](const QString& name){
            QApplication::processEvents();QJsonObject ui{{"live",analysis->isLive()},{"capture_state",page->property("captureStatus").toString()}};
            if(analysis->isLive()&&wave->plot()->explicitXRange()&&wave->sourceSeries().size()==3){
                const auto range=*wave->plot()->explicitXRange();auto* note=find<QLabel>(*analysis,"plotNavigationNote");
                const auto expected=QStringLiteral("实时跟随 · %1–%2 s").arg(QString::number(range.x(),'g',5),QString::number(range.y(),'g',5));
                ui["range_start_s"]=range.x();ui["range_end_s"]=range.y();ui["range_label"]=note->text();ui["range_label_matches_axis"]=note->text()==expected;
                require(note->text()==expected,"Live follow label is stale relative to the actual plot axis");QJsonArray amplitudes;
                for(int ch=0;ch<3;++ch){long double square=0;double low=std::numeric_limits<double>::infinity(),high=-low;quint64 count=0;
                    for(const auto& p:wave->sourceSeries()[ch].points)if(std::isfinite(p.x())&&std::isfinite(p.y())&&p.x()>=range.x()&&p.x()<=range.y()){square+=static_cast<long double>(p.y())*p.y();low=std::min(low,p.y());high=std::max(high,p.y());++count;}
                    auto* rms=find<QLabel>(*analysis,qPrintable(QString("singleRms%1").arg(ch+1)));auto* peak=find<QLabel>(*analysis,qPrintable(QString("singlePeak%1").arg(ch+1)));
                    const double expectedRms=count?std::sqrt(double(square/count)):std::numeric_limits<double>::quiet_NaN(),expectedPeak=count?high-low:std::numeric_limits<double>::quiet_NaN();
                    const bool matches=count&&rms->text()==QString::number(expectedRms,'g',3)&&peak->text()==QString::number(expectedPeak,'g',3);
                    amplitudes.append(QJsonObject{{"channel",ch+1},{"raw_window_points",decimal(count)},{"expected_rms_v",expectedRms},{"expected_peak_to_peak_v",expectedPeak},{"display_rms",rms->text()},{"display_peak_to_peak",peak->text()},{"scope",rms->toolTip()},{"matches_current_raw_window",matches}});
                    require(matches,"Live RMS/peak-to-peak does not match the actual current raw waveform window");
                }
                ui["amplitude_audit"]=amplitudes;
            }
            require(deferredShots.size()<maximumShots,"Bounded screenshot count exceeded");
            const auto before=clock.nsecsElapsed();auto pixels=window->grab();const auto grabNs=clock.nsecsElapsed()-before;
            require(!pixels.isNull(),"Cannot grab actual window screenshot");
            const auto pixelBytes=quint64(pixels.width())*quint64(pixels.height())*4;
            require(pixels.depth()<=32&&pixelBytes<=maximumShotPixelBytes-shotPixelBytes,"Bounded screenshot pixel payload exceeded");shotPixelBytes+=pixelBytes;
            const auto path=evidence+'/'+name+".png";
            deferredShots.push_back({std::move(pixels),QJsonObject{{"path",path},{"captured_at_ns",decimal(quint64(before))},{"grab_ms",grabNs/1e6},{"png_deferred",true},{"pixel_payload_upper_bound_bytes",decimal(pixelBytes)},{"ui_audit",ui}}});
        };
        page->setStreaming(true);page->setServiceCommand(service,{"--hardware-test"});window->showFullScreen();window->showAcquisition();
        require(waitFor([&]{return page->property("captureAvailable").toBool()||!asyncError.isEmpty()||!page->property("captureDiagnostic").toString().isEmpty();},10000),"Hardware capability response timed out");
        require(asyncError.isEmpty()&&page->property("captureAvailable").toBool(),"Hardware capability unavailable: "+asyncError+page->property("captureDiagnostic").toString());
        require(capabilities.value("backend")=="linux_ring"&&capabilities.value("source")=="hardware_test"&&capabilities.value("synthetic").toBool()&&capabilities.value("hardware_confirmed").toBool(),"Capabilities are not the explicit real hardware-test backend");report["capabilities"]=capabilities;
        auto* selection=find<QComboBox>(*page,"captureMode");auto* rate=find<QComboBox>(*page,"captureRate");auto* duration=find<QDoubleSpinBox>(*page,"captureDuration");auto* samples=find<QLineEdit>(*page,"captureSamples");
        find<QLineEdit>(*page,"captureDirectory")->setText(captureDirectory);rate->setCurrentIndex(rate->findData(50000));find<QDoubleSpinBox>(*page,"capturePpr")->setValue(1);
        selection->setCurrentIndex(selection->findData("timed"));duration->setValue(mode=="timed"?seconds:12);require(samples->text()==QString::number(50000*(mode=="timed"?seconds:12)),"Timed sample-count linkage mismatch");shot("parameters-timed");
        selection->setCurrentIndex(selection->findData("continuous"));require(!duration->isVisible()&&!samples->isVisible(),"Continuous mode must omit timed controls");shot("parameters-continuous");
        selection->setCurrentIndex(selection->findData(mode));if(mode=="timed")duration->setValue(seconds);
        const auto process=client->serviceProcessInfo();const auto pid=qint64(process.value("pid").toDouble());report["service_process_info"]=process;
        require(process.value("streaming").toBool()&&process.value("worker_enabled").toBool()&&process.value("running").toBool()&&process.value("arguments").toArray()==QJsonArray{"--hardware-test"}&&process.value("canonical_program")==QFileInfo(service).canonicalFilePath(),"Child command is not the required actual service --hardware-test");
        require(pid>0&&QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget()==QFileInfo(service).canonicalFilePath(),"Running child executable differs from the hashed service");
        auto* start=find<QPushButton>(*page,"captureStart");require(start->isEnabled(),"Actual Start button unavailable");start->click();
        require(waitFor([&]{return (frames>0&&maxAccepted>0&&maxRendered>0&&sawPlotData)||!asyncError.isEmpty()||!terminal.isEmpty();},12000)&&asyncError.isEmpty()&&frames>0&&maxAccepted>0&&maxRendered>0&&sawPlotData,"Real raw frames did not reach the live model and rendered waveform: "+asyncError);
        require(terminal.isEmpty(),"Capture ended before the requested cross-segment run");window->setView(0);shot("live-time-domain-early");
        require(waitFor([&]{return ((!segmentEvents.isEmpty())&&clock.nsecsElapsed()-firstNs>=10500000000LL)||!asyncError.isEmpty()||!terminal.isEmpty();},18000)&&asyncError.isEmpty()&&!segmentEvents.isEmpty(),"No complete 10-second SD segment while live rendering");
        require(terminal.isEmpty(),"Capture stopped before the across-segment screenshot");shot("live-time-domain-cross-segment");
        if(mode=="continuous"){
            require(waitFor([&]{return clock.nsecsElapsed()-firstNs>=qint64(seconds)*1000000000LL||!asyncError.isEmpty()||!terminal.isEmpty();},seconds*1000+5000)&&asyncError.isEmpty()&&terminal.isEmpty(),"Continuous source ended before manual stop");
            auto* stop=find<QPushButton>(*window,"workbenchStart");require(stop->isVisible()&&stop->isEnabled(),"Real Window Stop action unavailable");report["stop_action_ns"]=decimal(quint64(clock.nsecsElapsed()));stop->click();shot("stopping-tail-save");
        }
        require(waitFor([&]{return !terminal.isEmpty()||!asyncError.isEmpty();},45000)&&asyncError.isEmpty(),"No final saved session status: "+asyncError);
        require(terminal.value("state")==QJsonValue(mode=="timed"?"completed":"stopped")&&terminal.value("session_complete").toBool()&&terminal.value("source")=="hardware_test"&&terminal.value("synthetic").toBool()&&terminal.value("hardware_confirmed").toBool(),"Session failed or lost hardware-test provenance");
        const auto durable=stream_contract::decimal(terminal.value("durable_records"));require(durable==stream_contract::decimal(terminal.value("records_received"))&&durable>500000,"Session did not save its received cross-segment tail");
        if(mode=="timed")require(durable==quint64(seconds)*50000&&terminal.value("target_complete").toBool(),"Timed total mismatch");
        require(waitFor([&]{return !window->isAnalyzing();},60000),"Final segment analysis did not return");window->setView(0);shot("stopped-final-segment");
        modelSampler.stop();report["analysis_profile"]=analysis->performanceProfile();report["transport_profile"]=client->transportProfile();report["terminal"]=terminal;report["segment_events"]=segmentEvents;
        report["saved_data"]=inspectSegments(captureDirectory,job,durable);report["live_model"]=QJsonObject{{"validated_frames",decimal(frames)},{"max_model_accepted_frames",decimal(maxAccepted)},{"max_rendered_frames",decimal(maxRendered)},{"max_retained_records",decimal(maxRetained)},{"saw_actual_waveform_points",sawPlotData},{"preview_gap_records",decimal(previewGaps)},
            {"observed_delivery_hz",frames>1&&lastNs>firstNs?double(frames-1)*1e9/(lastNs-firstNs):0},{"first_frame_ns",decimal(quint64(firstNs))},{"last_frame_ns",decimal(quint64(lastNs))}};
        require(maxRetained<=100000,"Live model exceeded its two-second bounded ring");window->close();require(waitFor([&]{return !window->isVisible();},5000),"Window did not close normally after capture");client->stop();report["service_after_close"]=client->serviceProcessInfo();
        require(client->serviceProcessInfo().value("session_done_observed").toBool()&&client->serviceProcessInfo().value("process_not_running").toBool()&&!QFileInfo::exists(QString("/proc/%1").arg(pid)),"Child was not gracefully joined after final SD publication");
        passed=true;
    }catch(const std::exception& e){report["error"]=QString::fromUtf8(e.what());if(client){client->stop();report["service_after_failure"]=client->serviceProcessInfo();}}
    report["peak_deferred_screenshot_pixel_bytes"]=decimal(shotPixelBytes);
    // Keep the pixels from each original capture instant. Encode only after
    // the live source has stopped, including the cleanup path after a failure.
    for(auto& snapshot:deferredShots){
        auto entry=std::move(snapshot.entry);const auto path=entry.value("path").toString();const auto before=clock.nsecsElapsed();entry["png_started_at_ns"]=decimal(quint64(before));
        try{
            require(snapshot.pixels.save(path),"Cannot save deferred actual window screenshot: "+path);
            const double encodeMs=(clock.nsecsElapsed()-before)/1e6;entry["png_encode_and_write_ms"]=encodeMs;entry["capture_and_png_ms"]=entry.value("grab_ms").toDouble()+encodeMs;
            const auto hashStarted=clock.nsecsElapsed();entry["sha256"]=sha256(path);entry["sha256_ms"]=(clock.nsecsElapsed()-hashStarted)/1e6;entry["saved"]=true;
        }catch(const std::exception& e){const auto error=QString::fromUtf8(e.what());entry["saved"]=false;entry["error"]=error;if(!report.contains("error"))report["error"]=error;passed=false;}
        shots.append(entry);snapshot.pixels=QPixmap();
    }
    report["passed"]=passed;report["screenshots"]=shots;report["preview_errors"]=previewErrors;report["sd_data_retained"]=!captureDirectory.isEmpty()&&QFileInfo(captureDirectory).isDir();report["elapsed_ms"]=clock.elapsed();
    window.reset();const auto json=QJsonDocument(report).toJson(QJsonDocument::Indented);
    if(outputCreated){QFile out(evidence+"/board-stream-report.json");if(!out.open(QIODevice::WriteOnly|QIODevice::NewOnly)||out.write(json)!=json.size()){std::fprintf(stderr,"Cannot save final QA report\n");passed=false;}}
    std::fwrite(json.constData(),1,std::size_t(json.size()),stdout);return passed?0:1;
}
