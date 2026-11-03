#include "window.hpp"
#include "capture_page.hpp"
#include "candidate_panel.hpp"
#include "single_waveform_view.hpp"
#include "touch_directory_dialog.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <cstdio>

using namespace vibration::workbench;
namespace {
QJsonArray checks;
QString evidence, asynchronousFailure;
void events(int ms=20) { QElapsedTimer timer;timer.start();do{QApplication::processEvents();QThread::msleep(1);}while(timer.elapsed()<ms); }
void check(bool value,const QString& name) {checks.append(QJsonObject{{"check",name},{"passed",value}});if(!value)throw std::runtime_error(name.toStdString());}
template<class T> T* child(QObject& parent,const char* name) {auto* result=dynamic_cast<T*>(parent.findChild<QWidget*>(name));if(!result)throw std::runtime_error(std::string("Missing UI: ")+name);return result;}
void click(QWidget* widget,QPoint point=QPoint()) {
    for(auto* parent=widget->parentWidget();parent;parent=parent->parentWidget())if(auto* scroll=dynamic_cast<QScrollArea*>(parent))scroll->ensureWidgetVisible(widget,0,0);
    events();if(point.isNull())point=widget->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress,point,widget->mapToGlobal(point),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(widget,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,point,widget->mapToGlobal(point),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(widget,&release);events();
}
void text(QLineEdit* entry,const QString& value) {entry->setFocus();entry->selectAll();QKeyEvent key(QEvent::KeyPress,0,Qt::NoModifier,value);QApplication::sendEvent(entry,&key);events();}
void choose(QComboBox* combo,int index) {combo->setFocus();QKeyEvent home(QEvent::KeyPress,Qt::Key_Home,Qt::NoModifier);QApplication::sendEvent(combo,&home);for(int i=0;i<index;++i){QKeyEvent down(QEvent::KeyPress,Qt::Key_Down,Qt::NoModifier);QApplication::sendEvent(combo,&down);}events();}
void later(const std::function<void(QDialog&)>& task) {QTimer::singleShot(60,[task]{auto* dialog=dynamic_cast<QDialog*>(QApplication::activeModalWidget());try {if(!dialog)throw std::runtime_error("Expected active modal dialog");task(*dialog);}catch(const std::exception& error){asynchronousFailure=QString::fromUtf8(error.what());if(dialog)dialog->reject();}});}
void modal(QWidget* trigger,const std::function<void(QDialog&)>& task) {later(task);click(trigger);if(!asynchronousFailure.isEmpty())throw std::runtime_error(asynchronousFailure.toStdString());}
void shot(QWidget& widget,const QString& name) {events(60);check(widget.grab().save(evidence+"/"+name+".png"),"Rendered screenshot: "+name);}
void waitAnalysis(Window& window) {QElapsedTimer timer;timer.start();while(window.isAnalyzing()&&timer.elapsed()<10000)events();check(!window.isAnalyzing(),"Analysis completes within host timeout");events(60);}
void waitFor(const std::function<bool()>& ready,const QString& name) {QElapsedTimer timer;timer.start();while(!ready()&&timer.elapsed()<15000)events();check(ready(),name);}
bool sameSeries(const QVector<PlotSeries>& a,const QVector<PlotSeries>& b) {
    if(a.size()!=b.size())return false;
    for(int ch=0;ch<a.size();++ch){if(a[ch].name!=b[ch].name||a[ch].points.size()!=b[ch].points.size())return false;
        for(int i=0;i<a[ch].points.size();++i){const double ax=a[ch].points[i].x(),ay=a[ch].points[i].y(),bx=b[ch].points[i].x(),by=b[ch].points[i].y();
            if(std::memcmp(&ax,&bx,sizeof(double))||std::memcmp(&ay,&by,sizeof(double)))return false;}}
    return true;
}
void verifyAcceleration(const QVector<PlotSeries>& actual,const QVector<PlotSeries>& volts,const std::array<double,3>& sensitivities,const QString& context) {
    check(actual.size()==3&&volts.size()==3,context+" retains all three original channel sources");
    for(int ch=0;ch<3;++ch){bool correct=actual[ch].points.size()==volts[ch].points.size();
        for(int i=0;correct&&i<actual[ch].points.size();++i){const auto& a=actual[ch].points[i];const auto& v=volts[ch].points[i];
            // Fresh device configuration retains the signed -1 front-end gain.
            const double expected=-1000.*v.y()/sensitivities[ch];
            correct=a.x()==v.x()&&((std::isnan(a.y())&&std::isnan(v.y()))||(std::isfinite(a.y())&&std::abs(a.y()-expected)<=1e-9*std::max(1.,std::abs(expected))));}
        check(correct,context+QString(" CH%1 every point uses entered sensitivity and original signed gain").arg(ch+1));}
}
void sensitivityLayout(Window& window,const QString& context) {
    auto* dialog=child<QDialog>(window,"accelerationSensitivityDialog");
    if(!dialog->isVisible())qa::interaction::settingAction(window,"openAccelerationSensitivity");
    check(dialog->isVisible(),context+" sensitivity settings open through visible touch action");
    auto* bar=child<QWidget>(window,"accelerationSensitivitySettings");
    check(bar->isVisible()&&bar->findChildren<QDoubleSpinBox*>().size()==3&&bar->findChildren<QComboBox*>().isEmpty(),context+" exposes exactly three sensitivity inputs without a unit selector");
    for(const char* old:{"calibrationAmplitudeUnit","calibrationGain1","calibrationGain2","calibrationGain3","calibrationBias1","calibrationBias2","calibrationBias3"})check(!window.findChild<QWidget*>(old),context+" omits obsolete conversion control "+old);
    for(const char* name:{"accelerationSensitivity1","accelerationSensitivity2","accelerationSensitivity3","applyAccelerationSensitivity"}){
        auto* field=child<QWidget>(window,name);const QRect box(field->mapTo(dialog,QPoint()),field->size());
        check(field->isVisible()&&dialog->rect().contains(box)&&field->height()>=44,context+" control fits dialog and touch target: "+name);
        if(auto* spin=dynamic_cast<QDoubleSpinBox*>(field)){auto* entry=spin->findChild<QLineEdit*>();check(entry&&entry->fontMetrics().horizontalAdvance(entry->text())<=entry->contentsRect().width(),context+" full sensitivity number remains readable: "+name);}
    }
}
void numberPad(QWidget* field,const QString& invalid,const QString& valid,bool cancel=false) {
    auto* entry=field->findChild<QLineEdit*>();check(entry,"Numeric field exposes touch target");
    modal(entry,[&](QDialog& dialog){
        auto* input=child<QLineEdit>(dialog,"numberPadEntry");
        QPushButton *apply=nullptr,*cancelButton=nullptr;for(auto* button:dialog.findChildren<QPushButton*>()){if(button->text()==QStringLiteral("确定"))apply=button;if(button->text()==QStringLiteral("取消"))cancelButton=button;}
        check(apply&&cancelButton,"Touch keypad has confirm and cancel");
        text(input,invalid);click(apply);check(dialog.isVisible(),"Invalid touch numeric entry stays editable");
        text(input,valid);if(cancel)click(cancelButton);else click(apply);
    });
}
void loadFromBrowser(Window& window,const QString& filename) {
    qa::interaction::dataAction(window,"workbenchRecordsButton");auto* dialog=child<QDialog>(window,"workbenchRecordBrowser");auto* list=child<QListWidget>(*dialog,"workbenchRecords");
    QListWidgetItem* target=nullptr;for(int i=0;i<list->count();++i)if(QFileInfo(list->item(i)->data(Qt::UserRole).toString()).fileName()==filename)target=list->item(i);
    check(target,"Historical BIN is discoverable: "+filename);list->scrollToItem(target);events();click(list->viewport(),list->visualItemRect(target).center());click(child<QPushButton>(*dialog,"openSelectedRecord"));waitAnalysis(window);
}
void settings(Window& window,const std::function<void(QDialog&)>& task) {qa::interaction::tap(qa::interaction::button(window,"workbenchOptions"));modal(child<QPushButton>(window,"workbenchSettings"),task);waitAnalysis(window);}
void touchName(QDialog& chooser,const QString& name,bool cancel=false) {
    auto* field=child<QLineEdit>(chooser,"fileNameEdit");const auto initial=field->text();
    modal(field,[&](QDialog& keyboard){
        auto* input=child<QLineEdit>(keyboard,"touchKeyboardEntry");check(input->text()==initial,"File touch keyboard preserves existing default name");
        click(child<QPushButton>(keyboard,"touchKeyboardClear"));
        for(const auto character:name){const auto lower=character.toLower();if(character.isUpper())click(child<QPushButton>(keyboard,"touchKeyboardShift"));click(child<QPushButton>(keyboard,qPrintable("touchKey_"+QString(lower))));if(character.isUpper())click(child<QPushButton>(keyboard,"touchKeyboardShift"));}
        check(input->text()==name,"File name entered using only on-screen keyboard keys");
        if(name=="fresh-export.json"&&!cancel)shot(keyboard,"09a-touch-filename");
        click(child<QPushButton>(keyboard,cancel?"touchKeyboardCancel":"touchKeyboardApply"));
    });
    check(field->text()==(cancel?initial:name),cancel?"Cancel filename keyboard preserves chooser value":"Touch filename commits to file chooser");
}
}
int main(int argc,char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);qa::configureFonts(app);
    QJsonObject report{{"schema","v55-independent-fresh-user-ui-v1"},{"scope","Actual Qt dispatch and screenshots on host; synthetic records and isolated temporary storage only. No hardware, physical touch, SD throughput or sensor calibration acceptance."}};
    if(argc!=4)return 2;evidence=QString::fromUtf8(argv[2]);QDir().mkpath(evidence);QTemporaryDir storage;
    try {
        check(storage.isValid(),"Isolated temporary data root created");QDir().mkpath(storage.path()+"/vibration");
        check(QFile::copy(QString::fromUtf8(argv[1]),storage.path()+"/good.BIN"),"Copy own synthetic fixture");
        check(QFile::copy(QString::fromUtf8(argv[1]).replace(".BIN",".meta.json"),storage.path()+"/good.meta.json"),"Copy explicit synthetic source metadata");
        {QFile bad(storage.path()+"/broken.BIN");check(bad.open(QIODevice::WriteOnly),"Create own corrupt record fixture");bad.write(QByteArray("CAPDDR2\0",8));}
        Window window(nullptr,storage.path());child<CapturePage>(window,"capturePage")->setServiceCommand("/bin/false",{});window.setDirectory(storage.path());window.show();events(150);
        check(!child<QPushButton>(window,"workbenchStart")->isEnabled(),"Unavailable device cannot start capture");check(child<QPushButton>(window,"openRecord")->isEnabled(),"Offline data opening remains available");shot(window,"02-disconnected");
        qa::interaction::settingAction(window,"workbenchAcquire");auto* parameters=child<QDialog>(window,"workbenchCaptureDialog");shot(*parameters,"03-capture-parameters");
        auto* duration=child<QDoubleSpinBox>(*parameters,"captureDuration");numberPad(duration,"0","7",true);check(duration->value()==10,"Cancel numeric edit preserves duration");numberPad(duration,"1.5","7");check(duration->value()==7,"Touch duration recovery commits integer seconds");
        auto* samples=child<QLineEdit>(*parameters,"captureSamples");check(samples->text()=="350000","Duration updates per-channel sample count");
        modal(samples,[&](QDialog& dialog){text(child<QLineEdit>(dialog,"numberPadEntry"),"350001");click(child<QPushButton>(dialog,"touchKeyboardApply"));check(dialog.isVisible()&&!child<QLabel>(dialog,"touchKeyboardError")->text().isEmpty(),"Non-integral-second sample count rejected with readable error");text(child<QLineEdit>(dialog,"numberPadEntry"),"250000");click(child<QPushButton>(dialog,"touchKeyboardApply"));});
        check(duration->value()==5&&samples->text()=="250000","Corrected samples synchronize duration");
        auto* mode=child<QComboBox>(*parameters,"captureMode");choose(mode,1);check(!duration->isVisible()&&!samples->isVisible(),"Manual-stop mode hides timed-only fields");choose(mode,0);check(duration->isVisible()&&samples->isVisible(),"Return to timed mode preserves editable timing fields");
        const auto originalPath=child<QLineEdit>(*parameters,"captureDirectory")->text();
        modal(child<QPushButton>(*parameters,"captureBrowse"),[&](QDialog& directory){
            modal(child<QPushButton>(directory,"touchDirectoryNew"),[&](QDialog& keyboard){
                text(child<QLineEdit>(keyboard,"touchKeyboardEntry"),"../escape");click(child<QPushButton>(keyboard,"touchKeyboardApply"));check(keyboard.isVisible()&&!child<QLabel>(keyboard,"touchKeyboardError")->text().isEmpty(),"Invalid folder name rejected without navigating");
                click(child<QPushButton>(keyboard,"touchKeyboardClear"));for(const auto c:QString("run_27"))click(child<QPushButton>(keyboard,qPrintable("touchKey_"+QString(c))));click(child<QPushButton>(keyboard,"touchKeyboardApply"));
            });
            check(QFileInfo(storage.path()+"/vibration/run_27").isDir(),"Touch keys create explicitly named child folder");shot(directory,"04-named-folder");click(child<QPushButton>(directory,"touchDirectoryChoose"));
        });
        check(child<QLineEdit>(*parameters,"captureDirectory")->text().endsWith("/run_27"),"Selected capture save folder updates field");
        modal(child<QPushButton>(*parameters,"captureBrowse"),[&](QDialog& directory){click(child<QPushButton>(directory,"touchDirectoryUp"));click(child<QPushButton>(directory,"touchDirectoryCancel"));});
        check(child<QLineEdit>(*parameters,"captureDirectory")->text().endsWith("/run_27"),"Cancel directory browsing preserves save location");click(child<QPushButton>(*parameters,"captureParametersDone"));
        check(child<QLabel>(window,"capturePlanSummary")->toolTip().contains("250000"),"Main capture summary reflects saved parameters");
        qa::interaction::tap(qa::interaction::button(window,"workbenchDataMenu"));modal(child<QPushButton>(window,"openRecord"),[&](QDialog& dialog){check(dynamic_cast<QFileDialog*>(&dialog),"Open data shows an actual file chooser");dialog.reject();});check(child<QPushButton>(window,"openRecord")->isEnabled(),"Cancel file chooser leaves app ready");
        loadFromBrowser(window,"broken.BIN");check(child<QLabel>(window,"documentState")->text()==QStringLiteral("无法读取"),"Corrupt BIN gives recoverable read error");check(!child<QPushButton>(window,"workbenchExport")->isEnabled(),"Failed record cannot export stale analysis");
        loadFromBrowser(window,"good.BIN");check(child<QLabel>(window,"documentState")->text()==QStringLiteral("合成演示"),"Valid record recovers and identifies synthetic source");check(child<QPushButton>(window,"workbenchExport")->isEnabled(),"Valid record enables analysis export");
        qa::interaction::tap(qa::interaction::button(window,"workbenchDataMenu"));modal(child<QPushButton>(window,"openRecord"),[&](QDialog& dialog){touchName(dialog,"cancelled.BIN",true);touchName(dialog,"broken.BIN");click(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Open));});waitAnalysis(window);
        check(child<QLabel>(window,"documentState")->text()==QStringLiteral("无法读取"),"Touch-named invalid file produces recoverable core read error");
        qa::interaction::tap(qa::interaction::button(window,"workbenchDataMenu"));modal(child<QPushButton>(window,"openRecord"),[&](QDialog& dialog){touchName(dialog,"good.BIN");click(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Open));});waitAnalysis(window);
        check(child<QLabel>(window,"documentState")->text()==QStringLiteral("合成演示"),"Touch-named valid file recovers after invalid selection");
        auto* wave=child<SingleWaveformView>(window,"singleWaveformView");auto* plot=wave->plot();check(plot->series().size()==1,"Default displays one channel");
        const auto navigation=child<QLabel>(window,"plotNavigationNote")->text();check(navigation.contains(QStringLiteral("当前窗口"))&&navigation.contains(QStringLiteral("全记录 0.5 s"))&&!navigation.contains(QStringLiteral("完整记录")),"Partial initial view is labeled current window with metadata-derived full duration");
        click(child<QCheckBox>(window,"channelSelect2"));click(child<QCheckBox>(window,"channelSelect3"));check(plot->series().size()==3,"New user can overlay CH1 CH2 CH3");shot(window,"05-three-channels-volts");
        click(plot,plot->plotArea().center().toPoint());check(plot->cursorX().has_value()&&child<QLabel>(window,"waveCursorReadout")->text().contains("CH3"),"Waveform tap selects cursor and three channel values");
        const auto area=plot->plotArea();QWheelEvent wheel(area.center(),plot->mapToGlobal(area.center().toPoint()),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(plot,&wheel);events();const auto zoom=plot->explicitXRange();check(zoom&&zoom->y()-zoom->x()<plot->navigationBounds().y()-plot->navigationBounds().x(),"Wheel dispatch zooms the actual waveform");
        const auto from=area.center();const auto to=from+QPointF(55,0);QMouseEvent down(QEvent::MouseButtonPress,from,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(plot,&down);QMouseEvent move(QEvent::MouseMove,to,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(plot,&move);QMouseEvent up(QEvent::MouseButtonRelease,to,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(plot,&up);events();check(plot->explicitXRange()!=zoom,"Mouse drag pans the zoomed time range");
        QMouseEvent doubleClick(QEvent::MouseButtonDblClick,from,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(plot,&doubleClick);events();check(plot->explicitXRange()&&std::abs(plot->explicitXRange()->x()-plot->navigationBounds().x())<1e-8,"Double click restores full record start");
        click(child<QCheckBox>(window,"speedSelect"));check(plot->series().size()==1&&child<QLabel>(window,"singleWaveUnit")->text().contains("RPM",Qt::CaseInsensitive),"RPM trace uses its own axis and excludes vibration channels");
        modal(child<QPushButton>(window,"speedValueButton"),[&](QDialog& dialog){check(child<QLabel>(dialog,"speedValueReading")->text().contains("2000"),"Speed reading matches explicit fixture RPM");shot(dialog,"06-rpm-value");dialog.reject();});click(child<QCheckBox>(window,"speedSelect"));check(plot->series().size()==3,"Leaving speed restores channel overlay");
        const auto originalVolts=wave->sourceSeries();
        check(child<QPushButton>(window,"viewTab0")->text()==QStringLiteral("时域")&&child<QPushButton>(window,"viewTab1")->text()==QStringLiteral("频谱")&&child<QPushButton>(window,"viewTab2")->text()==QStringLiteral("加速度")&&child<QPushButton>(window,"viewTab3")->text()==QStringLiteral("检测"),"Four top-level tabs follow time spectrum acceleration detection order");
        check(child<QLabel>(window,"singleWaveUnit")->text().endsWith("V"),"Time-domain page initially retains original ADC volts");
        click(child<QPushButton>(window,"viewTab1"));auto* spectrum=child<SingleWaveformView>(window,"singleSpectrumView");const auto originalSpectrum=spectrum->sourceSeries();
        click(child<QPushButton>(window,"viewTab3"));const auto originalDetection=child<QTextBrowser>(window,"detectionFeatures")->toPlainText();
        click(child<QPushButton>(window,"viewTab2"));sensitivityLayout(window,"Acceleration page");
        const std::array<double,3> sensitivities{{2.,4.,8.}};
        bool allDefaultTwo=true;for(int ch=0;ch<3;++ch)allDefaultTwo&=std::abs(child<QDoubleSpinBox>(window,qPrintable(QString("accelerationSensitivity%1").arg(ch+1)))->value()-2.)<1e-12;
        check(!allDefaultTwo,"Fresh settings do not silently populate all channels with the 2.00 example");
        numberPad(child<QDoubleSpinBox>(window,"accelerationSensitivity1"),"0","2");
        numberPad(child<QDoubleSpinBox>(window,"accelerationSensitivity2"),"-1","4");
        numberPad(child<QDoubleSpinBox>(window,"accelerationSensitivity3"),"nan","8");
        click(child<QPushButton>(window,"applyAccelerationSensitivity"));
        verifyAcceleration(wave->sourceSeries(),originalVolts,sensitivities,"Acceleration page");
        const auto centered=plot->displayedYRange();check(plot->zeroCenteredY()&&centered.y()>0&&std::abs(centered.x()+centered.y())<1e-10,"Acceleration axis places zero at symmetric midpoint without changing samples");
        check(child<QLabel>(window,"singleWaveUnit")->text().contains("m/s"),"Acceleration page displays SI units");
        for(const auto& size:{QSize(1024,600),QSize(1280,720)}){window.resize(size);events(60);sensitivityLayout(window,QString("Acceleration %1x%2").arg(size.width()).arg(size.height()));shot(window,QString("07-acceleration-%1x%2").arg(size.width()).arg(size.height()));}
        numberPad(child<QDoubleSpinBox>(window,"accelerationSensitivity1"),"0","123",true);
        check(child<QDoubleSpinBox>(window,"accelerationSensitivity1")->value()==2,"Cancel sensitivity keypad preserves applied channel value");
        QFile saved(window.property("calibrationSettingsPath").toString());check(saved.open(QIODevice::ReadOnly),"Acceleration apply saves parameters at the declared data-root path");const auto persisted=QJsonDocument::fromJson(saved.readAll()).object();const auto channels=persisted.value("channel_calibration").toArray();
        check(channels.size()==3&&persisted.value("amplitude_unit")=="V"&&!persisted.contains("calibration_confirmed"),"Saved sensitivity parameters retain volts as original analysis unit and make no calibration claim");
        child<QDialog>(window,"accelerationSensitivityDialog")->reject();events();
        for(int ch=0;ch<3;++ch){const auto channel=channels[ch].toObject();check(channel.value("sensitivity_mv_per_m_s2").toDouble()==sensitivities[ch]&&channel.value("analog_gain").toDouble()==-1.&&channel.value("voltage_bias_v").toDouble()==0.,QString("Saved CH%1 sensitivity preserves signed gain -1 and zero bias").arg(ch+1));}
        if(auto* sensitivity=window.findChild<QDialog*>("accelerationSensitivityDialog"))sensitivity->reject();settings(window,[&](QDialog& dialog){auto* tabs=child<QTabWidget>(dialog,"workbenchSettingsTabs");check(tabs->count()==2&&!dialog.findChild<QWidget*>("calibrationAmplitudeUnit")&&!dialog.findChild<QWidget*>("calibrationSensitivity1"),"Analysis settings retain analysis/detection pages and omit channel-conversion page");numberPad(child<QDoubleSpinBox>(dialog,"analysisMaxHz"),"0","1500");dialog.reject();});
        check(child<QLabel>(window,"singleWaveUnit")->text().contains("m/s"),"Cancel analysis settings leaves acceleration page and applied sensitivity intact");
        click(child<QPushButton>(window,"viewTab0"));check(child<QLabel>(window,"singleWaveUnit")->text().endsWith("V")&&sameSeries(wave->sourceSeries(),originalVolts),"Returning to time page restores every original voltage point unchanged");
        check(!child<QWidget>(window,"accelerationSensitivitySettings")->isVisible(),"Sensitivity controls are shown only on acceleration page");
        click(child<QPushButton>(window,"viewTab1"));check(sameSeries(spectrum->sourceSeries(),originalSpectrum),"Sensitivity edits and cancelled analysis settings leave every original voltage FFT bin unchanged");
        click(child<QCheckBox>(window,"spectrumChannel2"));click(child<QCheckBox>(window,"spectrumChannel3"));check(spectrum->plot()->series().size()==3,"Spectrum permits independent three channel overlay");check(child<QLabel>(window,"plotNavigationNote")->text().contains(QStringLiteral("当前频段")),"Frequency range label distinguishes current band from full-record duration");click(spectrum->plot(),spectrum->plot()->plotArea().center().toPoint());check(child<QLabel>(window,"spectrumCursorReadout")->text().contains("Hz"),"Spectrum tap reports nearest bin");shot(window,"08-spectrum-volts");
        click(child<QPushButton>(window,"viewTab3"));check(child<QTextBrowser>(window,"detectionFeatures")->toPlainText()==originalDetection,"Acceleration sensitivities leave original voltage detection features unchanged");check(child<QTextBrowser>(window,"detectionEvidence")->toPlainText().contains(QStringLiteral("合成")),"Detection preserves synthetic evidence limitation");const auto candidate=QJsonDocument::fromJson(QByteArray::fromStdString(child<CandidatePanel>(window,"detectionPane")->candidateJson())).object();check(candidate.value("window").isNull()&&candidate.value("inference").toObject().value("classification").toString()=="unknown"&&child<QLabel>(window,"detectionState")->text().contains(QStringLiteral("等待")),"Short synthetic data waits without publishing a mechanical class");shot(window,"09-detection");
        const auto exportPath=storage.path()+"/fresh-export.json";
        qa::interaction::tap(qa::interaction::button(window,"workbenchDataMenu"));modal(child<QPushButton>(window,"workbenchExport"),[&](QDialog& dialog){auto* chooser=dynamic_cast<QFileDialog*>(&dialog);check(chooser,"Export uses actual Qt save chooser");check(child<QLineEdit>(dialog,"fileNameEdit")->text()=="analysis.json","Export preserves the suggested analysis filename");touchName(dialog,"cancelled.json",true);touchName(dialog,"fresh-export.json");report["export_selected_files"]=QJsonArray::fromStringList(chooser->selectedFiles());shot(dialog,"09b-export-chooser");auto* buttons=dialog.findChild<QDialogButtonBox*>();check(buttons&&buttons->button(QDialogButtonBox::Save),"Save chooser exposes confirmation");click(buttons->button(QDialogButtonBox::Save));});
        report["export_actual_files"]=QJsonArray::fromStringList(QDir(storage.path()).entryList(QDir::Files));report["export_status"]=child<QLabel>(window,"workbenchStatus")->text();shot(window,"09c-export-result");
        QByteArray exportedBytes;{QFile exported(exportPath);check(exported.open(QIODevice::ReadOnly),"Analysis JSON is saved through user action");exportedBytes=exported.readAll();const auto object=QJsonDocument::fromJson(exportedBytes).object();check(!object.isEmpty(),"Exported JSON parses after save");}
        qa::interaction::tap(qa::interaction::button(window,"workbenchDataMenu"));modal(child<QPushButton>(window,"workbenchExport"),[&](QDialog& dialog){touchName(dialog,"fresh-export.json");
            later([&](QDialog& confirmation){auto* message=dynamic_cast<QMessageBox*>(&confirmation);check(message&&message->button(QMessageBox::No),"Same-name save asks before overwriting");click(message->button(QMessageBox::No));});
            click(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save));check(dialog.isVisible(),"Reject overwrite leaves filename editable");click(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel));
        });
        {QFile exported(exportPath);check(exported.open(QIODevice::ReadOnly)&&exported.readAll()==exportedBytes,"Cancel same-name export preserves exact existing bytes");}
        {QFile exported(exportPath);check(exported.open(QIODevice::WriteOnly|QIODevice::Truncate)&&exported.write("own temporary overwrite sentinel")>0,"Prepare own temporary overwrite confirmation fixture");}
        qa::interaction::tap(qa::interaction::button(window,"workbenchDataMenu"));modal(child<QPushButton>(window,"workbenchExport"),[&](QDialog& dialog){touchName(dialog,"fresh-export.json");later([&](QDialog& confirmation){auto* message=dynamic_cast<QMessageBox*>(&confirmation);check(message&&message->button(QMessageBox::Yes),"Overwrite confirmation provides explicit yes action");click(message->button(QMessageBox::Yes));});click(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save));});
        {QFile exported(exportPath);check(exported.open(QIODevice::ReadOnly)&&exported.readAll()==exportedBytes,"Confirmed overwrite replaces only selected temporary output with analysis JSON");}
        check(window.close(),"Idle window closes cleanly");events();
        Window reopened(nullptr,storage.path());auto* capturePage=child<CapturePage>(reopened,"capturePage");capturePage->setServiceCommand("/bin/false",{});reopened.setDirectory(storage.path());reopened.show();events(100);loadFromBrowser(reopened,"good.BIN");check(child<QPushButton>(reopened,"viewTab0")->isChecked()&&child<QLabel>(reopened,"singleWaveUnit")->text().endsWith("V"),"Restart defaults to original voltage time page");
        const auto reopenedVolts=child<SingleWaveformView>(reopened,"singleWaveformView")->sourceSeries();click(child<QPushButton>(reopened,"viewTab2"));
        for(int ch=0;ch<3;++ch)check(child<QDoubleSpinBox>(reopened,qPrintable(QString("accelerationSensitivity%1").arg(ch+1)))->value()==sensitivities[ch],QString("Restart preserves independent CH%1 sensitivity").arg(ch+1));
        verifyAcceleration(child<SingleWaveformView>(reopened,"singleWaveformView")->sourceSeries(),reopenedVolts,sensitivities,"Restarted acceleration page");shot(reopened,"10-reopened-acceleration");click(child<QPushButton>(reopened,"viewTab0"));
        // The explicitly synthetic service uses only this temporary data root.
        capturePage->setServiceCommand(QString::fromUtf8(argv[3]),{"--test-fixture","--fixture-chunk-ms=2"});
        click(child<QPushButton>(reopened,"workbenchReconnect"));
        waitFor([&]{return child<QPushButton>(reopened,"workbenchStart")->isEnabled();},"Explicit synthetic stream service connects");
        qa::interaction::settingAction(reopened,"workbenchAcquire");auto* captureDialog=child<QDialog>(reopened,"workbenchCaptureDialog");
        numberPad(child<QDoubleSpinBox>(*captureDialog,"captureDuration"),"0","1");choose(child<QComboBox>(*captureDialog,"captureRate"),3);click(child<QPushButton>(*captureDialog,"captureParametersDone"));
        qa::interaction::condition(reopened,"normal");check(capturePage->nextRecordLabel()==RecordLabel::Normal,"Operator explicitly selects normal for synthetic organizer QA");
        click(child<QPushButton>(reopened,"workbenchStart"));waitFor([&]{return capturePage->property("captureActive").toBool();},"Timed capture starts via main control");check(!child<QPushButton>(reopened,"openRecord")->isEnabled(),"Active capture prohibits conflicting file loading");
        waitFor([&]{return !capturePage->property("captureActive").toBool()&&!reopened.isAnalyzing();},"Timed capture naturally saves and analyzes");const auto timed=capturePage->property("captureLatestSegment").toString();check(QFileInfo(timed).isFile()&&timed.startsWith(storage.path()+"/"),"Timed synthetic capture saves only inside temporary data root");shot(reopened,"11-timed-complete");
        qa::interaction::settingAction(reopened,"workbenchAcquire");choose(child<QComboBox>(*captureDialog,"captureMode"),1);click(child<QPushButton>(*captureDialog,"captureParametersDone"));click(child<QPushButton>(reopened,"workbenchStart"));waitFor([&]{return capturePage->property("captureActive").toBool()&&child<QPushButton>(reopened,"workbenchStart")->isEnabled();},"Continuous capture exposes main stop control");
        events(180);click(child<QPushButton>(reopened,"workbenchStart"));waitFor([&]{return !capturePage->property("captureActive").toBool()&&!reopened.isAnalyzing();},"Manual stop drains saves and returns ready");const auto manual=capturePage->property("captureLatestSegment").toString();check(QFileInfo(manual).isFile()&&manual!=timed&&manual.startsWith(storage.path()+"/"),"Manual stop retains its own final segment");shot(reopened,"12-manual-stop");
        click(child<QPushButton>(reopened,"workbenchStart"));waitFor([&]{return capturePage->property("captureActive").toBool()&&child<QPushButton>(reopened,"workbenchStart")->isEnabled();},"Capture can restart after manual stop");events(180);reopened.close();check(reopened.isVisible(),"Close while capturing defers until final save");waitFor([&]{return !reopened.isVisible();},"Active close completes after graceful synthetic stop");check(QFileInfo(capturePage->property("captureLatestSegment").toString()).isFile(),"Active close preserves final saved segment");report["passed"]=true;
    }catch(const std::exception& error){report["passed"]=false;report["error"]=QString::fromUtf8(error.what());}
    report["checks"]=checks;const auto bytes=QJsonDocument(report).toJson();QFile output(evidence+"/report.json");if(!output.open(QIODevice::WriteOnly))return 2;output.write(bytes);std::fwrite(bytes.data(),1,std::size_t(bytes.size()),stdout);return report["passed"].toBool()?0:1;
}
