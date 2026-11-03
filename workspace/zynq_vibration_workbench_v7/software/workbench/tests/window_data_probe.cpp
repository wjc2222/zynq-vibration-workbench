#include "window.hpp"
#include "ui_interaction.hpp"
#include <vibration/core.hpp>
#include <vibration/detection.hpp>
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
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
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QTextCodec>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace {
QJsonArray checks,dialogTrace;
void check(bool value,const QString& name){checks.append(QJsonObject{{"check",name},{"passed",value}});if(!value)throw std::runtime_error(name.toStdString());}
template<class T>T* find(QObject& owner,const char* name){auto* p=owner.findChild<T*>(name);check(p,QString("control: ")+name);return p;}
QByteArray bytes(const QString& path){QFile file(path);check(file.open(QIODevice::ReadOnly),"read evidence "+QFileInfo(path).fileName());return file.readAll();}
QString hash(const QString& path){return QString::fromLatin1(QCryptographicHash::hash(bytes(path),QCryptographicHash::Sha256).toHex());}
void click(QPushButton* button){
    check(button->isVisible()&&button->isEnabled(),"actual enabled action: "+button->objectName());
    const QPointF point(button->rect().center());button->setFocus(Qt::MouseFocusReason);
    QMouseEvent down(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier),up(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(button,&down);QApplication::sendEvent(button,&up);
}
void wait(vibration::workbench::Window& window){QElapsedTimer elapsed;elapsed.start();while(window.isAnalyzing()&&elapsed.elapsed()<15000){QApplication::processEvents();QThread::msleep(2);}QApplication::processEvents();check(!window.isAnalyzing(),"asynchronous real core analysis finished");}
void withSettings(vibration::workbench::Window& window,const std::function<void(QDialog&)>& action){
    vibration::workbench::qa::interaction::tap(find<QPushButton>(window,"workbenchOptions"));
    bool seen=false;QString error;
    QTimer::singleShot(0,&window,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog){error="Settings did not open";return;}seen=true;
        try{check(dialog->objectName()=="workbenchSettingsDialog","actual new settings dialog");check(dialog->height()<=window.height(),"settings fits 600px client height");action(*dialog);}
        catch(const std::exception& e){error=QString::fromUtf8(e.what());dialog->reject();}
    });
    click(find<QPushButton>(window,"workbenchSettings"));check(seen&&error.isEmpty(),"settings action: "+error);
}
void fileAction(QPushButton* opener,const QString& target,const QString& filter,bool expectRejection){
    if(opener->objectName()=="workbenchExport")vibration::workbench::qa::interaction::tap(find<QPushButton>(*opener->window()->parentWidget(),"workbenchDataMenu"));
    bool seen=false,rejected=false;QString error,lastModal;QTimer poll;poll.setInterval(5);QElapsedTimer elapsed;elapsed.start();
    QObject::connect(&poll,&QTimer::timeout,[&]{
        if(auto* file=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())){
            lastModal="QFileDialog: "+file->windowTitle()+" selected="+file->selectedFiles().join('|');
            if(!seen){seen=true;if(!filter.isEmpty())file->selectNameFilter(filter);file->selectFile(target);dialogTrace.append(QJsonObject{{"target",target},{"title",file->windowTitle()},{"selected",file->selectedFiles().join('|')}});QTimer::singleShot(0,file,[file]{static_cast<QDialog*>(file)->accept();});}
        }else if(auto* message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){
            lastModal="QMessageBox: "+message->windowTitle()+" text="+message->text();
            dialogTrace.append(QJsonObject{{"message",message->text()},{"title",message->windowTitle()},{"buttons",int(message->standardButtons())}});
            if(auto* yes=message->button(QMessageBox::Yes))yes->click();
            else if(auto* save=message->button(QMessageBox::Save))save->click();
            else{rejected=true;if(auto* ok=message->button(QMessageBox::Ok))ok->click();else message->accept();}
        }
        if(elapsed.elapsed()>8000){error="File action deadline exceeded; "+lastModal;if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();}
    });
    poll.start();click(opener);poll.stop();check(seen&&error.isEmpty(),"actual file dialog completed: "+error);check(rejected==expectRejection,"source overwrite rejection expectation");
}
void loadProfile(vibration::workbench::Window& window,const QString& profile){
    withSettings(window,[&](QDialog& dialog){find<QTabWidget>(dialog,"workbenchSettingsTabs")->setCurrentIndex(1);QApplication::processEvents();fileAction(find<QPushButton>(dialog,"loadWorkbenchProfile"),profile,{},false);dialog.reject();});
}
void saveText(const QString& path,const std::string& data){QFile f(path);check(f.open(QIODevice::WriteOnly|QIODevice::NewOnly),"new QA profile only");check(f.write(data.data(),qint64(data.size()))==qint64(data.size()),"write complete QA profile");}
}
int main(int argc,char** argv){
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));QApplication app(argc,argv);
    for(const auto& fontPath:{"/mnt/c/Windows/Fonts/segoeui.ttf","/mnt/c/Windows/Fonts/seguisb.ttf","/mnt/c/Windows/Fonts/msyh.ttc","/mnt/c/Windows/Fonts/msyhbd.ttc"})if(QFileInfo::exists(fontPath))QFontDatabase::addApplicationFont(fontPath);
    QFont font;font.setFamilies({"Segoe UI","Microsoft YaHei UI","Microsoft YaHei","Noto Sans CJK SC","sans-serif"});font.setPixelSize(14);font.setWeight(QFont::Normal);font.setStyleStrategy(QFont::PreferAntialias);font.setHintingPreference(QFont::PreferDefaultHinting);app.setFont(font);
    QJsonObject report{{"schema","new-workbench-window-data-qa-v1"},{"origin","All inputs are generated synthetic software QA. An explicitly unmarked copy exercises user-declaration paths only; it is never mechanical evidence."}};
    try{
        check(argc==8,"current baseline invalid known-ppr hardlink evidence size arguments");
        const QString current=QString::fromLocal8Bit(argv[1]),baseline=QString::fromLocal8Bit(argv[2]),invalid=QString::fromLocal8Bit(argv[3]),known=QString::fromLocal8Bit(argv[4]),alias=QString::fromLocal8Bit(argv[5]);QDir out(QString::fromLocal8Bit(argv[6]));const auto size=QString::fromLatin1(argv[7]).split('x');
        const auto meta=QFileInfo(baseline).absolutePath()+"/"+QFileInfo(baseline).completeBaseName()+".meta.json";
        const auto baselineBefore=hash(baseline),metaBefore=hash(meta),currentBefore=hash(current);
        const auto analysis=vibration::analyze(vibration::load_capture(baseline.toStdString()),vibration::AnalysisConfig{});
        const auto draft=vibration::make_baseline_draft(analysis,"qa_setup","qa_reference_only");const auto profile=out.filePath("qa-reference.profile");saveText(profile,vibration::detection_profile_text(draft));
        vibration::workbench::Window window;window.resize(size[0].toInt(),size[1].toInt());window.show();QApplication::processEvents();window.openCapture(current);wait(window);
        loadProfile(window,profile);
        // This baseline was deliberately never passed to Window.openCapture.
        for(const auto& target:QStringList{baseline,meta,alias})fileAction(find<QPushButton>(window,"workbenchExport"),target,"JSON (*.json)",true);
        check(hash(baseline)==baselineBefore&&hash(meta)==metaBefore&&hash(current)==currentBefore,"unopened referenced BIN/metadata/hardlink and current input unchanged");
        auto missing=draft;missing.id="qa_missing_reference";missing.source_path=out.filePath("missing-reference.BIN").toStdString();const auto missingProfile=out.filePath("qa-missing.profile");saveText(missingProfile,vibration::detection_profile_text(missing));loadProfile(window,missingProfile);
        const auto missingMeta=out.filePath("missing-reference.meta.json");fileAction(find<QPushButton>(window,"workbenchExport"),missingMeta,"JSON (*.json)",true);check(!QFileInfo::exists(missingMeta),"reserved missing baseline metadata cannot be created by export");
        for(int kind:{2,1,0}){
            withSettings(window,[&](QDialog& dialog){auto* scroll=find<QScrollArea>(dialog,"analysisSettingsScroll");check(scroll->widgetResizable(),"analysis fields scroll without taller dialog");find<QDoubleSpinBox>(dialog,"analysisPpr")->setValue(1);auto* source=find<QComboBox>(dialog,"analysisSource");source->setCurrentIndex(kind);find<QCheckBox>(dialog,"analysisCalibration")->setChecked(kind==2);click(find<QPushButton>(dialog,"applyWorkbenchSettings"));});wait(window);
            const auto output=out.filePath(QString("source-%1.csv").arg(kind));fileAction(find<QPushButton>(window,"workbenchExport"),output,"CSV (*.csv)",false);
            const QString expected=kind==2?"vibration_confirmed":kind==1?"test_signal":"unknown";check(bytes(output).contains(("\"source_kind\",\"\",\""+expected+"\",\"\"").toUtf8()),"CSV source state follows actual configuration: "+expected);
            if(kind==2){const auto pprResult=out.filePath("ppr-explicit.json");fileAction(find<QPushButton>(window,"workbenchExport"),pprResult,"JSON (*.json)",false);const auto object=QJsonDocument::fromJson(bytes(pprResult)).object();check(object.value("configuration").toObject().value("ppr_override").toDouble()==1&&object.value("speed").toObject().value("ppr_source")=="explicit_user_override","unknown header PPR uses explicit confirmed override in actual analysis");}
        }
        withSettings(window,[&](QDialog& dialog){find<QDoubleSpinBox>(dialog,"analysisPpr")->setValue(0);click(find<QPushButton>(dialog,"applyWorkbenchSettings"));});wait(window);
        const auto unknownPpr=out.filePath("ppr-unknown.json");fileAction(find<QPushButton>(window,"workbenchExport"),unknownPpr,"JSON (*.json)",false);const auto unknown=QJsonDocument::fromJson(bytes(unknownPpr)).object();check(unknown.value("speed").toObject().value("ppr_source")=="unknown"&&unknown.value("speed").toObject().value("pulses_per_rev").isNull(),"zero override and unknown header preserve unknown RPM semantics");
        window.openCapture(invalid);wait(window);
        withSettings(window,[&](QDialog& dialog){auto* source=find<QComboBox>(dialog,"analysisSource");auto* calibrated=find<QCheckBox>(dialog,"analysisCalibration");check(!source->isEnabled()&&!calibrated->isEnabled()&&!calibrated->isChecked(),"invalid metadata locks source and calibration");auto* notice=find<QLabel>(dialog,"analysisSourceNotice");check(notice->text().contains(QStringLiteral("来源信息无效")),"invalid metadata explains repair before confirmation");find<QScrollArea>(dialog,"analysisSettingsScroll")->ensureWidgetVisible(notice);check(dialog.grab().save(out.filePath("invalid-source-settings.png")),"actual locked-source dialog screenshot");dialog.reject();});
        window.openCapture(known);wait(window);
        withSettings(window,[&](QDialog& dialog){auto* ppr=find<QDoubleSpinBox>(dialog,"analysisPpr");ppr->setValue(2);click(find<QPushButton>(dialog,"applyWorkbenchSettings"));check(dialog.isVisible()&&find<QLabel>(dialog,"analysisSettingsError")->text().contains(QStringLiteral("PPR")),"known header PPR conflict stays in dialog");ppr->setValue(0);click(find<QPushButton>(dialog,"applyWorkbenchSettings"));});wait(window);
        const auto final=out.filePath("ppr-file-default.json");fileAction(find<QPushButton>(window,"workbenchExport"),final,"JSON (*.json)",false);const auto json=QJsonDocument::fromJson(bytes(final)).object();check(json.value("configuration").toObject().value("ppr_override").isNull()&&json.value("speed").toObject().value("ppr_source")=="file_header","zero PPR restores file/unknown behavior");
        check(hash(baseline)==baselineBefore&&hash(meta)==metaBefore&&hash(current)==currentBefore,"all protected originals remain unchanged after all operations");
        window.setView(3);QApplication::processEvents();check(window.grab().save(out.filePath("actual-detection-page.png")),"actual production-style detection page render");
        report["baseline_sha256_before"]=baselineBefore;report["baseline_sha256_after"]=hash(baseline);report["metadata_sha256_before"]=metaBefore;report["metadata_sha256_after"]=hash(meta);report["result"]="passed";
    }catch(const std::exception& e){report["result"]="failed";report["error"]=QString::fromUtf8(e.what());}
    report["checks"]=checks;report["dialog_trace"]=dialogTrace;const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(data.constData(),1,std::size_t(data.size()),stdout);return report.value("result")=="passed"?0:1;
}
