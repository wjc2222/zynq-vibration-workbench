#include "capture_page.hpp"
#include "capture_client.hpp"
#include "controls.hpp"
#include "touch_directory_dialog.hpp"
#include <vibration/core.hpp>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QTextCodec>
#include <QVariant>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
QJsonArray checks,captures;
void check(bool value,const QString& name){checks.append(QJsonObject{{"check",name},{"passed",value}});if(!value)throw std::runtime_error(name.toStdString());}
template<class T>T* find(QObject& owner,const char* name){auto* result=owner.findChild<T*>(name);check(result,QString("control: ")+name);return result;}
void waitFor(const std::function<bool()>& predicate,const QString& name){QElapsedTimer t;t.start();while(!predicate()&&t.elapsed()<12000){QApplication::processEvents();QThread::msleep(2);}QApplication::processEvents();check(predicate(),name);}
void click(QPushButton* button){
    check(button->isVisible()&&button->isEnabled(),"actual enabled action: "+button->objectName());
    check(button->width()>=44&&button->height()>=44,"44px touch action: "+button->objectName());
    const QPointF p(button->rect().center());button->setFocus(Qt::MouseFocusReason);
    QMouseEvent down(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier),up(QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(button,&down);QApplication::sendEvent(button,&up);
}
void shot(QWidget& page,const QDir& out,const QString& name){QApplication::processEvents();check(page.grab().save(out.filePath(name+".png")),"standalone new-page render: "+name);}
void bounds(QWidget& page){
    for(auto* widget:page.findChildren<QWidget*>()){
        if(!widget->isVisible()||widget->objectName().isEmpty())continue;
        const QRect r(widget->mapTo(&page,QPoint()),widget->size());check(page.rect().contains(r),"visible widget within new page: "+widget->objectName());
    }
}
void chooseDirectory(QPushButton* button,const QString& path){
    bool seen=false;QTimer timer;timer.setInterval(5);QElapsedTimer elapsed;elapsed.start();
    QObject::connect(&timer,&QTimer::timeout,[&]{
        if(auto* dialog=dynamic_cast<vibration::workbench::TouchDirectoryDialog*>(QApplication::activeModalWidget()))if(!seen){
            seen=true;auto* list=find<QListWidget>(*dialog,"touchDirectoryList");const auto wanted=QFileInfo(path).canonicalFilePath();QListWidgetItem* target=nullptr;
            for(int row=0;row<list->count();++row)if(QFileInfo(list->item(row)->data(Qt::UserRole).toString()).canonicalFilePath()==wanted)target=list->item(row);
            check(target,"temporary allowed-root contains requested directory");
            const QPointF p(list->visualItemRect(target).center());QMouseEvent down(QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier),up(QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(list->viewport(),&down);QApplication::sendEvent(list->viewport(),&up);
            check(dialog->currentDirectory()==wanted,"touch picker enters requested fixture child");click(find<QPushButton>(*dialog,"touchDirectoryChoose"));
        }
        if(elapsed.elapsed()>5000)if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();
    });timer.start();click(button);timer.stop();check(seen,"real touch existing-directory selection");
}
}
int main(int argc,char** argv){
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));QApplication app(argc,argv);
    for(const auto& fontPath:{"/mnt/c/Windows/Fonts/segoeui.ttf","/mnt/c/Windows/Fonts/seguisb.ttf","/mnt/c/Windows/Fonts/msyh.ttc","/mnt/c/Windows/Fonts/msyhbd.ttc"})if(QFileInfo::exists(fontPath))QFontDatabase::addApplicationFont(fontPath);
    QFont font;font.setFamilies({"Segoe UI","Microsoft YaHei UI","Microsoft YaHei","Noto Sans CJK SC","sans-serif"});font.setPixelSize(14);font.setWeight(QFont::Normal);font.setStyleStrategy(QFont::PreferAntialias);font.setHintingPreference(QFont::PreferDefaultHinting);app.setFont(font);app.setStyleSheet(vibration::workbench::stylesheet());
    QJsonObject report{{"schema","new-capture-page-host-qa-v1"},{"scope","New independent CapturePage only; shell styling and hardware are outside this probe"}};
    try{
        check(argc==7,"service python malformed-peer evidence work size arguments");
        const QString service=QString::fromLocal8Bit(argv[1]),python=QString::fromLocal8Bit(argv[2]),peer=QString::fromLocal8Bit(argv[3]);QDir out(QString::fromLocal8Bit(argv[4])),work(QString::fromLocal8Bit(argv[5]));const auto size=QString::fromLatin1(argv[6]).split('x');
        check(!QFileInfo::exists("/dev/ax7z020-capture"),"host test refuses real hardware");check(size.size()==2&&out.exists()&&work.exists(),"test paths and extent");check(work.mkdir("captures"),"isolated output directory");const auto destination=work.filePath("captures");
        vibration::workbench::CapturePage page([]{return new vibration::workbench::NumberField;},nullptr,work.absolutePath());page.setStreaming(false);page.resize(size[0].toInt(),size[1].toInt());page.setServiceCommand(service);page.show();QApplication::processEvents();
        auto* start=find<QPushButton>(page,"captureStart");auto* cancel=find<QPushButton>(page,"captureCancel");auto* view=find<QPushButton>(page,"captureView");auto* rate=find<QComboBox>(page,"captureRate");auto* duration=find<QDoubleSpinBox>(page,"captureDuration");auto* ppr=find<QDoubleSpinBox>(page,"capturePpr");auto* state=find<QLabel>(page,"captureState");
        int statuses=0,ready=0,liveFrames=0,previewErrors=0,lateFrames=0;bool liveActive=false;QJsonObject lastStatus;QString path;
        QObject::connect(&page,&vibration::workbench::CapturePage::statusChanged,&page,[&](const QString&){++statuses;});QObject::connect(&page,&vibration::workbench::CapturePage::captureReady,&page,[&](const QString& p){++ready;path=p;});
        QObject::connect(&page,&vibration::workbench::CapturePage::captureState,&page,[&](const QJsonObject& data){
            lastStatus=data;if(data.contains("preview_error")){++previewErrors;return;}
            const auto state=data.value("state").toString();liveActive=state!="failed"&&state!="cancelled"&&state!="completed";
        });
        QObject::connect(&page,&vibration::workbench::CapturePage::liveFrame,&page,[&](const QJsonObject& frame){
            ++liveFrames;if(!liveActive)++lateFrames;
            check(frame.value("ppr").toInt()==int(ppr->value())&&frame.value("sample_rate_hz").toInt()==rate->currentData().toInt(),"forwarded preview matches user-confirmed rate/PPR");
        });
        waitFor([&]{return state->text()==QStringLiteral("设备未连接");},"missing device unavailable");check(!start->isEnabled()&&!page.property("captureAvailable").toBool(),"no implicit fixture fallback");check(duration->value()==10&&ppr->value()==1&&rate->currentData().toInt()==50000,"50kHz 10second PPR1 defaults");
        auto* samples=find<QLineEdit>(page,"captureSamples");const auto a=QRect(rate->mapTo(&page,QPoint()),rate->size()),b=QRect(samples->mapTo(&page,QPoint()),samples->size()),c=QRect(duration->mapTo(&page,QPoint()),duration->size()),d=QRect(ppr->mapTo(&page,QPoint()),ppr->size());check(a.top()==b.top()&&c.top()==d.top()&&a.right()<b.left()&&c.right()<d.left()&&a.bottom()<c.top(),"four acquisition parameters in compact two-by-two grid");
        check(find<QWidget>(page,"captureActionBar")->y()>find<QWidget>(page,"captureParameters")->geometry().bottom(),"independent bottom action bar");bounds(page);shot(page,out,"00-unavailable");
        page.setServiceCommand(service,{"--test-fixture"});waitFor([&]{return start->isEnabled();},"explicit fixture ready");check(find<QLabel>(page,"captureSource")->text().contains(QStringLiteral("合成")),"synthetic label");check(rate->count()==4,"four supported rates");chooseDirectory(find<QPushButton>(page,"captureBrowse"),destination);duration->setValue(1);shot(page,out,"01-ready");
        click(start);waitFor([&]{return cancel->isEnabled();},"job starts");check(!start->isEnabled()&&!rate->isEnabled(),"single active plan");click(cancel);waitFor([&]{return !page.property("captureActive").toBool();},"cancel completes");check(state->text()==QStringLiteral("已取消"),"cancel distinct from success");check(QDir(destination).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty(),"cancel leaves no data/staging files");
        for(int hz:{50000,25000,10000,5000}){
            const int frameBefore=liveFrames;
            rate->setCurrentIndex(rate->findData(hz));click(start);waitFor([&]{return !page.property("captureActive").toBool()&&state->text()==QStringLiteral("采集完成");},"complete rate "+QString::number(hz));
            check(liveFrames-frameBefore>=10,"continuous committed preview forwarded at rate "+QString::number(hz));
            check(view->isVisible()&&view->isEnabled(),"open record available only on completion");page.setAnalysisBusy(true);check(!view->isEnabled(),"analysis busy blocks open record");page.setAnalysisBusy(false);click(view);const auto capture=vibration::load_capture(path.toStdString());check(capture.integrity_verified&&capture.source_metadata.synthetic&&capture.header.sample_rate_hz==unsigned(hz)&&capture.header.pulses_per_rev==1,"published file integrity/source/rate/PPR");captures.append(QJsonObject{{"rate_hz",hz},{"synthetic",true},{"records",double(capture.header.completed_records)}});
        }
        check(ready==4&&statuses>4,"captureReady/statusChanged emitted");bounds(page);shot(page,out,"02-completed");
        check(lateFrames==0&&previewErrors==0,"valid fixture frames precede terminal and have no preview errors");
        for(const auto& item:QList<QPair<QString,int>>{{"preview_bad_ppr",0},{"preview_bad_source",0},{"preview_missing",1},{"preview_crc",1},{"preview_padbits",1},{"preview_unicode",1},{"preview_fractional",1},{"preview_schema",1},{"preview_type",1},{"preview_extra",1},{"preview_duplicate_key",1},{"preview_duplicate",2},{"preview_generation",1},{"preview_source_change",1}}){
            page.setServiceCommand(python,{"-B",peer,item.first,path});waitFor([&]{return start->isEnabled();},"explicit adversarial live peer ready: "+item.first);
            const int before=liveFrames,rejections=previewErrors;click(start);
            waitFor([&]{return !page.property("captureActive").toBool()&&state->text()==QStringLiteral("采集完成");},"invalid preview does not fail final job: "+item.first);
            check(liveFrames-before==item.second,"rejected/quarantined preview and late-frame counts: "+item.first);
            check(previewErrors>rejections,"preview rejection is separately reported: "+item.first);
            check(lateFrames==0,"no terminal preview reaches renderer: "+item.first);
        }
        page.setServiceCommand(python,{"-B",peer,"preview_start_plan",path});waitFor([&]{return start->isEnabled();},"mismatched start-plan peer ready");click(start);
        waitFor([&]{return state->text()==QStringLiteral("设备未连接");},"mismatched start echo cannot confirm user PPR");
        check(page.property("captureDiagnostic").toString().contains("requested plan"),"start-plan error is traceable");
        page.setServiceCommand(python,{"-B",peer,"disconnect_active",path});waitFor([&]{return start->isEnabled();},"disconnect peer ready");click(start);
        waitFor([&]{return lastStatus.value("connection_lost").toBool();},"active disconnect emits terminal state for window unlock");
        check(lastStatus.value("state")=="failed"&&!lastStatus.value("driver_stop_confirmed").toBool()&&lastStatus.value("output_path").toString().isEmpty(),"connection loss does not claim hardware stopped or file completed");
        page.setServiceCommand(service,{"--test-fixture","--fixture-fault=preview_crc"});waitFor([&]{return start->isEnabled();},"real service explicit preview-CRC fixture ready");
        const int rejectedBefore=previewErrors,framesBefore=liveFrames;click(start);
        waitFor([&]{return !page.property("captureActive").toBool()&&state->text()==QStringLiteral("采集完成");},"bad preview CRC permits separately validated final BIN");
        check(previewErrors>rejectedBefore&&liveFrames==framesBefore&&view->isVisible(),"bad CRC never forwarded but final record accessible");
        for(const auto& item:QList<QPair<QString,QString>>{{"schema","version mismatch"},{"id","response ID"},{"duplicate","Duplicate JSON key"},{"escaped_duplicate","Duplicate JSON key"},{"long","line limit"}}){
            page.setServiceCommand(python,{"-B",peer,item.first});waitFor([&]{return page.property("captureDiagnostic").toString().contains(item.second);},"malformed child rejected: "+item.first);check(!start->isEnabled(),"malformed peer cannot enable capture");
        }
        page.setServiceCommand(service,{"--test-fixture","--fixture-fault=crc"});waitFor([&]{return start->isEnabled();},"fault fixture explicitly selected");const int before=QDir(destination).entryList({"*.BIN"},QDir::Files).size();click(start);waitFor([&]{return !page.property("captureActive").toBool()&&state->text()==QStringLiteral("采集未完成");},"CRC rejection shown near form");check(!view->isVisible()&&QDir(destination).entryList({"*.BIN"},QDir::Files).size()==before,"CRC failure exposes no new record");shot(page,out,"03-failure");
        check(work.mkdir("close-active"),"isolated close-active directory");
        const auto closeDirectory=work.filePath("close-active");
        auto* closing=new vibration::workbench::CapturePage([]{return new vibration::workbench::NumberField;});
        closing->setStreaming(false);
        closing->setServiceCommand(service,{"--test-fixture"});closing->resize(800,500);closing->show();
        auto* closeStart=find<QPushButton>(*closing,"captureStart");
        waitFor([&]{return closeStart->isEnabled();},"second page explicit fixture connects");
        find<QComboBox>(*closing,"captureRate")->setCurrentIndex(3);
        find<QDoubleSpinBox>(*closing,"captureDuration")->setValue(1);
        find<QLineEdit>(*closing,"captureDirectory")->setText(closeDirectory);
        auto* closeCancel=find<QPushButton>(*closing,"captureCancel");
        auto* closeClient=closing->findChild<vibration::workbench::CaptureClient*>();check(closeClient,"closing page transport exists");
        QJsonObject closingTransport;
        QObject::connect(closeClient,&vibration::workbench::CaptureClient::transportProfileUpdated,&app,[&](const QJsonObject& value){closingTransport=value;});
        click(closeStart);waitFor([&]{return closeCancel->isEnabled();},"second page owns active capture before close");
        delete closing;QApplication::processEvents();
        check(QDir(closeDirectory).entryList(QDir::Files|QDir::Hidden|QDir::NoDotAndDotDot).isEmpty(),"page destruction cancels and joins child without publishing BIN");
        auto* transport=page.findChild<vibration::workbench::CaptureClient*>();check(transport,"main page transport exists");const auto profile=transport->transportProfile();
        if(qgetenv("AX7_CAPTURE_PROFILE")=="1"){
            check(profile.value("enabled").toBool(),"transport profiling explicitly enabled");
            const auto metrics=profile.value("metrics").toObject();
            for(const auto* name:{"receive_total","json_parse","unique_keys","preview_verify","base64_materialize","base64_decode","frame_crc","record_validation","signal_delivery"})
                check(metrics.value(name).toObject().value("calls").toDouble()>0&&metrics.value(name).toObject().value("total_ns").toDouble()>0,QString("profile counter: ")+name);
            check(profile.value("decoder_mode")=="single_pass_utf16"&&!metrics.contains("base64_canonical"),"shared strict decoder fuses canonicality without re-encode");
            const auto service=closingTransport.value("service_profile").toObject();
            check(service.contains("service")&&service.contains("output"),"stop drains final service and writer stderr profiles before destruction");
            check(service.value("output").toObject().value("phase")=="writer_shutdown","final writer shutdown is observed");
            report["transport_profile"]=profile;report["closing_transport_profile"]=closingTransport;
        }else check(!profile.value("enabled").toBool()&&profile.value("metrics").toObject().isEmpty()&&profile.value("counters").toObject().isEmpty(),"transport profiling disabled by default with zero counters");
        report["live_frames_forwarded"]=liveFrames;report["preview_rejections"]=previewErrors;report["late_frames_forwarded"]=lateFrames;report["result"]="passed";
    }catch(const std::exception& e){report["result"]="failed";report["error"]=QString::fromUtf8(e.what());}
    report["checks"]=checks;report["captures"]=captures;const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return report.value("result")=="passed"?0:1;
}
