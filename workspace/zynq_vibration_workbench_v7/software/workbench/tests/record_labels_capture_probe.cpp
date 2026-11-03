#include "capture_page.hpp"
#include "capture_client.hpp"
#include "record_labels.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace vibration::workbench;
namespace {
QJsonArray checks;
void check(bool passed,const QString& text){checks.append(QJsonObject{{"check",text},{"passed",passed}});if(!passed)throw std::runtime_error(text.toStdString());}
void waitFor(const std::function<bool()>& predicate,const QString& text){QElapsedTimer timer;timer.start();while(!predicate()&&timer.elapsed()<15000){QApplication::processEvents();QThread::msleep(2);}check(predicate(),text);}
template<class T>T* find(QObject& object,const char* name){auto* child=object.findChild<T*>(name);check(child,QString("control ")+name);return child;}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);QJsonObject report{{"schema","v7-capture-label-session-qa-v1"},{"scope","Explicit synthetic host stream service and real CapturePage; no board, sensor or SD acceptance."}};
    try{
        check(argc==2,"explicit host synthetic service argument");check(!QFileInfo::exists("/dev/ax7z020-capture"),"refuse physical capture device");QTemporaryDir work;check(work.isValid(),"temporary data root");
        CapturePage page({},nullptr,work.path());page.setServiceCommand(QString::fromLocal8Bit(argv[1]),{"--test-fixture","--fixture-chunk-ms=1"});
        find<QLineEdit>(page,"captureDirectory")->setText(work.path());auto* rate=find<QComboBox>(page,"captureRate");rate->setCurrentIndex(rate->findData(5000));auto* mode=find<QComboBox>(page,"captureMode");mode->setCurrentIndex(1);
        page.show();auto* start=find<QPushButton>(page,"captureStart"),*stop=find<QPushButton>(page,"captureCancel");waitFor([&]{return start->isEnabled();},"synthetic service ready");
        QStringList paths;QSet<QString> sessions;QStringList errors;QString currentJob;bool annotationValid=true,leaked=false;RecordLabel expected=RecordLabel::Normal;
        QObject::connect(&page,&CapturePage::recordLabelSaveFailed,&page,[&](const QString&,const QString& message){errors.append(message);});
        QObject::connect(&page,&CapturePage::committedSegmentReady,&page,[&](const QString& path){RecordLabelAnnotation label;QString error;annotationValid=annotationValid&&loadRecordLabel(path,&label,&error)&&label.label==expected&&label.synthetic&&!label.sessionId.isEmpty();paths.append(path);sessions.insert(label.sessionId);});
        QObject::connect(&page,&CapturePage::liveFrame,&page,[&](const QJsonObject& value){leaked=leaked||value.contains("label")||value.contains("operator_label");});
        QObject::connect(&page,&CapturePage::captureState,&page,[&](const QJsonObject& value){currentJob=value.value("job_id").toString();const auto plan=value.value("plan").toObject();leaked=leaked||plan.contains("label")||plan.contains("operator_label");});
        check(page.nextRecordLabel()==RecordLabel::Unlabeled,"default next session is unlabelled");check(page.setNextRecordLabel(RecordLabel::Normal),"operator selects normal");start->click();
        check(page.activeRecordLabel()==RecordLabel::Normal&&!page.setNextRecordLabel(RecordLabel::Imbalance)&&page.nextRecordLabel()==RecordLabel::Normal,"start freezes label and rejects active changes");
        waitFor([&]{return paths.size()>=2;},"continuous session commits several labelled segments");stop->click();waitFor([&]{return !page.property("captureActive").toBool();},"stop drains and annotates final segment");
        check(annotationValid&&paths.size()>=2&&sessions.size()==1&&errors.isEmpty(),"every continuous segment has frozen normal and one session identity");
        check(!leaked,"operator label absent from capture protocol and live/model frame inputs");
        check(page.nextRecordLabel()==RecordLabel::Normal,"next selection retained after stop");
        check(page.setNextRecordLabel(RecordLabel::Imbalance),"operator changes next capture to imbalance");expected=RecordLabel::Imbalance;const auto firstSessions=sessions;paths.clear();sessions.clear();mode->setCurrentIndex(0);find<QDoubleSpinBox>(page,"captureDuration")->setValue(1);start->click();waitFor([&]{return !page.property("captureActive").toBool();},"next timed session finishes");
        check(annotationValid&&!paths.isEmpty()&&sessions.size()==1&&!firstSessions.contains(*sessions.begin())&&errors.isEmpty(),"next session receives new label without changing previous session");
        // With no selection the core CapturePage remains backward compatible;
        // the shell may require a conscious selection before invoking start.
        check(page.setNextRecordLabel(RecordLabel::Unlabeled),"clear next selection");
        QObject::disconnect(&page,&CapturePage::committedSegmentReady,&page,nullptr);paths.clear();
        QObject::connect(&page,&CapturePage::committedSegmentReady,&page,[&](const QString& path){paths.append(path);});start->click();waitFor([&]{return !page.property("captureActive").toBool();},"unlabelled CapturePage session finishes");
        check(!paths.isEmpty()&&!QFileInfo::exists(recordLabelSidecarPath(paths.last())),"unselected capture does not invent a normal/fault label");
        // Inject only an isolated committed-event notification for a genuine
        // previously generated synthetic BIN whose label path is occupied.
        // This exercises the UI failure signal without a real disk failure.
        const auto faultDirectory=work.filePath("label-save-fault"),faultPath=faultDirectory+"/data.BIN";
        check(QDir().mkdir(faultDirectory)&&QFile::copy(paths.last(),faultPath)&&QDir().mkdir(recordLabelSidecarPath(faultPath)),"isolated annotation failure fixture");
        check(page.setNextRecordLabel(RecordLabel::Normal),"select label for visible-save-failure test");mode->setCurrentIndex(1);start->click();waitFor([&]{return stop->isEnabled()&&!currentJob.isEmpty();},"failure-test session starts");
        auto* client=page.findChild<CaptureClient*>();check(client,"capture client for isolated event injection");
        client->jobUpdated(QJsonObject{{"job_id",currentJob},{"event","segment_ready"},{"path",faultPath}});
        check(!errors.isEmpty()&&page.property("captureLabelFailedPath").toString()==faultPath&&!find<QLabel>(page,"captureError")->text().isEmpty(),"annotation failure has visible UI message and explicit signal");
        check(page.property("captureActive").toBool(),"label-save failure preserves ongoing raw acquisition");stop->click();waitFor([&]{return !page.property("captureActive").toBool();},"failure-test capture stops and saves");
        check(QFileInfo::exists(faultPath),"annotation failure preserves saved raw data");report["passed"]=true;
    }catch(const std::exception& e){report["passed"]=false;report["error"]=QString::fromUtf8(e.what());}
    report["checks"]=checks;auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return report.value("passed").toBool()?0:1;
}
