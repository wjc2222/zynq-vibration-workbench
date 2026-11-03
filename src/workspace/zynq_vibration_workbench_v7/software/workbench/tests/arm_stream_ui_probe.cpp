#include "window.hpp"
#include "capture_page.hpp"
#include "capture_client.hpp"
#include "analysis_pane.hpp"
#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include "vibration/core.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QImage>
#include <QElapsedTimer>
#include <QThread>
#include <QSysInfo>
#include <QTimer>
#include <stdexcept>
#include <cstdio>
using namespace vibration;
using namespace vibration::workbench;
namespace {
template<class T>T* control(QObject& root,const char* name){auto* p=dynamic_cast<T*>(root.findChild<QWidget*>(name));if(!p)throw std::runtime_error(name);return p;}
void require(bool ok,const char* error){if(!ok)throw std::runtime_error(error);}
void pump(int milliseconds){QElapsedTimer t;t.start();while(t.elapsed()<milliseconds){QApplication::processEvents();QThread::msleep(1);}}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);qa::configureFonts(app);
    if(argc!=2)return 2;const auto output=QDir::cleanPath(QString::fromUtf8(argv[1]));
    if(!output.startsWith("/tmp/hdmi55-")||QFileInfo::exists(output))return 2;
    QDir().mkpath(output);
    QJsonObject report{{"schema","v55.actual-arm.synthetic-stream-ui.v1"},{"source","software_test_fixture"},
      {"scope","Actual ARM and Qt display; synthetic source and RAM files only. No ADC, sensor, physical finger or SD throughput acceptance."},
      {"architecture",QSysInfo::currentCpuArchitecture()},{"qpa",QGuiApplication::platformName()}};
    bool passed=false;
    try {
        require(QSysInfo::currentCpuArchitecture().startsWith("arm"),"Actual ARM required");
        Window window(nullptr,output);window.resize(1280,720);window.showFullScreen();
        auto* capture=control<CapturePage>(window,"capturePage");
        auto* pane=control<AnalysisPane>(window,"analysisPane");
        auto* wave=control<SingleWaveformView>(window,"singleWaveformView");
        auto* client=capture->findChild<CaptureClient*>();require(client,"CaptureClient missing");
        capture->setServiceCommand("/usr/bin/vibration_stream_service",{"--test-fixture","--fixture-chunk-ms=40"});
        QElapsedTimer clock;clock.start();while(!capture->property("captureAvailable").toBool()&&clock.elapsed()<8000)pump(10);
        require(capture->property("captureAvailable").toBool(),"Fixture capabilities timeout");
        control<QComboBox>(*capture,"captureRate")->setCurrentIndex(0);
        control<QDoubleSpinBox>(*capture,"captureDuration")->setValue(10);
        QDir().mkpath(output+"/records");control<QLineEdit>(*capture,"captureDirectory")->setText(output+"/records");
        qa::interaction::settingAction(window,"openAccelerationSensitivity");
        for(int ch=1;ch<=3;++ch){control<QCheckBox>(window,qPrintable(QString("channelSelect%1").arg(ch)))->setChecked(true);control<QDoubleSpinBox>(window,qPrintable(QString("accelerationSensitivity%1").arg(ch)))->setValue(ch*2.);}
        qa::interaction::tap(control<QPushButton>(window,"applyAccelerationSensitivity"));
        auto* sensitivity=control<QDialog>(window,"accelerationSensitivityDialog");
        qa::interaction::tap(sensitivity->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close));
        quint64 frames=0,sourceErrors=0;QJsonObject terminal;QJsonObject liveProfile;QVector<QImage> shots;QStringList names;
        QObject::connect(capture,&CapturePage::liveFrame,&app,[&](const QJsonObject& frame){++frames;if(!frame.value("synthetic").toBool()||frame.value("hardware_confirmed").toBool()||frame.value("source").toString()!="test_fixture")++sourceErrors;});
        QObject::connect(capture,&CapturePage::captureState,&app,[&](const QJsonObject& state){const auto phase=state.value("state").toString();if(phase=="completed"||phase=="failed"||phase=="cancelled"){terminal=state;liveProfile=pane->performanceProfile();}});
        qa::interaction::condition(window,"normal");
        require(capture->nextRecordLabel()==RecordLabel::Normal,"Operator label selector did not commit synthetic organizer selection");
        qa::interaction::tap(control<QPushButton>(window,"workbenchStart"));
        clock.restart();bool started=false,stalled=false;int stage=0;double maxLoopMs=0;QElapsedTimer interval;interval.start();
        while(clock.elapsed()<45000){
            pump(2);maxLoopMs=std::max(maxLoopMs,double(interval.restart()));
            started|=capture->property("captureActive").toBool();
            const auto elapsed=clock.elapsed();
            if(started&&!stalled&&elapsed>1000){QThread::msleep(165);stalled=true;}
            if(started&&stage==0&&elapsed>2500){window.setView(2);++stage;}
            if(started&&stage==1&&elapsed>3500){shots.push_back(window.grab().toImage());names<<"acceleration";++stage;}
            if(started&&stage==2&&elapsed>5000){window.setView(1);++stage;}
            if(started&&stage==3&&elapsed>6500){shots.push_back(window.grab().toImage());names<<"spectrum";++stage;}
            if(started&&stage==4&&elapsed>7500){window.setView(0);++stage;}
            if(started&&stage==5&&elapsed>8500){shots.push_back(window.grab().toImage());names<<"voltage";++stage;}
            if(started&&!capture->property("captureActive").toBool()&&!window.isAnalyzing())break;
        }
        require(started&&!capture->property("captureActive").toBool()&&!window.isAnalyzing(),"Capture did not finalize within bound");
        require(terminal.value("state").toString()=="completed","Synthetic capture failed");
        const auto path=capture->property("captureLatestSegment").toString();
        require(path.startsWith(output+"/records/"),"Capture escaped isolated RAM directory");
        const auto record=load_capture(path.toStdString());
        report["last_segment_records"]=double(record.records.size());report["terminal"]=terminal;
        report["live_profile_at_terminal"]=liveProfile;report["transport"]=client->transportProfile();
        report["frames"]=double(frames);report["source_errors"]=double(sourceErrors);report["elapsed_ms"]=double(clock.elapsed());
        report["max_event_loop_interval_ms_including_intentional_stall"]=maxLoopMs;
        report["intentional_stall_ms"]=stalled?165:0;report["visited_live_pages"]=stage;
        require(frames>50&&sourceErrors==0&&stage==6,"Live source or navigation coverage missing");
        require(!record.records.empty(),"No durable synthetic data");
        require(terminal.value("durable_records").toString()=="500000","Ten-second record count mismatch");
        for(int i=0;i<shots.size();++i)require(shots[i].save(output+"/"+names[i]+".png"),"Screenshot save failed");
        report["after_capture_profile"]=pane->performanceProfile();
        report["record_check"]="Final synthetic segment loaded with core integrity validation";
        window.close();pump(30);passed=true;
    } catch(const std::exception& e){report["error"]=QString::fromUtf8(e.what());}
    report["passed"]=passed;QFile file(output+"/report.json");if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly))return 3;
    const auto bytes=QJsonDocument(report).toJson();file.write(bytes);file.close();std::puts(QJsonDocument(report).toJson(QJsonDocument::Compact).constData());return passed?0:1;
}
