// Independent real-widget/real-child-process host QA. No hardware is opened.
#include "acquisition_panel.hpp"
#include "main_window.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
QJsonArray checks, geometry, captures;
bool flowOnly=false;
void check(bool condition,const QString& name){
    checks.append(QJsonObject{{"check",name},{"passed",condition}});
    if(!condition)throw std::runtime_error(name.toStdString());
}
void waitFor(const std::function<bool()>& predicate,const QString& name,int timeout=12000){
    QElapsedTimer elapsed;elapsed.start();
    while(!predicate()&&elapsed.elapsed()<timeout){QApplication::processEvents();QThread::msleep(2);}
    QApplication::processEvents();check(predicate(),name);
}
template<class T>T* child(QObject& owner,const char* id){
    auto* value=owner.findChild<T*>(id);check(value,QString("widget exists: ")+id);return value;
}
void click(QPushButton* button){
    check(button->isVisible()&&button->isEnabled(),"real visible enabled action: "+button->objectName());
    check(button->width()>=44&&button->height()>=44,QString("touch action >=44px: %1 (%2x%3)").arg(button->objectName()).arg(button->width()).arg(button->height()));
    button->setFocus(Qt::MouseFocusReason);
    const QPointF point(button->rect().center());
    QMouseEvent down(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(button,&down);QApplication::sendEvent(button,&up);
    // Do not nest processEvents while a modal dialog's owning loop is closing.
}
void snapshot(QWidget& window,const QDir& out,const QString& name){
    if(flowOnly)return;
    QApplication::processEvents();check(window.grab().save(out.filePath(name+".png")),"real Qt render: "+name);
}
void bounds(QWidget& window,vibration::gui::AcquisitionPanel& panel,const QString& phase){
    if(flowOnly)return;
    QApplication::processEvents();
    const QStringList touch{"captureRate","captureDuration","capturePpr","captureBrowse","captureReconnect","captureStart","captureCancel","captureView"};
    for(const auto& id:touch){
        auto* widget=panel.findChild<QWidget*>(id);check(widget,"geometry widget: "+id);
        if(!widget->isVisible())continue;
        const QRect r(widget->mapTo(&window,QPoint()),widget->size());
        geometry.append(QJsonObject{{"phase",phase},{"id",id},{"x",r.x()},{"y",r.y()},{"width",r.width()},{"height",r.height()}});
        check(window.rect().contains(r),"inside window: "+phase+":"+id);
        check(r.width()>=44&&r.height()>=44,"44px control: "+phase+":"+id);
    }
    for(auto* label:panel.findChildren<QLabel*>()){
        if(!label->isVisible()||label->text().isEmpty())continue;
        const QRect r(label->mapTo(&window,QPoint()),label->size());
        check(window.rect().contains(r),"label inside window: "+phase+":"+label->text().left(24));
        const auto content=label->contentsRect();QFontMetrics fm(label->font());
        const auto needed=label->wordWrap()?fm.boundingRect(QRect(0,0,content.width(),10000),Qt::TextWordWrap,label->text()):fm.boundingRect(label->text());
        check(needed.height()<=content.height()+3,"label height fits: "+phase+":"+label->text().left(24));
        if(!label->wordWrap())check(fm.horizontalAdvance(label->text())<=content.width()+3,"label width fits: "+phase+":"+label->text().left(24));
    }
}
void chooseDirectory(QPushButton* opener,const QString& path){
    bool seen=false;QString error;QElapsedTimer elapsed;elapsed.start();QTimer poll;poll.setInterval(5);
    QObject::connect(&poll,&QTimer::timeout,[&]{
        if(auto* dialog=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())){
            if(!seen){seen=true;dialog->setDirectory(path);dialog->selectFile(path);QTimer::singleShot(0,dialog,[dialog]{static_cast<QDialog*>(dialog)->accept();});}
        }
        if(elapsed.elapsed()>5000){error="Directory dialog timed out";if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();}
    });
    poll.start();click(opener);poll.stop();check(seen,"actual existing-directory dialog opened");check(error.isEmpty(),"actual existing-directory action completed");
}
QJsonObject readJson(const QString& path){QFile f(path);check(f.open(QIODevice::ReadOnly),"read JSON evidence");return QJsonDocument::fromJson(f.readAll()).object();}
}

