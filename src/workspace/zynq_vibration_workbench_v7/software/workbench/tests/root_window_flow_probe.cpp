#include "window.hpp"
#include "capture_page.hpp"
#include "record_labels.hpp"
#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include <vibration/core.hpp>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtEndian>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace vibration::workbench;
namespace {
QJsonArray checks,shots,plotGeometry;
void check(bool passed,const QString& description){checks.append(QJsonObject{{"check",description},{"passed",passed}});if(!passed)throw std::runtime_error(description.toStdString());}
void events(int milliseconds=30){QElapsedTimer timer;timer.start();do{QApplication::processEvents();QThread::msleep(1);}while(timer.elapsed()<milliseconds);}
void waitFor(const std::function<bool()>& predicate,const QString& description){QElapsedTimer timer;timer.start();while(!predicate()&&timer.elapsed()<15000)events(5);check(predicate(),description);events();}
template<class T>T* child(QObject& owner,const char* name){auto* result=owner.findChild<T*>(name);check(result,QString("control ")+name);return result;}
void word(QByteArray& bytes,int offset,quint32 value){qToLittleEndian(value,reinterpret_cast<uchar*>(bytes.data()+offset));}
void wide(QByteArray& bytes,int offset,quint64 value){qToLittleEndian(value,reinterpret_cast<uchar*>(bytes.data()+offset));}
void write(const QString& path,const QByteArray& bytes){QFile file(path);check(file.open(QIODevice::WriteOnly|QIODevice::NewOnly)&&file.write(bytes)==bytes.size(),"create isolated QA fixture "+QFileInfo(path).fileName());}
QByteArray fixture(){
    constexpr int count=8192,rate=5000;QByteArray bytes(4096+count*32,0);bytes.replace(0,8,QByteArray("CAPDDR2\0",8));
    word(bytes,8,2);word(bytes,12,4096);word(bytes,16,32);word(bytes,20,2);word(bytes,24,rate);word(bytes,28,50000000);word(bytes,32,count);word(bytes,36,count);word(bytes,40,count*32);word(bytes,48,64);word(bytes,60,1);wide(bytes,64,50000000);wide(bytes,72,50000000ULL+(count-1)*10000ULL);word(bytes,80,328449);word(bytes,84,5000000);word(bytes,88,5);word(bytes,92,100000000);
    for(int i=0;i<count;++i){const auto at=4096+i*32;const quint64 tick=50000000ULL+quint64(i)*10000;wide(bytes,at,quint64(i));wide(bytes,at+8,tick);for(int channel=0;channel<3;++channel){const auto sample=qint16((2200-400*channel)*std::sin(2*3.141592653589793*30*i/rate+channel*.2));qToLittleEndian(sample,reinterpret_cast<uchar*>(bytes.data()+at+16+channel*2));}qToLittleEndian<quint16>(3,reinterpret_cast<uchar*>(bytes.data()+at+22));word(bytes,at+24,1666667);word(bytes,at+28,quint32(tick/1666667));}
    word(bytes,96,vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()+4096),count*32));word(bytes,100,vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),128));return bytes;
}
void screenshot(QWidget& widget,const QString& evidence,const QString& name){events(70);const auto path=QDir(evidence).filePath(name+".png");check(widget.grab().save(path),"screenshot "+name);shots.append(path);}
void fits(QWidget& surface,const QStringList& names){for(const auto& name:names){auto* widget=surface.findChild<QWidget*>(name);check(widget&&widget->isVisible()&&surface.rect().contains(QRect(widget->mapTo(&surface,QPoint()),widget->size())),"visible and contained "+name);}}
QListWidgetItem* record(QListWidget& list,const QString& path){for(int i=0;i<list.count();++i)if(list.item(i)->data(Qt::UserRole).toString()==path)return list.item(i);return nullptr;}
void select(QListWidget& list,const QString& path){auto* item=record(list,path);check(item,"record discoverable by stable path "+QFileInfo(path).fileName());list.setCurrentItem(item);list.scrollToItem(item);events();}
void sensitivityInput(QDoubleSpinBox& field,const QString& value){
    bool visited=false,valid=true;
    QTimer::singleShot(0,[&]{
        auto* keyboard=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!keyboard||keyboard->objectName()!="workbenchNumberPad"){valid=false;return;}
        visited=true;auto* clear=keyboard->findChild<QPushButton*>("touchKeyboardClear");auto* apply=keyboard->findChild<QPushButton*>("touchKeyboardApply");
        if(!clear||!apply){valid=false;keyboard->reject();return;}clear->click();
        for(const auto character:value){auto* key=keyboard->findChild<QPushButton*>("touchKey_"+QString(character));if(!key){valid=false;keyboard->reject();return;}key->click();}apply->click();
    });
    auto* editor=field.findChild<QLineEdit*>();check(editor,"sensitivity touch number editor");
    QMouseEvent tap(QEvent::MouseButtonRelease,QPointF(10,10),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(editor,&tap);
    check(visited&&valid&&std::abs(field.value()-value.toDouble())<1e-8,"sensitivity number entered through touch keypad "+field.objectName());
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);QJsonObject report{{"schema","v7-root-window-flow-qa-v1"},{"scope","All BINs and captures are explicitly synthetic software QA. Labels exercise organization only; no mechanical classification accuracy or board acceptance."}};
    try{
        check(argc==3,"host stream service and evidence arguments");check(!QFileInfo::exists("/dev/ax7z020-capture"),"refuse physical hardware");const QString evidence=QString::fromLocal8Bit(argv[2]);check(QDir().mkpath(evidence),"evidence directory");
        for(const QSize size:{QSize(1024,600),QSize(1280,720)}){
            const auto prefix=QString::number(size.width())+"-";QTemporaryDir storage;check(storage.isValid(),"isolated application root "+prefix);const auto bytes=fixture();QString error;
            const QList<RecordLabel> labels{RecordLabel::Normal,RecordLabel::Looseness,RecordLabel::Misalignment,RecordLabel::Imbalance,RecordLabel::Unlabeled};
            for(auto label:labels){const auto path=storage.filePath(recordLabelKey(label)+".BIN");write(path,bytes);write(storage.filePath(recordLabelKey(label)+".meta.json"),"{\"synthetic\":true,\"purpose\":\"v7_window_qa_only\"}");if(label!=RecordLabel::Unlabeled)check(saveRecordLabel(path,label,{},nullptr,&error),"seed synthetic organizer label "+recordLabelKey(label)+error);}
            const auto corrupt=storage.filePath("corrupt-label.BIN");write(corrupt,bytes);write(recordLabelSidecarPath(corrupt),"{broken");
            Window window(nullptr,storage.path());window.resize(size);window.setDirectory(storage.path());auto* capture=child<CapturePage>(window,"capturePage");capture->setServiceCommand(QString::fromLocal8Bit(argv[1]),{"--test-fixture","--fixture-chunk-ms=4"});child<QLineEdit>(*capture,"captureDirectory")->setText(storage.path());auto* rates=child<QComboBox>(*capture,"captureRate");rates->setCurrentIndex(rates->findData(5000));window.show();events();
            auto* start=child<QPushButton>(window,"workbenchStart");waitFor([&]{return start->isEnabled();},"host fixture connects "+prefix);qa::interaction::tap(start);check(!capture->property("captureActive").toBool()&&child<QLabel>(window,"documentState")->text().contains(QStringLiteral("先选择")),"shell requires conscious operator label before capture");
            qa::interaction::condition(window,"looseness");check(capture->nextRecordLabel()==RecordLabel::Looseness,"touch dropdown chooses next-session looseness");
            child<QComboBox>(*capture,"captureMode")->setCurrentIndex(1);qa::interaction::tap(start);waitFor([&]{return capture->property("captureActive").toBool()&&start->isEnabled();},"labelled session active "+prefix);
            check(!child<QComboBox>(window,"nextCaptureCondition")->isEnabled(),"operator dropdown cannot change during capture");
            waitFor([&]{return !capture->property("captureLatestSegment").toString().isEmpty();},"synthetic first segment committed "+prefix);
            qa::interaction::tap(start);waitFor([&]{return !capture->property("captureActive").toBool()&&!window.isAnalyzing();},"labelled stop saves final segment "+prefix);
            RecordLabelAnnotation captured;check(loadRecordLabel(capture->property("captureLatestSegment").toString(),&captured,&error)&&captured.label==RecordLabel::Looseness&&captured.synthetic,"captured sidecar carries operator label and synthetic guard");
            qa::interaction::condition(window,"unlabeled");check(capture->nextRecordLabel()==RecordLabel::Unlabeled,"unknown capture does not require a predicted fault label");
            child<QComboBox>(*capture,"captureMode")->setCurrentIndex(0);window.setView(3);qa::interaction::tap(start);waitFor([&]{return capture->property("captureActive").toBool()&&start->isEnabled();},"explicit unknown input can start capture");
            waitFor([&]{return !capture->property("captureActive").toBool()&&!window.isAnalyzing();},"unknown record completes without automatic reference admission");
            check(window.property("majorPage").toString()=="detection","unknown acquisition keeps the requested detection page through preview and completion");
            RecordLabelAnnotation unknown;check(loadRecordLabel(capture->property("captureLatestSegment").toString(),&unknown,&error)&&unknown.label==RecordLabel::Unlabeled&&!unknown.exists,"unknown record remains unlabelled and is not a class training target");
            window.setView(0);qa::interaction::condition(window,"looseness");
            const auto normal=storage.filePath("normal.BIN");window.openCapture(normal);waitFor([&]{return !window.isAnalyzing();},"load synthetic plot source "+prefix);check(child<QLabel>(window,"documentState")->text()==QStringLiteral("合成演示"),"labels do not turn a synthetic source into physical data");
            for(int view=0;view<4;++view){
                window.setView(view);events();check(window.size()==size,"main page retains requested board resolution "+prefix+QString::number(view));screenshot(window,evidence,prefix+QString("page-%1").arg(view));
                if(view!=3){
                    fits(window,{"pageAcquisition","viewTab3","nextCaptureCondition","workbenchDataMenu","workbenchOptions","viewTab0","viewTab1","viewTab2"});
                    auto* graph=child<SingleWaveformView>(window,view==1?"singleSpectrumView":"singleWaveformView")->plot();graph->grab();const auto area=graph->plotArea();const int minimum=view==2?160:200;
                    plotGeometry.append(QJsonObject{{"width",size.width()},{"height",size.height()},{"view",view},{"plot_width",area.width()},{"plot_height",area.height()},{"minimum_height",minimum}});
                    check(area.height()>=minimum,"actual drawing height meets minimum "+prefix+QString::number(view)+" "+QString::number(area.height()));
                }else fits(window,{"pageAcquisition","viewTab3","openReferenceLibrary","openCandidateModelSettings"});
            }
            window.setView(2);events();fits(window,{"workbenchOptions"});qa::interaction::settingAction(window,"openAccelerationSensitivity");events();auto* sensitivity=child<QDialog>(window,"accelerationSensitivityDialog");check(sensitivity->isVisible(),"same-page sensitivity dialog opens through touch entry");
            fits(*sensitivity,{"accelerationSensitivity1","accelerationSensitivity2","accelerationSensitivity3","applyAccelerationSensitivity"});
            for(int channel=1;channel<=3;++channel){auto* field=child<QDoubleSpinBox>(*sensitivity,QString("accelerationSensitivity%1").arg(channel).toUtf8().constData());check(field->height()>=44,"sensitivity input keeps touch height");sensitivityInput(*field,QString::number(50+channel)+".25");}
            child<QPushButton>(*sensitivity,"applyAccelerationSensitivity")->click();events();check(child<QLabel>(*sensitivity,"sensitivityDialogStatus")->text().contains(QStringLiteral("已保存")),"sensitivity application confirms saved values inside dialog");screenshot(*sensitivity,evidence,prefix+"sensitivity-dialog");sensitivity->reject();window.setView(3);events();
            child<QPushButton>(window,"openReferenceLibrary")->click();events();auto* reference=child<QDialog>(window,"referenceLibraryDialog");check(reference->isVisible(),"reference library opens from detection");screenshot(*reference,evidence,prefix+"reference-library");reference->reject();events();
            qa::interaction::settingAction(window,"workbenchAcquire");events();auto* parameters=child<QDialog>(window,"workbenchCaptureDialog");check(parameters->isVisible(),"capture parameters remain accessible");screenshot(*parameters,evidence,prefix+"capture-parameters");parameters->reject();events();
            qa::interaction::dataAction(window,"workbenchRecordsButton");events();auto* dialog=child<QDialog>(window,"workbenchRecordBrowser");auto* list=child<QListWidget>(*dialog,"workbenchRecords");auto* filter=child<QComboBox>(*dialog,"recordLabelFilter");auto* editor=child<QComboBox>(*dialog,"recordLabelEditor");auto* save=child<QPushButton>(*dialog,"saveRecordLabel");auto* add=child<QPushButton>(*dialog,"addRecordToReferenceLibrary");
            check(dialog->isVisible()&&filter->count()==6,"record browser exposes all/four-class/unlabelled filters");
            select(*list,normal);check(!add->isEnabled()&&save->isEnabled(),"synthetic record can be organized but cannot enter mechanical reference library");screenshot(*dialog,evidence,prefix+"records-all");
            for(auto label:labels){filter->setCurrentIndex(filter->findData(recordLabelKey(label)));events();const auto path=storage.filePath(recordLabelKey(label)+".BIN");select(*list,path);for(int row=0;row<list->count();++row){const auto itemPath=list->item(row)->data(Qt::UserRole).toString();if(itemPath.isEmpty())continue;RecordLabelAnnotation annotation;check(loadRecordLabel(itemPath,&annotation,&error)&&annotation.label==label,"filter contains only selected operator label "+recordLabelKey(label));}screenshot(*dialog,evidence,prefix+"records-"+recordLabelKey(label));}
            filter->setCurrentIndex(filter->findData("all"));events();select(*list,corrupt);check(!save->isEnabled()&&!add->isEnabled()&&child<QLabel>(*dialog,"recordAnnotationStatus")->text().contains(QStringLiteral("失败")),"corrupt annotation is visible and never silently replaced");
            select(*list,normal);editor->setCurrentIndex(editor->findData("imbalance"));save->click();events();RecordLabelAnnotation updated;check(loadRecordLabel(normal,&updated,&error)&&updated.label==RecordLabel::Imbalance&&updated.history.size()==2,"manual relabel commits to sidecar and preserves history");check(capture->nextRecordLabel()==RecordLabel::Looseness,"editing old record does not change next capture label");check(!add->isEnabled(),"manual normal/fault relabel cannot promote synthetic source");
            fits(*dialog,{"recordLabelFilter","recordLabelEditor","saveRecordLabel","addRecordToReferenceLibrary","openSelectedRecord"});dialog->reject();window.close();events();
        }
        report["passed"]=true;
    }catch(const std::exception& error){report["passed"]=false;report["error"]=QString::fromUtf8(error.what());}
    report["checks"]=checks;report["screenshots"]=shots;report["plot_geometry"]=plotGeometry;auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return report.value("passed").toBool()?0:1;
}
