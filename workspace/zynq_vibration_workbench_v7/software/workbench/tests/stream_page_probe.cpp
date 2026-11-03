#include "capture_page.hpp"
#include "capture_client.hpp"
#include "controls.hpp"
#include "window.hpp"
#include "test_fonts.hpp"
#include <vibration/core.hpp>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QThread>
#include <functional>
#include <cstdio>
#include <stdexcept>
namespace {
QJsonArray checks;
void check(bool ok,const QString& name){checks.append(QJsonObject{{"check",name},{"passed",ok}});if(!ok)throw std::runtime_error(name.toStdString());}
void waitFor(const std::function<bool()>& predicate,const QString& name){QElapsedTimer t;t.start();while(!predicate()&&t.elapsed()<15000){QApplication::processEvents();QThread::msleep(2);}check(predicate(),name);}
template<class T>T* find(QObject& owner,const char* name){auto* result=owner.findChild<T*>(name);check(result,QString("control ")+name);return result;}
void fits(QWidget& page){QApplication::processEvents();for(auto* w:page.findChildren<QWidget*>())if(w->isVisible()&&!w->objectName().isEmpty())check(page.rect().contains(QRect(w->mapTo(&page,QPoint()),w->size())),"bounds "+w->objectName());}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);vibration::workbench::qa::configureFonts(app);app.setStyleSheet(vibration::workbench::stylesheet());
    QJsonObject report{{"schema","stream-capture-page-host-v1"},{"scope","Explicit synthetic service and real Qt widgets; no board or SD access"}};
    try{
        check(argc==4,"service work evidence arguments");check(!QFileInfo::exists("/dev/ax7z020-capture"),"refuses real hardware");QDir work(QString::fromLocal8Bit(argv[2])),out(QString::fromLocal8Bit(argv[3]));check(work.exists()&&out.exists(),"isolated directories");
        for(const auto size:{QSize(1024,600),QSize(1280,720)}){
            const auto tag=QString::number(size.width());check(work.mkdir(tag),"per-size output");
            vibration::workbench::CapturePage page([]{return new vibration::workbench::NumberField;});page.resize(size);page.setServiceCommand(QString::fromLocal8Bit(argv[1]),{"--test-fixture","--fixture-chunk-ms=1"});page.show();
            auto* mode=find<QComboBox>(page,"captureMode");auto* rate=find<QComboBox>(page,"captureRate");auto* duration=find<QDoubleSpinBox>(page,"captureDuration");auto* samples=find<QLineEdit>(page,"captureSamples");auto* start=find<QPushButton>(page,"captureStart");auto* stop=find<QPushButton>(page,"captureCancel");auto* view=find<QPushButton>(page,"captureView");
            find<QLineEdit>(page,"captureDirectory")->setText(work.filePath(tag));waitFor([&]{return start->isEnabled();},"stream fixture connects");check(page.property("captureStreaming").toBool()&&mode->count()==2,"default stream with two modes");check(duration->maximum()==86400,"timed limit 86400 seconds");duration->setValue(86400);check(samples->text()=="4320000000"&&start->isEnabled(),"u64 sample total beyond uint32");samples->setText("50000000");check(duration->value()==1000,"sample total changes integer seconds");samples->setText("8192");check(!start->isEnabled(),"invalid timed count cannot start");
            mode->setCurrentIndex(1);check(!duration->isVisible()&&!samples->isVisible()&&start->isEnabled(),"continuous omits timed fields and ignores hidden invalid count");fits(page);check(page.grab().save(out.filePath("continuous-"+tag+".png")),"render continuous parameters");
            QJsonObject requested,last;int preparing=0,frames=0;QString path;
            QObject::connect(&page,&vibration::workbench::CapturePage::captureState,&page,[&](const QJsonObject& value){last=value;if(value.value("state")=="preparing")++preparing;if(requested.isEmpty())requested=value.value("plan").toObject();});
            QObject::connect(&page,&vibration::workbench::CapturePage::liveFrame,&page,[&](const QJsonObject&){++frames;});
            QObject::connect(&page,&vibration::workbench::CapturePage::captureReady,&page,[&](const QString& value){path=value;});
            start->click();waitFor([&]{return stop->isEnabled();},"manual capture starts");check(requested.value("mode")=="continuous"&&(!requested.contains("duration_seconds")||requested.value("duration_seconds").isNull()),"continuous request has no numeric duration");
            waitFor([&]{return !page.property("captureLatestSegment").toString().isEmpty();},"segment published during active capture");check(page.property("captureActive").toBool()&&!view->isEnabled()&&frames>0,"segments preserve active preview and prohibit mid-session analysis");
            stop->click();check(page.property("captureActive").toBool()&&!start->isEnabled(),"stop keeps controls locked through final save");waitFor([&]{return !page.property("captureActive").toBool();},"manual stop drains final segment");check(last.value("state")=="stopped"&&view->isEnabled(),"stopped final segment accessible");view->click();const auto capture=vibration::load_capture(path.toStdString());check(capture.header.stream_segment&&capture.source_metadata.synthetic,"opened final segment validates original stream format");check(find<QLabel>(page,"captureProgressText")->text().contains(QStringLiteral("已保存")),"received and saved counts shown separately");
            mode->setCurrentIndex(0);duration->setValue(1);rate->setCurrentIndex(rate->findData(5000));check(duration->isVisible()&&samples->isVisible(),"timed fields restored");fits(page);check(page.grab().save(out.filePath("timed-"+tag+".png")),"render timed parameters");start->click();waitFor([&]{return !page.property("captureActive").toBool();},"timed session completes naturally");check(last.value("state")=="completed"&&last.value("analysis_scope")=="segment","completed analysis scope stays single segment");
            const auto saved=page.property("captureLatestSegment").toString();check(!saved.isEmpty(),"final timed segment retained");page.setServiceCommand(QString::fromLocal8Bit(argv[1]),{"--test-fixture","--fixture-chunk-ms=0","--fixture-fault=storage_after_first"});waitFor([&]{return start->isEnabled();},"storage failure fixture connects");mode->setCurrentIndex(1);start->click();waitFor([&]{return !page.property("captureActive").toBool();},"storage failure terminates");check(last.value("state")=="failed"&&!page.property("captureLatestSegment").toString().isEmpty()&&page.property("captureLatestSegment").toString()!=saved&&view->isEnabled(),"storage failure preserves newly published segments");
        }
        check(work.mkdir("close"),"close scenario output");vibration::workbench::Window window;window.resize(1024,600);auto* page=find<vibration::workbench::CapturePage>(window,"capturePage");page->setServiceCommand(QString::fromLocal8Bit(argv[1]),{"--test-fixture","--fixture-chunk-ms=2"});find<QLineEdit>(*page,"captureDirectory")->setText(work.filePath("close"));find<QComboBox>(*page,"captureMode")->setCurrentIndex(1);window.show();window.showAcquisition();auto* start=find<QPushButton>(*page,"captureStart");waitFor([&]{return start->isEnabled();},"close scenario ready");start->click();waitFor([&]{return find<QPushButton>(*page,"captureCancel")->isEnabled();},"close scenario active");window.close();check(window.isVisible(),"window close defers during capture");waitFor([&]{return !window.isVisible();},"window closes after graceful stop and save");check(!page->property("captureLatestSegment").toString().isEmpty(),"close retains final segment");report["result"]="passed";
    }catch(const std::exception& e){report["result"]="failed";report["error"]=QString::fromUtf8(e.what());}
    report["checks"]=checks;const auto json=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(json.constData(),1,std::size_t(json.size()),stdout);return report.value("result")=="passed"?0:1;
}