int main(int argc,char** argv){
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);app.setFont(QFont("Noto Sans CJK SC",10));
    QJsonObject report{{"schema","acquisition-gui-host-qa-v1"},{"boundary","Explicit synthetic fixture and missing-device host only; no physical sensor, board, SD or touch acceptance"}};
    try{
        check(argc==5||(argc==6&&QString::fromLatin1(argv[5])=="--flow-only"),"arguments: service evidence-directory size temporary-work-directory [--flow-only]");
        flowOnly=argc==6;report["flow_only"]=flowOnly;
        const QString service=QString::fromLocal8Bit(argv[1]);QDir out(QString::fromLocal8Bit(argv[2]));const QStringList size=QString::fromLatin1(argv[3]).split('x');QDir work(QString::fromLocal8Bit(argv[4]));
        check(QFileInfo(service).isExecutable(),"frozen real service executable exists");
        check(!QFileInfo::exists("/dev/ax7z020-capture"),"host probe refuses any real capture device");
        check(size.size()==2,"size syntax");check(out.exists()&&work.exists(),"QA directories exist");
        check(work.mkdir("captures"),"isolated capture directory created");const QString captureDir=work.filePath("captures");
        vibration::AnalysisConfig cfg;cfg.fft_size=1024;
        vibration::gui::MainWindow window(cfg);window.resize(size[0].toInt(),size[1].toInt());window.show();
        QApplication::processEvents();QThread::msleep(30);QApplication::processEvents();
        auto* panel=child<vibration::gui::AcquisitionPanel>(window,"acquisitionPanel");
        panel->setServiceCommand(service);click(child<QPushButton>(window,"deviceCaptureMode"));
        auto* start=child<QPushButton>(*panel,"captureStart");auto* cancel=child<QPushButton>(*panel,"captureCancel");
        auto* state=child<QLabel>(*panel,"captureState");auto* source=child<QLabel>(*panel,"captureSource");
        auto* rate=child<QComboBox>(*panel,"captureRate");auto* duration=child<QDoubleSpinBox>(*panel,"captureDuration");auto* ppr=child<QDoubleSpinBox>(*panel,"capturePpr");
        auto* view=child<QPushButton>(*panel,"captureView");auto* directory=child<QLineEdit>(*panel,"captureDirectory");
        waitFor([&]{return state->text()==QStringLiteral("设备未就绪")||state->text()==QStringLiteral("设备不可用");},"missing real device settles unavailable");
        check(!panel->property("captureAvailable").toBool()&&!start->isEnabled()&&!cancel->isEnabled(),"missing device disables capture without simulation fallback");
        check(!source->text().contains(QStringLiteral("合成")),"normal service did not switch to a synthetic source");
        check(duration->value()==10&&ppr->value()==1&&rate->currentData().toInt()==50000,"GUI defaults 50kHz 10seconds PPR1");
        if(!flowOnly)check(window.width()==size[0].toInt()&&window.height()==size[1].toInt(),"requested physical window extent");
        bounds(window,*panel,"unavailable");snapshot(window,out,"00-device-unavailable");
        panel->setServiceCommand(service,{"--test-fixture"});
        waitFor([&]{return panel->property("captureAvailable").toBool()&&start->isEnabled();},"explicit fixture connects through QProcess JSONL");
        check(source->text().contains(QStringLiteral("合成")),"fixture source visibly labelled synthetic");
        check(rate->count()==4,"all four backend rates exposed");
        for(int i=0;i<4;++i)check(rate->itemData(i).toInt()==QList<int>{50000,25000,10000,5000}[i],"supported rate value "+QString::number(i));
        check(duration->minimum()==1&&duration->maximum()==40&&ppr->minimum()==1&&ppr->maximum()==4096,"duration and PPR edit boundaries");
        chooseDirectory(child<QPushButton>(*panel,"captureBrowse"),captureDir);
        check(directory->text()==captureDir,"chosen existing directory reaches capture plan");duration->setValue(1);
        bounds(window,*panel,"ready");snapshot(window,out,"01-fixture-ready");
        click(start);waitFor([&]{return cancel->isEnabled();},"started job exposes cancellation");
        check(!start->isEnabled()&&!rate->isEnabled()&&!duration->isEnabled()&&!ppr->isEnabled(),"active capture locks plan and prevents concurrent starts");
        snapshot(window,out,"02-running");click(cancel);
        waitFor([&]{return !panel->property("captureActive").toBool();},"cancel request reaches terminal state");
        check(state->text()==QStringLiteral("已取消"),"cancelled job distinct from success and failure");
        check(QDir(captureDir).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty(),"cancelled job leaves no BIN, sidecar or staging file");
        bounds(window,*panel,"cancelled");snapshot(window,out,"03-cancelled");
        QString emittedPath;int readySignals=0;
        QObject::connect(panel,&vibration::gui::AcquisitionPanel::captureReady,&window,[&](const QString& path){emittedPath=path;++readySignals;});
        QString lastPath;
        for(int selected:{50000,25000,10000,5000}){
            rate->setCurrentIndex(rate->findData(selected));click(start);
            waitFor([&]{return !panel->property("captureActive").toBool()&&state->text()==QStringLiteral("采集完成");},"real service complete at "+QString::number(selected));
            check(view->isVisible()&&view->isEnabled(),"completion exposes view-record action");
            const auto files=QDir(captureDir).entryInfoList({"*.BIN"},QDir::Files,QDir::Time);
            check(!files.isEmpty(),"published BIN found after completion");lastPath=files.first().absoluteFilePath();
            const auto capture=vibration::load_capture(lastPath.toStdString());
            check(capture.integrity_verified&&capture.header.sample_rate_hz==unsigned(selected)&&capture.header.pulses_per_rev==1,"saved CAPDDR2 rate PPR and core integrity at "+QString::number(selected));
            check(capture.source_metadata.synthetic,"published file remains synthetic in existing loader");
            captures.append(QJsonObject{{"rate_hz",selected},{"records",double(capture.header.completed_records)},{"synthetic",capture.source_metadata.synthetic},{"header_crc32",double(capture.header.header_crc32)},{"payload_crc32",double(capture.header.payload_crc32)}});
        }
        check(QDir(captureDir).entryList({"*.BIN"},QDir::Files).size()==4,"four unique successful files after four rates");
        bounds(window,*panel,"completed");snapshot(window,out,"04-completed");
        panel->setAnalysisBusy(true);check(!view->isEnabled(),"view record disabled while analysis is busy");panel->setAnalysisBusy(false);
        click(view);waitFor([&]{return readySignals==1&&!window.isAnalyzing();},"view-record signal opens and analyzes actual newly saved file",20000);
        check(emittedPath==lastPath,"view-record path equals last newly published capture");
        check(child<QLabel>(window,"currentFile")->toolTip()==lastPath,"main window current record is the new capture");
        check(child<QWidget>(window,"waveformPlot")->isVisible(),"view-record switches to existing time analysis page");
        const QString analysisPath=work.filePath("actual-view-analysis.json");QString error;
        check(window.saveResult(analysisPath,false,&error),"existing analysis exports actual new capture: "+error);
        const auto analysis=readJson(analysisPath);
        check(analysis.value("source_metadata").toObject().value("synthetic").toBool(),"new record analysis retains synthetic source");
        check(!window.setDetectionMeasurementConditions(vibration::SignalKind::VibrationConfirmed,true,&error),"synthetic capture cannot be promoted to confirmed mechanical vibration");
        snapshot(window,out,"05-view-record-analysis");
        report["actual_analysis"]=analysis;report["result"]="passed";
    }catch(const std::exception& error){report["result"]="failed";report["error"]=QString::fromUtf8(error.what());}
    report["checks"]=checks;report["geometry"]=geometry;report["captures"]=captures;
    const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(data.constData(),1,std::size_t(data.size()),stdout);
    return report.value("result")=="passed"?0:1;
}
