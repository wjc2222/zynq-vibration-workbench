#include "window.hpp"
#include "capture_page.hpp"
#include "capture_client.hpp"
#include "analysis_pane.hpp"
#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include <vibration/core.hpp>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
using namespace vibration;
using namespace vibration::workbench;
namespace {
QJsonArray checks;
void check(bool okay,const QString& name){checks.append(QJsonObject{{"check",name},{"passed",okay}});if(!okay)throw std::runtime_error(name.toStdString());}
template<class T>T* child(QObject& owner,const char* name){auto* value=dynamic_cast<T*>(owner.findChild<QObject*>(QString::fromLatin1(name)));check(value,QStringLiteral("control ")+name);return value;}
void events(int milliseconds=30){QElapsedTimer clock;clock.start();do{QApplication::processEvents();QThread::msleep(2);}while(clock.elapsed()<milliseconds);}
void waitFor(const std::function<bool()>& ready,const QString& name){QElapsedTimer clock;clock.start();while(!ready()&&clock.elapsed()<15000)events(2);check(ready(),name);}
QByteArray hash(const QString& path){QFile file(path);check(file.open(QIODevice::ReadOnly),"hash readable "+path);QCryptographicHash sha(QCryptographicHash::Sha256);check(sha.addData(&file),"hash consumed "+path);return sha.result().toHex();}
void shot(QWidget& widget,const QDir& output,const QString& name){events();check(widget.grab().save(output.filePath(name+".png")),"screenshot "+name);}
QRect inWindow(QWidget& control,QWidget& window){return QRect(control.mapTo(&window,QPoint()),control.size());}
std::array<QPushButton*,3> actions(Window& window){return {child<QPushButton>(window,"workbenchStart"),child<QPushButton>(window,"workbenchDataMenu"),child<QPushButton>(window,"workbenchOptions")};}
void actionGeometry(Window& window,const QString& phase){const auto buttons=actions(window);int lastRight=-1;const int y=inWindow(*buttons[0],window).y();for(auto* button:buttons){const auto rect=inWindow(*button,window);check(button->isVisible()&&window.rect().contains(rect)&&rect.y()==y&&rect.left()>lastRight&&rect.height()>=44,phase+" compact main actions fit and remain visible: "+button->objectName());lastRight=rect.right();check(button->fontMetrics().horizontalAdvance(button->text())+28<=rect.width(),phase+" action text fits: "+button->objectName());}}
void sampleKeypad(Window& window,CapturePage& page,const QDir& output,const QString& tag){
    auto* samples=child<QLineEdit>(page,"captureSamples");QString error;bool inspected=false;QTimer watchdog;watchdog.setSingleShot(true);
    QObject::connect(&watchdog,&QTimer::timeout,&window,[&]{error="number pad timeout";if(auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget()))modal->reject();});watchdog.start(5000);
    QTimer::singleShot(0,&window,[&]{auto* pad=qobject_cast<QDialog*>(QApplication::activeModalWidget());try{
        check(pad&&pad->objectName()=="workbenchNumberPad","touch count field opens original numeric keypad");
        auto* entry=child<QLineEdit>(*pad,"numberPadEntry");QPushButton* apply=nullptr;for(auto* button:pad->findChildren<QPushButton*>())if(button->text()==QStringLiteral("确定"))apply=button;check(apply,"keypad confirm action");
        const auto old=samples->text();entry->setText("8192");apply->click();check(pad->isVisible()&&samples->text()==old,"invalid count remains in keypad without changing plan");
        check(pad->height()<=window.height()&&pad->width()<=window.width(),"keypad fits display");shot(*pad,output,"number-pad-invalid-"+tag);
        entry->setText("5000");apply->click();inspected=true;
    }catch(const std::exception& exception){error=QString::fromUtf8(exception.what());if(pad)pad->reject();}});
    QMouseEvent press(QEvent::MouseButtonPress,QPointF(12,12),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(samples,&press);watchdog.stop();
    check(inspected&&error.isEmpty(),"numeric keypad completed: "+error);check(samples->text()=="5000"&&child<QDoubleSpinBox>(page,"captureDuration")->value()==1,"keypad point count retains integer duration synchronization");
}
void copyFixture(const QString& fixture,const QString& destination){check(QFile::copy(fixture,destination),"copy isolated synthetic library record");const QFileInfo source(fixture),target(destination);const auto meta=source.absolutePath()+"/"+source.completeBaseName()+".meta.json";if(QFileInfo::exists(meta))check(QFile::copy(meta,target.absolutePath()+"/"+target.completeBaseName()+".meta.json"),"copy declared synthetic sidecar");}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);qa::configureFonts(app);
    QJsonObject report{{"schema","hdmi-unified-layout-host-v1"},{"scope","Actual Qt Window, explicit synthetic stream service, isolated temporary files; no physical board/display/touch acceptance"},{"worker",qEnvironmentVariable("AX7_CAPTURE_WORKER","0")}};QJsonArray runs;
    try{
        check(argc==4,"service evidence fixture arguments");check(!QFileInfo::exists("/dev/ax7z020-capture"),"refuses real capture hardware");const QString service=QString::fromLocal8Bit(argv[1]),fixture=QString::fromLocal8Bit(argv[3]);check(QFileInfo::exists(service)&&QFileInfo::exists(fixture),"explicit service and synthetic fixture exist");
        QDir output(QString::fromLocal8Bit(argv[2]));check(QDir().mkpath(output.absolutePath()),"evidence directory");QTemporaryDir scratch;check(scratch.isValid(),"isolated scratch directory");
        for(const auto size:{QSize(1024,600),QSize(1280,720)}){
            const auto tag=QStringLiteral("%1x%2").arg(size.width()).arg(size.height());QDir work(scratch.path()+"/"+tag);check(QDir().mkpath(work.filePath("capture"))&&QDir().mkpath(work.filePath("library/a/b/c")),"per-size isolated output and library");
            Window window;window.resize(size);auto* page=child<CapturePage>(window,"capturePage");page->setServiceCommand(service,{"--test-fixture","--fixture-chunk-ms=20"});child<QLineEdit>(*page,"captureDirectory")->setText(work.filePath("capture"));
            window.show();waitFor([&]{return child<QPushButton>(window,"workbenchStart")->isEnabled();},tag+" hidden capture controller connects without collecting");events();
            auto* pane=child<AnalysisPane>(window,"analysisPane");auto* wave=child<SingleWaveformView>(window,"singleWaveformView");auto* pages=child<QStackedWidget>(window,"newPages");
            check(window.size()==size&&pages->currentWidget()==pane&&child<QPushButton>(window,"viewTab0")->isChecked()&&wave->plot()->series().isEmpty(),tag+" startup is empty time view");
            check(!page->property("captureActive").toBool()&&!pane->isLive()&&QDir(work.filePath("capture")).entryList(QDir::NoDotAndDotDot|QDir::AllEntries).isEmpty(),tag+" connecting never starts capture");
            check(!window.findChild<QWidget*>("documentSidebar")&&!window.findChild<QPushButton*>("workbenchHome")&&!window.findChild<QWidget*>("commandBar"),tag+" welcome sidebar and old toolbar absent");
            actionGeometry(window,tag+" idle");check(wave->plot()->width()>=600&&wave->plot()->height()>=170,tag+" one large plot retains usable area");shot(window,output,"empty-time-"+tag);
            qa::interaction::settingAction(window,"workbenchAcquire");events();auto* parameters=child<QDialog>(window,"workbenchCaptureDialog");
            check(parameters->isVisible()&&parameters->isModal()&&pages->currentWidget()==pane&&parameters->height()<=window.height()&&parameters->width()<=window.width(),tag+" capture parameters are a fitting modal dialog");
            check(!page->property("captureActive").toBool(),tag+" opening parameters never starts capture");auto* rate=child<QComboBox>(*page,"captureRate");rate->setCurrentIndex(rate->findData(5000));auto* samples=child<QLineEdit>(*page,"captureSamples");samples->setText("8192");check(!child<QPushButton>(window,"workbenchStart")->isEnabled(),tag+" invalid count disables actual shell start");samples->setText("5000");sampleKeypad(window,*page,output,tag);shot(*parameters,output,"capture-parameters-"+tag);
            child<QPushButton>(*parameters,"captureParametersDone")->click();events();check(!parameters->isVisible()&&!page->property("captureActive").toBool()&&child<QPushButton>(window,"workbenchStart")->isEnabled(),tag+" confirm saves plan without starting");
            const auto library=work.filePath("library");const QStringList relatives{"data.BIN","a/old.BIN","a/b/history.bin","a/b/c/deep.BIN"};QMap<QString,QByteArray> hashes;for(const auto& relative:relatives){const auto path=library+'/'+relative;copyFixture(fixture,path);hashes[path]=hash(path);}QFile invalid(library+"/invalid.BIN");check(invalid.open(QIODevice::WriteOnly)&&invalid.write("invalid")==7,"isolated invalid BIN");invalid.close();window.setDirectory(library);qa::interaction::dataAction(window,"workbenchRecordsButton");events();
            auto* browser=child<QDialog>(window,"workbenchRecordBrowser");auto* list=child<QListWidget>(window,"workbenchRecords");check(browser->isVisible()&&browser->isModal()&&list->count()==3,tag+" SD browser only lists valid root and two-level BIN records");
            int chosen=-1;for(int i=0;i<list->count();++i){const auto path=list->item(i)->data(Qt::UserRole).toString();check(!path.endsWith("deep.BIN")&&!path.endsWith("invalid.BIN"),tag+" deeper and invalid records omitted");if(path.endsWith("history.bin"))chosen=i;}
            for(auto* button:browser->findChildren<QPushButton*>())check(!button->text().contains(QStringLiteral("删除")),tag+" browser has no deletion action");shot(*browser,output,"sd-browser-"+tag);check(chosen>=0,"two-level record available");list->setCurrentRow(chosen);child<QPushButton>(*browser,"openSelectedRecord")->click();waitFor([&]{return !window.isAnalyzing();},tag+" SD open uses existing analysis loader");check(!browser->isVisible()&&!wave->plot()->series().isEmpty(),tag+" selected SD record opens time analysis");for(auto it=hashes.cbegin();it!=hashes.cend();++it)check(hash(it.key())==it.value(),tag+" SD browsing preserves source bytes");
            window.setView(0);shot(window,output,"synthetic-time-"+tag);child<QPushButton>(window,"viewTab1")->click();events();auto* spectrum=child<SingleWaveformView>(window,"singleSpectrumView");const auto range=spectrum->plot()->explicitXRange();check(range&&std::abs(range->y()-2000)<1e-6,tag+" default spectrum matches PC 2000 Hz");shot(window,output,"synthetic-spectrum-"+tag);window.setView(0);
            child<QComboBox>(*page,"captureMode")->setCurrentIndex(1);QJsonObject terminal;int frames=0;QObject::connect(page,&CapturePage::captureState,&window,[&](const QJsonObject& status){const auto state=status.value("state").toString();if(state=="stopped"||state=="completed"||state=="failed")terminal=status;});QObject::connect(page,&CapturePage::liveFrame,&window,[&](const QJsonObject&){++frames;});
            qa::interaction::condition(window,"normal");check(page->nextRecordLabel()==RecordLabel::Normal,tag+" operator selects normal for synthetic organizer QA");
            child<QPushButton>(window,"workbenchStart")->click();waitFor([&]{return page->property("captureActive").toBool()&&pane->isLive()&&child<QPushButton>(window,"workbenchStart")->isEnabled();},tag+" actual top start reaches fixture and renders preview");check(page->property("captureActive").toBool()&&child<QPushButton>(window,"workbenchStart")->text().contains(QStringLiteral("停止"))&&!child<QComboBox>(window,"nextCaptureCondition")->isEnabled()&&!child<QPushButton>(window,"workbenchExport")->isEnabled(),tag+" capture locks conflicting start and export controls");
            const auto framesBeforeDetection=frames;child<QPushButton>(window,"viewTab3")->click();events();check(pages->currentWidget()->objectName()=="detectionPane",tag+" detection page remains accessible during capture");waitFor([&]{return frames>framesBeforeDetection&&page->property("captureActive").toBool()&&pane->isLive()&&terminal.isEmpty();},tag+" switching to detection preserves continuous raw acquisition and preview delivery");child<QPushButton>(window,"pageAcquisition")->click();events();check(pages->currentWidget()==pane&&page->property("captureActive").toBool(),tag+" return to acquisition without restarting session");actionGeometry(window,tag+" live");shot(window,output,"synthetic-live-"+tag);
            child<QPushButton>(window,"workbenchStart")->click();check(!child<QPushButton>(window,"workbenchStart")->isEnabled(),tag+" stopping keeps start locked while saving tail");waitFor([&]{return !page->property("captureActive").toBool()&&!window.isAnalyzing()&&!terminal.isEmpty();},tag+" top stop drains and opens final saved segment");
            check(terminal.value("state")=="stopped"&&terminal.value("records_received").toString()==terminal.value("durable_records").toString(),tag+" stopped received count equals durable count");const auto finalPath=page->property("captureLatestSegment").toString();const auto capture=load_capture(finalPath.toUtf8().toStdString());check(capture.header.stream_segment&&capture.source_metadata.synthetic&&capture.header.completed_records>0&&frames>0,tag+" final file validates and stays explicitly synthetic");quint64 saved=0;QDirIterator files(work.filePath("capture"),QStringList{"*.BIN","*.bin"},QDir::Files,QDirIterator::Subdirectories);while(files.hasNext())saved+=load_capture(files.next().toUtf8().toStdString()).header.completed_records;check(saved==terminal.value("durable_records").toString().toULongLong(),tag+" all saved BIN record counts match final durable count");
            actionGeometry(window,tag+" stopped");check(child<QPushButton>(window,"workbenchStart")->isVisible()&&child<QPushButton>(window,"workbenchStart")->isEnabled()&&child<QPushButton>(window,"workbenchStart")->text()==QStringLiteral("开始采集")&&child<QPushButton>(window,"workbenchExport")->isEnabled(),tag+" all main actions remain after stop and loaded result enables export");shot(window,output,"synthetic-stopped-"+tag);
            const auto copy=output.filePath("synthetic-final-segment-"+tag+".BIN");copyFixture(finalPath,copy);runs.append(QJsonObject{{"size",tag},{"preview_frames",frames},{"terminal",terminal},{"sum_saved_records",QString::number(saved)},{"retained_final_segment",copy},{"retained_final_segment_sha256",QString::fromLatin1(hash(copy))},{"font",qa::fontAudit(app.font())}});window.close();events();check(!window.isVisible(),tag+" normal final close");
        }
        report["result"]="passed";
    }catch(const std::exception& exception){report["result"]="failed";report["error"]=QString::fromUtf8(exception.what());}
    report["runs"]=runs;report["checks"]=checks;const auto json=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(json.constData(),1,std::size_t(json.size()),stdout);return report.value("result")=="passed"?0:1;
}
