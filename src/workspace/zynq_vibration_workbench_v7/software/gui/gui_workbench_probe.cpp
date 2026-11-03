#include "main_window.hpp"
#include "vibration/detection.hpp"
#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QAccessible>
#include <QBuffer>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QPointer>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTabBar>
#include <QTextBrowser>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#endif

namespace {
QJsonArray checks, geometry, timings;
int timeoutScale=1;
int deadlineMs(int baseMs) {return baseMs*timeoutScale;}
struct OperationTiming {
    QString operation;
    int baseDeadlineMs;
    QElapsedTimer elapsed;
    explicit OperationTiming(QString name,int baseMs=0):operation(std::move(name)),baseDeadlineMs(baseMs){elapsed.start();}
    ~OperationTiming(){
        timings.append(QJsonObject{{"operation",operation},{"elapsed_ms",double(elapsed.elapsed())},
            {"base_deadline_ms",baseDeadlineMs},{"deadline_ms",deadlineMs(baseDeadlineMs)},{"timeout_scale",timeoutScale}});
    }
};
void check(bool condition, const QString& name) {
    checks.append(QJsonObject{{"check",name},{"passed",condition}});
    if(!condition)throw std::runtime_error(name.toStdString());
}
QByteArray read(const QString& path) {QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
QString digest(const QByteArray& value) {return QString::fromLatin1(QCryptographicHash::hash(value,QCryptographicHash::Sha256).toHex());}
QJsonObject json(const QString& path) {return QJsonDocument::fromJson(read(path)).object();}
QPushButton* button(QWidget& owner,const char* id) {
    auto* result=owner.findChild<QPushButton*>(id);check(result,QStringLiteral("button exists: ")+id);return result;
}
void click(QPushButton* target) {
    check(target&&target->isVisible()&&target->isEnabled(),QStringLiteral("real visible enabled button: ")+(target?target->objectName():"null"));
    check(target->width()>=44&&target->height()>=44,QStringLiteral("44px touch target: ")+target->objectName());
    const QPointer<QPushButton> safeTarget(target);
    const QPointer<QWidget> originalWindow(target->window());
    const QPointer<QWidget> originalModal(QApplication::activeModalWidget());
    const bool hadModal=bool(originalModal);
    // QApplication's native pointer dispatcher gives the hit control mouse
    // focus before delivering events. sendEvent bypasses that outer dispatcher.
    target->setFocus(Qt::MouseFocusReason);
    if(!safeTarget)throw std::runtime_error("Mouse focus destroyed the test action target");
    const QPointF point(target->rect().center());
    QMouseEvent press(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(safeTarget.data(),&press);
    if(safeTarget)QApplication::sendEvent(safeTarget.data(),&release);
    // Closing a modal has already interrupted its owning event loop. A nested
    // Unix processEvents here can consume that wakeup before exec returns.
    const bool sameModal=hadModal?(originalModal&&originalModal->isVisible()&&QApplication::activeModalWidget()==originalModal.data()):!QApplication::activeModalWidget();
    if(safeTarget&&originalWindow&&originalWindow->isVisible()&&sameModal)QApplication::processEvents();
}
void wait(QApplication& app,vibration::gui::MainWindow& window) {
    QElapsedTimer timer;timer.start();while(window.isAnalyzing()&&timer.elapsed()<30000){app.processEvents();QThread::msleep(3);}
    app.processEvents();check(!window.isAnalyzing(),"asynchronous analysis completed");
}
void snapshot(QWidget& widget,const QDir& directory,const QString& name) {
    OperationTiming timing("snapshot:"+name);
    QApplication::processEvents();QThread::msleep(30);QApplication::processEvents();
    check(widget.grab().save(directory.filePath(name+".png")),"real Qt screenshot: "+name);
}
void bounds(QWidget& window,const QString& phase) {
    for(const auto* control:window.findChildren<QPushButton*>()){
        if(!control->isVisible()||control->window()!=&window)continue;
        const auto p=control->mapTo(&window,QPoint());
        geometry.append(QJsonObject{{"phase",phase},{"id",control->objectName()},{"x",p.x()},{"y",p.y()},
            {"width",control->width()},{"height",control->height()}});
        check(control->width()>=44&&control->height()>=44,"visible main-window control touch size: "+control->objectName());
        check(p.x()>=0&&p.y()>=0&&p.x()+control->width()<=window.width()&&p.y()+control->height()<=window.height(),
              "visible main-window control stays within window: "+control->objectName());
    }
}
void header(vibration::gui::MainWindow& window,const QString& phase,const QString& sourcePath) {
    auto* file=window.findChild<QLabel*>("currentFile");auto* source=window.findChild<QLabel*>("analysisBanner");
    auto* calibration=window.findChild<QLabel*>("calibrationState");auto* exportButton=button(window,"exportOptions");
    check(file&&source&&calibration,"header identity/source/calibration widgets exist");
    check(file->textFormat()==Qt::PlainText,"user filename is explicitly PlainText");
    const auto literal=QFileInfo(sourcePath).fileName().contains("<>&")?QStringLiteral("<>&"):QStringLiteral("&");
    check(file->text().contains(literal),"literal filename metacharacters remain visible");
    std::array<QWidget*,4> controls{{file,source,calibration,exportButton}};std::array<QRect,4> boxes;
    for(std::size_t index=0;index<controls.size();++index){
        const auto* control=controls[index];const auto origin=control->mapTo(&window,QPoint());boxes[index]=QRect(origin,control->size());
        check(control->isVisible()&&control->width()>0&&control->height()>0,"critical header item remains visible: "+control->objectName());
        check(window.rect().contains(boxes[index]),"critical header item remains inside native window: "+control->objectName());
        geometry.append(QJsonObject{{"phase",phase},{"id",control->objectName()},{"x",origin.x()},{"y",origin.y()},{"width",control->width()},{"height",control->height()}});
    }
    for(std::size_t first=0;first<boxes.size();++first)for(std::size_t second=first+1;second<boxes.size();++second)
        check(!boxes[first].intersects(boxes[second]),"long filename/profile cannot overlap header state/export controls");
}
// All modal operations are opened by mouse events on the actual visible app
// button. Timers only operate the resulting real dialog, never call business
// load/export/range functions in place of the user interaction.
void modal(QPushButton* opener,const QString& expected,const std::function<void(QDialog&)>& action) {
    OperationTiming timing("modal:"+expected,6000);
    bool seen=false;QString error;QPointer<QDialog> ownedDialog;QTimer watchdog;watchdog.setSingleShot(true);
    QObject::connect(&watchdog,&QTimer::timeout,[&]{error="Modal action timed out: "+expected;if(ownedDialog)ownedDialog->reject();});
    QTimer::singleShot(0,&watchdog,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog){error="Visible button did not open a real modal dialog: "+expected;return;}
        if(!expected.isEmpty()&&dialog->objectName()!=expected){error="Wrong modal dialog: "+dialog->objectName()+" expected "+expected;dialog->reject();return;}
        seen=true;ownedDialog=dialog;
        try {action(*dialog);}catch(const std::exception& exception){error=QString::fromUtf8(exception.what());if(ownedDialog)ownedDialog->reject();}
    });
    watchdog.start(deadlineMs(6000));click(opener);watchdog.stop();
    check(seen,"real modal opened: "+expected);check(error.isEmpty(),error.isEmpty()?"modal action completed: "+expected:error);
}
void chooseFile(QPushButton* opener,const QString& path,bool warningExpected=false) {
    OperationTiming timing("file:"+opener->objectName(),5000);
    bool seen=false,warning=false;QString error;QElapsedTimer elapsed;elapsed.start();QTimer poll;poll.setInterval(5);
    QObject::connect(&poll,&QTimer::timeout,[&]{
        if(auto* file=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())){
            if(!seen){seen=true;file->selectFile(path);QTimer::singleShot(0,file,[file]{static_cast<QDialog*>(file)->accept();});}
        }else if(auto* message=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())){
            // QFileDialog's overwrite prompt can use Warning, rather than
            // Question, for the controlled QA output file on repeat runs.
            const bool overwrite=(message->standardButtons()&QMessageBox::Yes)&&(message->standardButtons()&QMessageBox::No);
            const bool bad=!overwrite&&(message->icon()==QMessageBox::Warning||message->icon()==QMessageBox::Critical);
            if(bad){warning=true;if(!warningExpected)error="Unexpected app warning: "+message->text();}
            message->done(overwrite?QMessageBox::Yes:QMessageBox::Ok);
        }
        if(elapsed.elapsed()>deadlineMs(5000)){error="File-dialog action timed out";if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();}
    });
    poll.start();click(opener);poll.stop();
    check(seen,"real QFileDialog opened: "+opener->objectName());
    check(error.isEmpty(),error.isEmpty()?"real file action completed: "+opener->objectName():error);
    check(!warningExpected||warning,"invalid profile warning shown through actual load UI");
}
void keypad(QDoubleSpinBox* spin,const std::vector<const char*>& keys,bool cancel,bool invalid,const QDir& output,bool takeShot=false) {
    OperationTiming timing("keypad:"+spin->objectName(),5000);
    const auto before=spin->value();bool seen=false;QString error;QPointer<QDialog> ownedDialog;QTimer watchdog;watchdog.setSingleShot(true);
    QObject::connect(&watchdog,&QTimer::timeout,[&]{error="Numeric keypad timed out";if(ownedDialog)ownedDialog->reject();});
    auto* edit=spin->findChild<QLineEdit*>();check(edit&&edit->isVisible(),"actual range field line edit is touchable");
    const QPointer<QLineEdit> safeEdit(edit);
    const QPointer<QWidget> parentModal(QApplication::activeModalWidget());
    const QPointF point(edit->rect().center());QMouseEvent press(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QTimer::singleShot(0,&watchdog,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog||dialog->objectName()!="numericKeypadDialog"){error="Tapping time field did not open numeric keypad";if(dialog)dialog->reject();return;}
        seen=true;ownedDialog=dialog;
        try{
            auto* display=dialog->findChild<QLineEdit*>("numericValueDisplay");check(display&&display->isVisible(),"real numeric value display visible");
            check(button(*dialog,"numericKeySign")->isEnabled()==(spin->minimum()<0),"keypad sign follows field domain");
            click(button(*dialog,"numericKeyClear"));for(const auto* key:keys)click(button(*dialog,key));
            if(takeShot)snapshot(*dialog,output,"18-numeric-keypad");
            if(invalid){
                click(button(*dialog,"numericKeyConfirm"));auto* validation=dialog->findChild<QLabel*>("numericValidationError");
                check(dialog->isVisible()&&validation&&validation->isVisible()&&!validation->text().isEmpty(),"out-of-range keypad confirm stays open with visible error");
                check(spin->value()==before,"out-of-range keypad input does not alter field");click(button(*dialog,"numericKeyCancel"));
            }else click(button(*dialog,cancel?"numericKeyCancel":"numericKeyConfirm"));
        }catch(const std::exception& exception){error=QString::fromUtf8(exception.what());if(ownedDialog)ownedDialog->reject();}
    });
    watchdog.start(deadlineMs(5000));QApplication::sendEvent(safeEdit.data(),&press);
    if(safeEdit)QApplication::sendEvent(safeEdit.data(),&release);
    if(parentModal&&parentModal->isVisible()&&QApplication::activeModalWidget()==parentModal.data())QApplication::processEvents();
    watchdog.stop();
    check(seen,"tapping actual range field opens native numeric keypad");check(error.isEmpty(),error.isEmpty()?"numeric keypad touch interaction completed":error);
    if(cancel||invalid)check(spin->value()==before,"cancelled/rejected keypad input preserves prior value");
}
void exportThroughDialogs(QPushButton* opener,const char* format,const QString& path,const QDir& output,bool takeShot) {
    OperationTiming timing(QStringLiteral("export:")+format,5000);
    bool formatSeen=false,fileSeen=false;QString error;QElapsedTimer elapsed;elapsed.start();QTimer poll;poll.setInterval(5);
    QObject::connect(&poll,&QTimer::timeout,[&]{
        try{
            auto* active=QApplication::activeModalWidget();
            if(auto* file=qobject_cast<QFileDialog*>(active)){
                if(!fileSeen){fileSeen=true;file->selectFile(path);QTimer::singleShot(0,file,[file]{static_cast<QDialog*>(file)->accept();});}
            }else if(auto* message=qobject_cast<QMessageBox*>(active)){
                const bool overwrite=(message->standardButtons()&QMessageBox::Yes)&&(message->standardButtons()&QMessageBox::No);
                if(!overwrite&&(message->icon()==QMessageBox::Warning||message->icon()==QMessageBox::Critical))error="Unexpected export warning: "+message->text();
                message->done(overwrite?QMessageBox::Yes:QMessageBox::Ok);
            }else if(auto* dialog=qobject_cast<QDialog*>(active)){
                if(!formatSeen&&dialog->objectName()=="exportOptionsDialog"){
                    formatSeen=true;if(takeShot)snapshot(*dialog,output,"15-export-options");click(button(*dialog,format));
                }
            }
            if(elapsed.elapsed()>deadlineMs(5000)){error="Export UI sequence timed out";if(auto* dialog=qobject_cast<QDialog*>(active))dialog->reject();}
        }catch(const std::exception& exception){error=QString::fromUtf8(exception.what());if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();}
    });
    poll.start();click(opener);poll.stop();
    check(formatSeen,"visible top export opened real format dialog");check(fileSeen,"format choice opened real save-file dialog");
    check(error.isEmpty(),error.isEmpty()?QStringLiteral("actual export UI completed: ")+format:error);
}
}
int main(int argc,char** argv) {
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6,0,0)
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
#endif
    QApplication app(argc,argv);QFont font;font.setFamilies({"Noto Sans CJK SC","WenQuanYi Zen Hei","sans-serif"});font.setPixelSize(14);app.setFont(font);
    if(argc!=5)return 2;
    const auto source=QString::fromUtf8(argv[1]);const QDir output(QString::fromUtf8(argv[2]));QDir().mkpath(output.path());
    const auto dimensions=QString::fromUtf8(argv[3]).split('x');const auto invalidProfile=QString::fromUtf8(argv[4]);
    QJsonObject report;
    try{
        const auto scaleText=qgetenv("VIBRATION_GUI_TEST_TIMEOUT_SCALE");
        if(!scaleText.isEmpty()){
            bool valid=false;const auto requested=QString::fromLatin1(scaleText).toInt(&valid);
            if(!valid||requested<1||requested>10)throw std::runtime_error("VIBRATION_GUI_TEST_TIMEOUT_SCALE must be an integer from 1 to 10");
            timeoutScale=requested;
        }
        check(dimensions.size()==2,"requested native dimensions supplied");
        const auto sourceBefore=read(source);check(!sourceBefore.isEmpty(),"software QA input exists");
        vibration::AnalysisConfig config;config.signal_kind=vibration::SignalKind::VibrationConfirmed;config.calibration_confirmed=true;config.spectrum_max_hz=400;
        const std::string longId="example_experimental_"+std::string(107,'p');
        check(longId.size()==128,"long profile fixture uses legal 128 ASCII id");
        auto profile=vibration::make_baseline_draft(vibration::analyze_file(source.toUtf8().toStdString(),config),"software-qa-bench",longId);
        profile.rules={{"example_workbench_normal","normal",{{"ac_rms",1,"absolute",0,1000000}}}};
        const auto profilePath=output.filePath("example-experimental-workbench.profile");
        vibration::write_detection_text(profilePath.toUtf8().toStdString(),vibration::detection_profile_text(profile),{});
        const auto profileBefore=read(profilePath);
        vibration::gui::MainWindow window;window.resize(dimensions[0].toInt(),dimensions[1].toInt());window.show();app.processEvents();
        const std::array<const char*,5> nav{{"navFiles","navWaveform","navSpectrum","navSpeed","navDetection"}};
        std::set<QString> icons;
        for(std::size_t index=0;index<nav.size();++index){
            auto* target=button(window,nav[index]);
            const auto icon=target->grab().copy(12,(target->height()-20)/2,20,20).toImage();std::set<QRgb> colours;
            for(int y=0;y<icon.height();++y)for(int x=0;x<icon.width();++x)colours.insert(icon.pixel(x,y));
            check(colours.size()>1,QStringLiteral("navigation paints nonempty actual icon region: ")+nav[index]);
            const auto* accessible=QAccessible::queryAccessibleInterface(target);
            check(accessible&&!accessible->text(QAccessible::Name).isEmpty(),QStringLiteral("navigation exposes actual accessible name: ")+nav[index]);
            QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);icon.save(&buffer,"PNG");icons.insert(digest(png));
            click(target);check(window.findChild<QStackedWidget*>("workspacePages")->currentIndex()==int(index),QStringLiteral("real icon navigation opens intended page: ")+nav[index]);bounds(window,QString::fromLatin1(nav[index]));
        }
        check(icons.size()==5,"all five navigation icons are distinct");
        check(window.setDetectionSetupId("software-qa-bench"),"explicit software QA setup supplied");
        window.openCapture(source);check(window.isAnalyzing(),"opening record starts worker");
        check(!button(window,"exportOptions")->isEnabled(),"visible top export disabled during analysis");wait(app,window);
        check(button(window,"exportOptions")->isEnabled(),"visible top export enabled after valid result");
        header(window,"long_filename",source);
        for(const auto& page:std::array<std::pair<const char*,const char*>,3>{{{"navFiles","09-records"},{"navSpectrum","10-spectrum"},{"navSpeed","11-speed"}}}){
            click(button(window,page.first));bounds(window,QString::fromLatin1(page.first));snapshot(window,output,QString::fromLatin1(page.second));
        }
        QTemporaryDir scratch;check(scratch.isValid(),"isolated temporary comparison directory");QString error;
        const auto before=scratch.filePath("before.json"),after=scratch.filePath("after.json");
        check(window.saveResult(before,false,&error),"pre-range complete result snapshot");
        click(button(window,"navWaveform"));
        modal(button(window,"waveRangePicker"),"waveRangeDialog",[&](QDialog& dialog){
            snapshot(dialog,output,"12-range-dialog");
            auto* start=dialog.findChild<QDoubleSpinBox*>("rangeStart");auto* end=dialog.findChild<QDoubleSpinBox*>("rangeEnd");
            check(start&&end&&start->isVisible()&&end->isVisible(),"range inputs are real visible dialog controls");
            check(start->height()>=44&&end->height()>=44,"range input touch height");
            keypad(start,{"numericKey0","numericKeyDecimal","numericKey0","numericKey9","numericKeyBackspace","numericKey5"},false,false,output,true);
            check(std::abs(start->value()-.05)<1e-9,"touch digits, decimal, backspace and confirm enter exact start");
            keypad(end,{"numericKey0","numericKeyDecimal","numericKey0","numericKey5","numericKey1"},false,false,output);
            check(std::abs(end->value()-.051)<1e-9,"touch keypad enters exact end");
            keypad(start,{"numericKey0","numericKeyDecimal","numericKey0","numericKey6"},true,false,output);
            keypad(end,{"numericKey9","numericKeyDecimal","numericKey9","numericKey9"},false,true,output);
            click(button(dialog,"applyRange"));
        });
        for(const auto* name:{"waveCH1Plot","waveCH2Plot","waveCH3Plot","waveformPlot"}){
            auto* plot=window.findChild<vibration::gui::PlotWidget*>(name);check(plot&&plot->explicitXRange()&&std::abs(plot->explicitXRange()->x()-.05)<1e-9&&std::abs(plot->explicitXRange()->y()-.051)<1e-9,QStringLiteral("real range Apply synchronized plot: ")+name);
        }
        check(window.saveResult(after,false,&error)&&read(before)==read(after),"real range dialog preserves complete-record analysis and detection result");
        report.insert("range_before_sha256",digest(read(before)));report.insert("range_after_sha256",digest(read(after)));
        click(button(window,"navDetection"));
        modal(button(window,"manageDetectionConfig"),"detectionConfigDialog",[&](QDialog& dialog){
            chooseFile(button(dialog,"loadDetectionProfile"),profilePath);snapshot(dialog,output,"13-config-dialog");dialog.accept();
        });
        auto* tabs=window.findChild<QTabWidget*>("detectionTabs");check(tabs&&tabs->count()==3,"detection has exactly overview/features/conditions tabs");
        for(int index=0;index<3;++index){const std::array<const char*,3> names{{"detectionOverviewTab","detectionFeaturesTab","detectionConditionsTab"}};click(button(window,names[index]));check(tabs->currentIndex()==index,QStringLiteral("real detection tab: ")+names[index]);bounds(window,QString::fromLatin1(names[index]));}
        auto* evidence=window.findChild<QTextBrowser*>("detectionEvidence");check(evidence&&evidence->isVisible()&&!evidence->toPlainText().isEmpty(),"conditions tab shows actual full detection evidence");snapshot(window,output,"14-conditions");
        check(window.saveResult(after,false,&error),"loaded profile result exports");report.insert("loaded_profile",json(after).value("detection"));
        check(window.setDetectionMeasurementConditions(vibration::SignalKind::VibrationConfirmed,true),"software QA declaration path starts reanalysis");wait(app,window);
        header(window,"long_filename_and_128_ascii_profile",source);bounds(window,"long_profile_candidate");
        click(button(window,"detectionOverviewTab"));bounds(window,"long_profile_overview");snapshot(window,output,"17-long-profile-candidate");
        auto* summary=window.findChild<QLabel*>("detectionConfigSummary");
        check(summary&&summary->isVisible()&&summary->textFormat()==Qt::PlainText,"long profile summary is a visible PlainText label");
        check(summary->fontMetrics().horizontalAdvance(summary->text())<=summary->contentsRect().width(),"long profile summary text fits its actual label width");
        check(summary->toolTip().contains(QString::fromStdString(longId))&&summary->toolTip().contains("[draft]")&&summary->toolTip().contains(profilePath),"profile tooltip preserves complete id, status and input path");
        if(summary->fontMetrics().horizontalAdvance(QString::fromStdString(longId)+"  [draft]")>summary->contentsRect().width())
            check(summary->text().contains(QChar(0x2026)),"overwide profile identity uses explicit middle ellipsis instead of clipping");
        // Choose fresh per-run paths in the actual file dialogs. The same
        // stable evidence directory may contain prior QA output, while input
        // protection/atomic replacement remains covered by the unchanged tests.
        const auto uiBaseline=scratch.filePath("example-observed-ui-baseline.profile");
        modal(button(window,"manageDetectionConfig"),"detectionConfigDialog",[&](QDialog& dialog){
            click(button(dialog,"clearDetectionProfile"));check(window.saveResult(after,false,&error)&&json(after).value("detection").toObject().value("status")=="not_configured","real clear configuration removes active rules immediately");
            chooseFile(button(dialog,"loadDetectionProfile"),profilePath);chooseFile(button(dialog,"saveBaselineDraft"),uiBaseline);dialog.accept();
        });
        const auto draft=vibration::load_detection_profile(uiBaseline.toUtf8().toStdString());check(draft.rules.empty()&&draft.status=="draft","real baseline UI saves observed-only draft without invented normal rules");
        const auto jsonOutput=scratch.filePath("actual-ui-export.json"),csvOutput=scratch.filePath("actual-ui-export.csv");
        for(const auto& format:std::array<std::pair<const char*,QString>,2>{{{"exportResult",jsonOutput},{"exportCsv",csvOutput}}}){
            exportThroughDialogs(button(window,"exportOptions"),format.first,format.second,output,format.second==jsonOutput);
            check(!read(format.second).isEmpty(),QStringLiteral("real format dialog saved selected output: ")+format.first);
        }
        report.insert("actual_json",json(jsonOutput).value("detection"));report.insert("actual_csv",QString::fromUtf8(read(csvOutput)));
        for(const auto& path:{uiBaseline,jsonOutput,csvOutput}){
            const auto destination=output.filePath(QFileInfo(path).fileName());
            vibration::write_detection_text(destination.toUtf8().toStdString(),read(path).toStdString(),{source.toUtf8().toStdString(),profilePath.toUtf8().toStdString()});
            check(read(destination)==read(path),"retained actual UI output byte-identical: "+QFileInfo(path).fileName());
        }
        modal(button(window,"analysisParameters"),"analysisParametersDialog",[&](QDialog& dialog){
            check(dialog.windowTitle().contains(QStringLiteral("参数")),"actual parameter dialog opened");
            auto* parameters=dialog.findChild<QTabWidget*>("parameterTabs");auto* bar=parameters?parameters->findChild<QTabBar*>():nullptr;
            check(parameters&&parameters->count()==2&&bar&&bar->isVisible(),"both actual parameter tabs exist");
            for(int index=0;index<2;++index){
                const auto rect=bar->tabRect(index);check(rect.width()>=44&&rect.height()>=44,"parameter tab touch area");
                const QPointF point(rect.center());QMouseEvent press(QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease,point,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
                bar->setFocus(Qt::MouseFocusReason);QApplication::sendEvent(bar,&press);QApplication::sendEvent(bar,&release);QApplication::processEvents();
                check(parameters->currentIndex()==index,"actual parameter tab mouse action changes page");
                snapshot(dialog,output,index==0?"16-parameters":"19-parameters-source-calibration");
            }
            dialog.reject();
        });
        modal(button(window,"manageDetectionConfig"),"detectionConfigDialog",[&](QDialog& dialog){chooseFile(button(dialog,"loadDetectionProfile"),invalidProfile,true);dialog.accept();});
        check(window.saveResult(after,false,&error),"failed real profile-load state remains auditable");report.insert("invalid_profile",json(after).value("detection"));
        check(read(source)==sourceBefore&&read(profilePath)==profileBefore,"UI operations preserved source capture and loaded profile");
        check(!QFile::exists(source.left(source.size()-4)+".meta.json"),"UI did not create or overwrite reserved missing metadata");
        check(window.width()==dimensions[0].toInt()&&window.height()==dimensions[1].toInt(),"main window stayed at requested native dimensions");
        report.insert("result","passed");
    }catch(const std::exception& exception){report.insert("result","failed");report.insert("error",QString::fromUtf8(exception.what()));}
    report.insert("test_timeout",QJsonObject{{"environment","VIBRATION_GUI_TEST_TIMEOUT_SCALE"},{"scale",timeoutScale},
        {"minimum_scale",1},{"maximum_scale",10},{"modal_base_ms",6000},{"keypad_file_export_base_ms",5000},
        {"analysis_wait_ms",30000},{"scope","Test-harness wall-clock deadlines only; not a UI-performance acceptance claim"}});
    report.insert("timings",timings);
    const auto* dispatcher=QAbstractEventDispatcher::instance();
    report.insert("event_dispatcher",dispatcher?QString::fromLatin1(dispatcher->metaObject()->className()):QStringLiteral("none"));
    report.insert("checks",checks);report.insert("geometry",geometry);report.insert("scope","Software QA fixture only; no physical measurement, detection accuracy or optical acceptance claim");
    const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(data.constData(),1,std::size_t(data.size()),stdout);return report.value("result")=="passed"?0:1;
}
