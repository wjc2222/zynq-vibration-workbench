#include "window.hpp"
#include "controls.hpp"
#include "analysis_pane.hpp"
#include "candidate_panel.hpp"
#include "capture_page.hpp"
#include "calibration_settings.hpp"
#include "record_labels.hpp"
#include <fcntl.h>
#include <unistd.h>
#include "touch_directory_dialog.hpp"
#include "touch_keyboard.hpp"
#include <QCloseEvent>
#include <QTimer>
#include <QThread>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QStackedWidget>
#include <QPushButton>
#include <QListWidget>
#include <QButtonGroup>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QScrollArea>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>

namespace vibration::workbench {
namespace {
class LoadJob final : public QThread {
public:
    LoadJob(QString path,AnalysisConfig config,QObject* parent):QThread(parent),path_(std::move(path)),config_(config){}
    std::shared_ptr<const Capture> capture;
    std::unique_ptr<AnalysisResult> result;
    QString error;
private:
    void run() override {try{capture=std::make_shared<Capture>(load_capture(path_.toUtf8().toStdString()));result=std::make_unique<AnalysisResult>(analyze(*capture,config_));}catch(const std::exception& e){error=QString::fromUtf8(e.what());}catch(...){error=QStringLiteral("无法分析此记录");}}
    QString path_;AnalysisConfig config_;
};
QPushButton* action(const QString& text,Icon glyph,const char* name){auto* p=new TouchButton(text);p->setIcon(icon(glyph));p->setIconSize(QSize(20,20));p->setObjectName(name);p->setMinimumHeight(44);p->setAccessibleName(text);return p;}
QLabel* muted(const QString& text){auto* p=new QLabel(text);p->setTextFormat(Qt::PlainText);p->setProperty("role","muted");return p;}
QString csvCell(QString value){value.replace('"',"\"\"");return '"'+value+'"';}
QString recordSummary(const AnalysisResult& result){return QStringLiteral("%1 kHz  ·  %2 s  ·  %3 条%4%5").arg(result.header.sample_rate_hz/1000.,0,'g',4).arg(result.duration_s,0,'f',2).arg(result.header.completed_records).arg(result.source_metadata.synthetic?QStringLiteral("  ·  非实测数据"):QString()).arg(result.header.stream_segment?QStringLiteral("  ·  单个分段，非全程"):QString());}
class FileNameTouchInput final : public QObject {
public:
    FileNameTouchInput(QLineEdit* field,QFileDialog* dialog):QObject(dialog),field_(field),dialog_(dialog){field_->installEventFilter(this);}
protected:
    bool eventFilter(QObject* object,QEvent* event)override{
        if(object==field_&&event->type()==QEvent::MouseButtonPress&&static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton){
            bool accepted=false;const auto value=TouchKeyboardDialog::getText(dialog_,QStringLiteral("文件名或完整路径"),field_->text(),TouchKeyboardDialog::Mode::Text,&accepted);
            if(accepted)field_->setText(value);return true;
        }
        return QObject::eventFilter(object,event);
    }
private:
    QLineEdit* field_;QFileDialog* dialog_;
};
QString touchFile(QWidget* parent,const QString& title,const QString& initial,const QString& filters,bool save,QString* selectedFilter=nullptr){
    QFileDialog dialog(parent,title,initial,filters);dialog.setObjectName("workbenchFileDialog");dialog.setOption(QFileDialog::DontUseNativeDialog);
    dialog.setAcceptMode(save?QFileDialog::AcceptSave:QFileDialog::AcceptOpen);dialog.setFileMode(save?QFileDialog::AnyFile:QFileDialog::ExistingFile);
    if(selectedFilter&&!selectedFilter->isEmpty())dialog.selectNameFilter(*selectedFilter);
    dialog.setLabelText(QFileDialog::LookIn,QStringLiteral("所在目录"));dialog.setLabelText(QFileDialog::FileName,QStringLiteral("文件名"));dialog.setLabelText(QFileDialog::FileType,QStringLiteral("文件类型"));
    dialog.setLabelText(QFileDialog::Accept,save?QStringLiteral("保存"):QStringLiteral("打开"));dialog.setLabelText(QFileDialog::Reject,QStringLiteral("取消"));
    dialog.setStyleSheet("QFileDialog{background:white;}QFileDialog QTreeView::item,QFileDialog QListView::item{min-height:44px;}QFileDialog QToolButton{min-width:44px;min-height:44px;}QFileDialog QLineEdit,QFileDialog QComboBox{min-height:44px;font-size:16px;}");
    dialog.resize(std::min(860,parent?parent->width()-40:860),std::min(540,parent?parent->height()-36:540));
    if(auto* field=dialog.findChild<QLineEdit*>("fileNameEdit")){field->setAccessibleName(QStringLiteral("文件名或完整路径"));new FileNameTouchInput(field,&dialog);}
    if(dialog.exec()!=QDialog::Accepted||dialog.selectedFiles().isEmpty())return {};
    if(selectedFilter)*selectedFilter=dialog.selectedNameFilter();return dialog.selectedFiles().first();
}
}
Window::Window(QWidget* parent,const QString& dataRoot):QMainWindow(parent),dataRoot_(QDir(dataRoot).absolutePath()){
    config_.amplitude_unit=AmplitudeUnit::Voltage;
    setObjectName("newWorkbench");setWindowTitle(QStringLiteral("振动测量 · 板端工作台"));resize(1024,600);setMinimumSize(1024,600);setStyleSheet(stylesheet());
    auto* central=new QWidget;central->setObjectName("workbenchCentral");auto* shell=new QVBoxLayout(central);shell->setContentsMargins(12,8,12,8);shell->setSpacing(4);setCentralWidget(central);
    auto* heading=new QHBoxLayout;heading->setSpacing(10);
    auto* brand=new QLabel(QStringLiteral("振动测量"));brand->setObjectName("workbenchTitle");brand->setProperty("role","title");heading->addWidget(brand);heading->addStretch();
    device_=muted(QStringLiteral("板端设备 · 正在连接"));device_->setObjectName("workbenchDeviceState");heading->addWidget(device_);
    reconnect_=action(QStringLiteral("重新连接"),Icon::Refresh,"workbenchReconnect");reconnect_->setIcon(QIcon());heading->addWidget(reconnect_);shell->addLayout(heading);
    auto* actions=new QHBoxLayout;actions->setSpacing(8);
    start_=action(QStringLiteral("开始采集"),Icon::Add,"workbenchStart");start_->setProperty("primary",true);
    export_=action(QStringLiteral("导出分析"),Icon::Export,"workbenchExport");
    open_=action(QStringLiteral("打开数据"),Icon::Folder,"openRecord");
    acquire_=action(QStringLiteral("采集参数"),Icon::Settings,"workbenchAcquire");
    recordBrowser_=action(QStringLiteral("SD 记录"),Icon::Folder,"workbenchRecordsButton");
    for(auto* button:{start_,export_,open_,acquire_,recordBrowser_}){button->setIcon(QIcon());button->setMinimumWidth(0);}
    start_->setFixedWidth(132);actions->addWidget(start_);
    dataMenuButton_=action(QStringLiteral("数据"),Icon::Folder,"workbenchDataMenu");dataMenuButton_->setIcon(QIcon());dataMenuButton_->setFixedWidth(92);
    optionsButton_=action(QStringLiteral("设置"),Icon::Settings,"workbenchOptions");optionsButton_->setIcon(QIcon());optionsButton_->setFixedWidth(92);
    shell->addLayout(actions);
    auto* statusRow=new QHBoxLayout;statusRow->setSpacing(16);
    state_=muted(QStringLiteral("正在连接设备 · 可打开已有 BIN 文件分析"));state_->setObjectName("documentState");state_->setMinimumWidth(0);state_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);statusRow->addWidget(state_);
    plan_=new FileTitle;plan_->setObjectName("capturePlanSummary");plan_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);statusRow->addWidget(plan_,1);
    // Status belongs to the single footer; plan details live in capture settings.
    delete statusRow->takeAt(1);delete statusRow->takeAt(0);delete statusRow;
    tabs_=new QFrame;tabs_->setObjectName("workbenchTabs");auto* tabRow=new QHBoxLayout(tabs_);tabRow->setContentsMargins(0,0,0,0);tabRow->setSpacing(4);auto* views=new QButtonGroup(this);views->setExclusive(true);
    acquisitionTab_=new TouchButton(QStringLiteral("采集"));acquisitionTab_->setObjectName("pageAcquisition");acquisitionTab_->setProperty("tab",true);acquisitionTab_->setCheckable(true);acquisitionTab_->setFixedSize(90,44);views->addButton(acquisitionTab_);tabRow->addWidget(acquisitionTab_);connect(acquisitionTab_,&QPushButton::clicked,this,[this]{setView(lastPlotView_);});
    viewButtons_[3]=new TouchButton(QStringLiteral("检测"));viewButtons_[3]->setObjectName("viewTab3");viewButtons_[3]->setProperty("tab",true);viewButtons_[3]->setCheckable(true);viewButtons_[3]->setFixedSize(90,44);views->addButton(viewButtons_[3]);tabRow->addWidget(viewButtons_[3]);connect(viewButtons_[3],&QPushButton::clicked,this,[this]{setView(3);});tabRow->addStretch();
    // Main page navigation shares the heading. Plot mode and the explicit
    // next-record label share one tools row, leaving the graph the full height.
    heading->insertWidget(1,tabs_);heading->setStretch(1,0);delete tabRow->takeAt(tabRow->count()-1);tabs_->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed);
    captureLabelControls_=new QWidget;auto* labelRow=new QHBoxLayout(captureLabelControls_);labelRow->setContentsMargins(0,0,0,0);labelRow->setSpacing(0);
    captureLabelChoice_=new ChoiceField;captureLabelChoice_->setObjectName("nextCaptureCondition");captureLabelChoice_->setAccessibleName(QStringLiteral("下次采集工况"));captureLabelChoice_->setFixedSize(220,46);captureLabelChoice_->setProperty("labelRequired",true);captureLabelChoice_->addItem(QStringLiteral("采集工况：请选择"),QString());labelRow->addWidget(captureLabelChoice_);actions->addWidget(captureLabelControls_);
    const std::array<RecordLabel,5> kinds{RecordLabel::Unlabeled,RecordLabel::Normal,RecordLabel::Looseness,RecordLabel::Misalignment,RecordLabel::Imbalance};
    for(auto label:kinds)captureLabelChoice_->addItem(QStringLiteral("采集工况：")+(label==RecordLabel::Unlabeled?QStringLiteral("待检测"):recordLabelText(label)),recordLabelKey(label));
    connect(captureLabelChoice_,QOverload<int>::of(&QComboBox::activated),this,[this](int index){
        if(captureActive_||capturePage_->property("captureActive").toBool())return;
        RecordLabel label=RecordLabel::Unlabeled;
        const bool selected=recordLabelFromKey(captureLabelChoice_->itemData(index).toString(),&label);
        if(!capturePage_->setNextRecordLabel(label))return;
        // The placeholder clears an earlier choice. Explicit "待检测" has a
        // valid key, while the placeholder requires a new operator selection.
        setProperty("nextCaptureLabel",selected?QVariant(recordLabelKey(label)):QVariant());
        captureLabelChoice_->setProperty("labelRequired",!selected);
        captureLabelChoice_->style()->unpolish(captureLabelChoice_);
        captureLabelChoice_->style()->polish(captureLabelChoice_);
        captureLabelChoice_->update();
        if(selected&&state_->text()==QStringLiteral("先选择工况标签，未知数据选择“待检测”"))setDocumentState({});
    });
    curveControls_=new QWidget;curveControls_->setObjectName("captureCurveControls");auto* curveRow=new QHBoxLayout(curveControls_);curveRow->setContentsMargins(0,0,0,0);curveRow->setSpacing(6);auto* modes=new QButtonGroup(this);modes->setExclusive(true);
    const QStringList names{QStringLiteral("时域"),QStringLiteral("频谱"),QStringLiteral("加速度")};
    for(int i=0;i<3;++i){auto* b=new TouchButton(names[i]);b->setObjectName(QString("viewTab%1").arg(i));b->setAccessibleName(names[i]);b->setCheckable(true);b->setProperty("tab",true);b->setFixedSize(i==2?88:76,46);modes->addButton(b);viewButtons_[i]=b;curveRow->addWidget(b);connect(b,&QPushButton::clicked,this,[this,i]{setView(i);});}
    orderMarkers_=new QCheckBox(QStringLiteral("转频标记"));orderMarkers_->setObjectName("showOrderMarkers");orderMarkers_->setMinimumHeight(44);orderMarkers_->setToolTip(QStringLiteral("1X为实测rpm÷60；2X、3X为其倍数。标记不会改变实测频谱。"));
    sensitivityButton_=action(QStringLiteral("加速度灵敏度"),Icon::Settings,"openAccelerationSensitivity");sensitivityButton_->setIcon(QIcon());
    settings_=action(QStringLiteral("频谱 / 分析参数"),Icon::Settings,"workbenchSettings");settings_->setIcon(QIcon());
    actions->addWidget(curveControls_);actions->addStretch();actions->addWidget(dataMenuButton_);actions->addWidget(optionsButton_);
    const auto menuDialog=[this](const QString& title,const char* name){auto* dialog=new QDialog(this);dialog->setObjectName(name);dialog->setWindowTitle(title);dialog->setModal(true);dialog->setFixedWidth(420);auto* box=new QVBoxLayout(dialog);box->setContentsMargins(20,18,20,18);box->setSpacing(12);auto* heading=new QLabel(title);heading->setProperty("role","title");box->addWidget(heading);return dialog;};
    dataMenuDialog_=menuDialog(QStringLiteral("数据"),"workbenchDataDialog");auto* dataLayout=qobject_cast<QVBoxLayout*>(dataMenuDialog_->layout());for(auto* entry:{open_,recordBrowser_,export_})dataLayout->addWidget(entry);auto* dataClose=action(QStringLiteral("返回"),Icon::Close,"closeDataMenu");dataClose->setIcon(QIcon());dataLayout->addWidget(dataClose);connect(dataClose,&QPushButton::clicked,dataMenuDialog_,&QDialog::reject);
    optionsDialog_=menuDialog(QStringLiteral("设置"),"workbenchOptionsDialog");auto* optionLayout=qobject_cast<QVBoxLayout*>(optionsDialog_->layout());for(auto* entry:{acquire_,settings_,sensitivityButton_})optionLayout->addWidget(entry);auto* reconnectMenu=action(QStringLiteral("重新连接设备"),Icon::Refresh,"settingsReconnect");reconnectMenu->setIcon(QIcon());optionLayout->addWidget(reconnectMenu);connect(reconnectMenu,&QPushButton::clicked,this,[this]{optionsDialog_->accept();capturePage_->reconnect();});auto* optionsClose=action(QStringLiteral("返回"),Icon::Close,"closeOptionsMenu");optionsClose->setIcon(QIcon());optionLayout->addWidget(optionsClose);connect(optionsClose,&QPushButton::clicked,optionsDialog_,&QDialog::reject);
    connect(dataMenuButton_,&QPushButton::clicked,this,[this]{dataMenuDialog_->adjustSize();dataMenuDialog_->show();dataMenuDialog_->raise();});connect(optionsButton_,&QPushButton::clicked,this,[this,reconnectMenu]{reconnectMenu->setEnabled(!captureActive_&&!capturePage_->property("captureActive").toBool());optionsDialog_->adjustSize();optionsDialog_->show();optionsDialog_->raise();});
    sensitivityDialog_=new QDialog(this);sensitivityDialog_->setObjectName("accelerationSensitivityDialog");sensitivityDialog_->setWindowTitle(QStringLiteral("三通道灵敏度"));sensitivityDialog_->setModal(true);sensitivityDialog_->resize(970,220);
    auto* sensitivityLayout=new QVBoxLayout(sensitivityDialog_);sensitivityLayout->setContentsMargins(12,12,12,12);sensitivityLayout->setSpacing(10);
    auto* sensitivityNote=new QLabel(QStringLiteral("分别输入 CH1、CH2、CH3 灵敏度，单位 mV/(m/s²)。参数换算不代表已完成实测标定。"));sensitivityNote->setWordWrap(true);sensitivityLayout->addWidget(sensitivityNote);
    sensitivity_=new SensitivitySettings(config_);sensitivityLayout->addWidget(sensitivity_);
    auto* sensitivityStatus=new QLabel;sensitivityStatus->setObjectName("sensitivityDialogStatus");sensitivityStatus->setWordWrap(true);sensitivityLayout->addWidget(sensitivityStatus);
    auto* sensitivityClose=new QDialogButtonBox(QDialogButtonBox::Close);sensitivityClose->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));sensitivityLayout->addWidget(sensitivityClose);connect(sensitivityClose,&QDialogButtonBox::rejected,sensitivityDialog_,&QDialog::reject);
    connect(sensitivityButton_,&QPushButton::clicked,this,[this]{optionsDialog_->accept();sensitivityDialog_->resize(std::min(970,width()-32),220);sensitivityDialog_->show();sensitivityDialog_->raise();sensitivityDialog_->activateWindow();});
    pages_=new QStackedWidget;pages_->setObjectName("newPages");shell->addWidget(pages_,1);
    analysis_=new AnalysisPane;pages_->addWidget(analysis_);detectionPage_=new CandidatePanel;detectionPage_->setStorageRoot(dataRoot_);pages_->addWidget(detectionPage_);
    if(auto* selector=analysis_->findChild<QScrollArea*>("spectrumSelectorScroll"))if(auto* tools=qobject_cast<QVBoxLayout*>(selector->widget()->layout()))tools->insertWidget(tools->count()-1,orderMarkers_);
    connect(orderMarkers_,&QCheckBox::toggled,this,[this](bool checked){analysis_->setOrderMarkersVisible(checked);});
    auto* footer=new QHBoxLayout;footer->setSpacing(12);
    title_=new FileTitle;title_->setObjectName("documentTitle");title_->setFullText(QStringLiteral("来源 —"));title_->setMaximumWidth(260);footer->addWidget(title_,1);state_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);footer->addWidget(state_,2);
    footnote_=muted(QStringLiteral("就绪"));footnote_->setObjectName("workbenchStatus");footnote_->setMinimumWidth(0);footnote_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);footnote_->setFixedHeight(24);footnote_->hide();footer->addWidget(footnote_,2);plan_->setMaximumWidth(310);footer->addWidget(plan_,1);shell->addLayout(footer);

    captureDialog_=new QDialog(this);captureDialog_->setObjectName("workbenchCaptureDialog");captureDialog_->setWindowTitle(QStringLiteral("采集参数"));captureDialog_->setModal(true);captureDialog_->resize(760,536);
    captureDialog_->setStyleSheet("QDialog#workbenchCaptureDialog{background:#ffffff;border:1px solid #8d9bad;border-radius:6px;}");
    auto* captureLayout=new QVBoxLayout(captureDialog_);captureLayout->setContentsMargins(1,1,1,12);captureLayout->setSpacing(0);
    capturePage_=new CapturePage([]{return new NumberField;},nullptr,dataRoot_);capturePage_->setParameterOnly(true);captureLayout->addWidget(capturePage_,1);
    auto* captureButtons=new QDialogButtonBox(QDialogButtonBox::Ok);captureButtons->setContentsMargins(16,0,16,0);captureButtons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));captureButtons->button(QDialogButtonBox::Ok)->setObjectName("captureParametersDone");captureLayout->addWidget(captureButtons);
    connect(captureButtons,&QDialogButtonBox::accepted,captureDialog_,&QDialog::accept);connect(captureDialog_,&QDialog::finished,this,[this]{syncCaptureControls();});

    recordDialog_=new QDialog(this);recordDialog_->setObjectName("workbenchRecordBrowser");recordDialog_->setWindowTitle(QStringLiteral("SD 记录"));recordDialog_->setModal(true);recordDialog_->resize(780,520);
    auto* recordLayout=new QVBoxLayout(recordDialog_);recordLayout->setContentsMargins(18,16,18,16);recordLayout->setSpacing(10);
    auto* folderRow=new QHBoxLayout;folderTitle_=new FileTitle;folderTitle_->setObjectName("recordRootTitle");folderRow->addWidget(folderTitle_,1);
    folder_=action(QStringLiteral("选择目录"),Icon::Folder,"recordFolder");folder_->setIcon(QIcon());folderRow->addWidget(folder_);
    auto* refresh=action(QStringLiteral("刷新"),Icon::Refresh,"refreshRecords");refresh->setIcon(QIcon());folderRow->addWidget(refresh);recordLayout->addLayout(folderRow);
    auto* filterRow=new QHBoxLayout;filterRow->addWidget(new QLabel(QStringLiteral("工况")));recordFilter_=new QComboBox;recordFilter_->setObjectName("recordLabelFilter");recordFilter_->setMinimumHeight(44);recordFilter_->addItem(QStringLiteral("全部记录"),"all");for(auto label:kinds)if(label!=RecordLabel::Unlabeled)recordFilter_->addItem(recordLabelText(label),recordLabelKey(label));recordFilter_->addItem(QStringLiteral("未标注"),"unlabeled");filterRow->addWidget(recordFilter_,1);recordLayout->addLayout(filterRow);
    records_=new QListWidget;records_->setObjectName("workbenchRecords");records_->setTextElideMode(Qt::ElideMiddle);records_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);records_->setSelectionMode(QAbstractItemView::SingleSelection);recordLayout->addWidget(records_,1);
    auto* annotationRow=new QHBoxLayout;annotationRow->addWidget(new QLabel(QStringLiteral("人工标注")));recordLabelEditor_=new QComboBox;recordLabelEditor_->setObjectName("recordLabelEditor");recordLabelEditor_->setMinimumHeight(44);recordLabelEditor_->addItem(QStringLiteral("未标注"),"unlabeled");for(auto label:kinds)if(label!=RecordLabel::Unlabeled)recordLabelEditor_->addItem(recordLabelText(label),recordLabelKey(label));annotationRow->addWidget(recordLabelEditor_,1);applyRecordLabel_=action(QStringLiteral("保存标注"),Icon::Export,"saveRecordLabel");applyRecordLabel_->setIcon(QIcon());annotationRow->addWidget(applyRecordLabel_);addReferenceButton_=action(QStringLiteral("加入参考库"),Icon::Add,"addRecordToReferenceLibrary");addReferenceButton_->setIcon(QIcon());annotationRow->addWidget(addReferenceButton_);recordLayout->addLayout(annotationRow);
    recordAnnotationStatus_=new QLabel;recordAnnotationStatus_->setObjectName("recordAnnotationStatus");recordAnnotationStatus_->setTextFormat(Qt::PlainText);recordAnnotationStatus_->setWordWrap(true);recordLayout->addWidget(recordAnnotationStatus_);
    auto* recordButtons=new QDialogButtonBox(QDialogButtonBox::Open|QDialogButtonBox::Close);recordButtons->button(QDialogButtonBox::Open)->setText(QStringLiteral("打开数据"));recordButtons->button(QDialogButtonBox::Open)->setObjectName("openSelectedRecord");recordButtons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));recordLayout->addWidget(recordButtons);
    const auto openSelected=[this]{auto* item=records_->currentItem();if(!item||captureActive_||isAnalyzing())return;const auto path=item->data(Qt::UserRole).toString();if(path.isEmpty())return;recordDialog_->accept();openCapture(path);};
    connect(recordButtons->button(QDialogButtonBox::Open),&QPushButton::clicked,this,openSelected);connect(records_,&QListWidget::itemDoubleClicked,this,[openSelected](QListWidgetItem*){openSelected();});connect(recordButtons,&QDialogButtonBox::rejected,recordDialog_,&QDialog::reject);
    connect(refresh,&QPushButton::clicked,this,&Window::refreshRecords);connect(folder_,&QPushButton::clicked,this,&Window::chooseFolder);
    connect(recordFilter_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{refreshRecords();});connect(records_,&QListWidget::currentRowChanged,this,[this]{updateRecordAnnotation();});connect(applyRecordLabel_,&QPushButton::clicked,this,&Window::saveSelectedRecordLabel);connect(addReferenceButton_,&QPushButton::clicked,this,&Window::addSelectedReference);
    connect(acquire_,&QPushButton::clicked,this,[this]{optionsDialog_->accept();showAcquisition();});connect(recordBrowser_,&QPushButton::clicked,this,[this]{dataMenuDialog_->accept();showRecords();});connect(open_,&QPushButton::clicked,this,[this]{dataMenuDialog_->accept();chooseRecord();});
    connect(start_,&QPushButton::clicked,this,[this]{if(captureActive_||capturePage_->property("captureActive").toBool()){cancelRequested_=true;setDocumentState(QStringLiteral("正在停止并保存末段"));capturePage_->cancelCurrent();syncCaptureControls();}else startLabeledCapture();});connect(reconnect_,&QPushButton::clicked,capturePage_,&CapturePage::reconnect);
    connect(capturePage_,&CapturePage::recordLabelSaved,this,[this](const QString&){if(recordDialog_->isVisible())refreshRecords();});connect(capturePage_,&CapturePage::recordLabelSaveFailed,this,[this](const QString& path,const QString& reason){setFootnote(QStringLiteral("工况标注未保存：")+reason);footnote_->setToolTip(path);setProperty("recordLabelError",reason);});
    connect(settings_,&QPushButton::clicked,this,[this]{optionsDialog_->accept();settings();});connect(export_,&QPushButton::clicked,this,[this]{dataMenuDialog_->accept();exportResult();});
    connect(capturePage_,&CapturePage::captureReady,this,&Window::openCapture);
    connect(capturePage_,&CapturePage::liveFrame,this,&Window::liveFrame);
    connect(capturePage_,&CapturePage::committedSegmentReady,this,[this](const QString& path){detectionPage_->acceptSegment(path,config_);});
    connect(capturePage_,&CapturePage::previewBatchStarted,this,[this]{analysis_->beginLiveBatch();});
    connect(capturePage_,&CapturePage::previewBatchFinished,this,[this]{analysis_->endLiveBatch();});
    connect(capturePage_,&CapturePage::captureState,this,&Window::captureState);
    connect(capturePage_,&CapturePage::controlsChanged,this,&Window::syncCaptureControls);
    connect(capturePage_,&CapturePage::statusChanged,this,[this](const QString& status){const bool active=capturePage_->property("captureActive").toBool();if(!isAnalyzing()&&(path_.isEmpty()||active))setDocumentState(status);if(active)setFootnote(capturePage_->progressSummary());});
    analysis_->statusChanged=[this](const QString& text){setFootnote(text);};
    loadCalibrationSettings();config_.amplitude_unit=AmplitudeUnit::Voltage;sensitivity_->setConfig(config_);analysis_->setAnalysisConfig(config_);
    sensitivity_->applyRequested=[this]{
        if(isAnalyzing())return;
        auto candidate=config_;QString error;
        if(!sensitivity_->applyTo(candidate,&error)||!saveCalibrationSettings(candidate,&error)){setFootnote(error);sensitivityDialog_->findChild<QLabel*>("sensitivityDialogStatus")->setText(error);return;}
        config_=candidate;sensitivity_->setConfig(config_);analysis_->setAnalysisConfig(config_);if(capture_)detectionPage_->setCapture(capture_,config_);
        setFootnote(result_?recordSummary(*result_)+QStringLiteral("  ·  灵敏度已保存"):QStringLiteral("灵敏度已保存"));sensitivityDialog_->findChild<QLabel*>("sensitivityDialogStatus")->setText(QStringLiteral("灵敏度已保存并应用。"));
    };
    setView(0);refreshRecords();setBusy(false);QTimer::singleShot(0,capturePage_,&CapturePage::ensureConnected);
}
void Window::setDocumentState(const QString& text){state_->setText(text);state_->setVisible(!text.isEmpty()&&text!=QStringLiteral("设备就绪")&&text!=QStringLiteral("就绪")&&text!=QStringLiteral("等待数据"));}
void Window::setFootnote(const QString& text){footnote_->setText(text);footnote_->setVisible(!text.isEmpty()&&text!=QStringLiteral("设备就绪")&&text!=QStringLiteral("就绪")&&text!=QStringLiteral("等待数据"));}
Window::~Window(){disconnect(capturePage_,nullptr,this,nullptr);if(worker_){disconnect(worker_,nullptr,this,nullptr);worker_->wait();}analysis_->clear();detectionPage_->clear();}
void Window::closeEvent(QCloseEvent* event){if(captureActive_||capturePage_->property("captureActive").toBool()){closeAfterStop_=true;event->ignore();capturePage_->cancelCurrent();QTimer::singleShot(100,this,[this]{close();});return;}QMainWindow::closeEvent(event);}
bool Window::isAnalyzing() const{return worker_!=nullptr;}
void Window::setView(int view){if(view<0||view>3)return;viewButtons_[view]->setChecked(true);acquisitionTab_->setChecked(view!=3);curveControls_->setVisible(view!=3);orderMarkers_->setVisible(view==1);sensitivityButton_->setVisible(true);if(view==3){pages_->setCurrentWidget(detectionPage_);}else{lastPlotView_=view;analysis_->setAccelerationMode(view==2);analysis_->setView(view==2?0:view);pages_->setCurrentWidget(analysis_);}setProperty("majorPage",view==3?"detection":"acquisition");title_->setFullText(captureActive_||analysis_->isLive()?QStringLiteral("实时采集"):path_.isEmpty()?QStringLiteral("来源 —"):QFileInfo(path_).fileName());}
void Window::startLabeledCapture(){if(!property("nextCaptureLabel").isValid()){setDocumentState(QStringLiteral("先选择工况标签，未知数据选择“待检测”"));setView(lastPlotView_);return;}setProperty("recordLabelError",QString());capturePage_->startCapture();}
void Window::showAcquisition(){if(captureActive_||capturePage_->property("captureActive").toBool()||isAnalyzing())return;captureDialog_->resize(std::min(760,width()-40),std::min(536,height()-36));captureDialog_->show();captureDialog_->raise();captureDialog_->activateWindow();}
void Window::chooseRecord(){if(captureActive_||isAnalyzing())return;const auto file=touchFile(this,QStringLiteral("打开采集记录"),directory_,QStringLiteral("采集记录 (*.BIN *.bin)"),false);if(!file.isEmpty())openCapture(file);}
void Window::chooseFolder(){if(captureActive_||isAnalyzing())return;const auto path=TouchDirectoryDialog::getExistingDirectory(this,directory_.isEmpty()?capturePage_->outputDirectory():directory_,dataRoot_);if(!path.isEmpty())setDirectory(path);}
void Window::setDirectory(const QString& path){directory_=QFileInfo(path).absoluteFilePath();recordRoot_=directory_;refreshRecords();}
void Window::showRecords(){if(captureActive_||capturePage_->property("captureActive").toBool()||isAnalyzing())return;refreshRecords();recordDialog_->resize(std::min(780,width()-40),std::min(520,height()-36));recordDialog_->show();recordDialog_->raise();recordDialog_->activateWindow();}
void Window::refreshRecords(){
    records_->clear();const auto requestedRoot=recordRoot_.isEmpty()?capturePage_->outputDirectory():recordRoot_;const auto root=QDir(requestedRoot).canonicalPath();
    folderTitle_->setFullText(requestedRoot);folderTitle_->setToolTip(requestedRoot);QFileInfoList candidates;
    // Read-only, bounded-depth browsing. Do not traverse symbolic links or
    // mutate records; all selected files still pass the normal core loader.
    std::function<void(const QString&,int)> scan=[&](const QString& directory,int depth){
        if(candidates.size()>=2000)return;
        const auto entries=QDir(directory).entryInfoList(QDir::Files|QDir::Dirs|QDir::Readable|QDir::NoDotAndDotDot|QDir::NoSymLinks,QDir::Time);
        for(const auto& e:entries){if(e.isSymLink())continue;if(e.isDir()){if(depth<2)scan(e.absoluteFilePath(),depth+1);continue;}if(e.suffix().compare("BIN",Qt::CaseInsensitive))continue;
            const auto canonical=e.canonicalFilePath();const auto prefix=root.endsWith('/')?root:root+'/';if(canonical.isEmpty()||!canonical.startsWith(prefix))continue;
            QFile file(canonical);if(!file.open(QIODevice::ReadOnly))continue;const auto magic=file.read(8);if(magic!=QByteArray("CAPDDR2\0",8)&&magic!="CAPSTRM1")continue;candidates.append(QFileInfo(canonical));if(candidates.size()>=2000)return;
        }
    };
    if(!root.isEmpty())scan(root,0);
    std::sort(candidates.begin(),candidates.end(),[](const QFileInfo& a,const QFileInfo& b){return a.lastModified()>b.lastModified();});
    int count=0;const auto filter=recordFilter_->currentData().toString();for(const auto& e:candidates){if(count>=500)break;RecordLabelAnnotation annotation;QString annotationError;const bool valid=loadRecordLabel(e.absoluteFilePath(),&annotation,&annotationError);if(filter!="all"&&(!valid||recordLabelKey(annotation.label)!=filter))continue;const auto name=QDir(root).relativeFilePath(e.absoluteFilePath());const auto badge=valid?recordLabelText(annotation.label):QStringLiteral("标注异常");auto* item=new QListWidgetItem(QStringLiteral("[%1] %2\n%3 KiB · %4").arg(badge,name).arg(e.size()/1024.,0,'f',1).arg(e.lastModified().toString("yyyy-MM-dd HH:mm")),records_);item->setData(Qt::UserRole,e.absoluteFilePath());item->setToolTip(e.absoluteFilePath()+(valid?QString():"\n"+annotationError));item->setSizeHint(QSize(0,62));if(e.absoluteFilePath()==path_)records_->setCurrentItem(item);++count;}
    if(!count){auto* empty=new QListWidgetItem(root.isEmpty()?QStringLiteral("目录尚不可用，请选择数据目录"):QStringLiteral("没有此工况的记录"),records_);empty->setFlags(Qt::NoItemFlags);}updateRecordAnnotation();
}
void Window::updateRecordAnnotation(){auto* item=records_->currentItem();const auto path=item?item->data(Qt::UserRole).toString():QString();const bool selected=!path.isEmpty()&&!captureActive_&&!isAnalyzing();applyRecordLabel_->setEnabled(selected);recordLabelEditor_->setEnabled(selected);addReferenceButton_->setEnabled(false);recordAnnotationStatus_->clear();if(!selected)return;RecordLabelAnnotation a;QString error;if(!loadRecordLabel(path,&a,&error)){recordAnnotationStatus_->setText(QStringLiteral("标注读取失败：")+error);applyRecordLabel_->setEnabled(false);return;}recordLabelEditor_->setCurrentIndex(recordLabelEditor_->findData(recordLabelKey(a.label)));recordAnnotationStatus_->setText(a.synthetic?QStringLiteral("合成测试记录，不可加入机械参考库"):QStringLiteral("人工标注：%1").arg(recordLabelText(a.label)));addReferenceButton_->setEnabled(!a.synthetic&&a.label!=RecordLabel::Unlabeled);}
void Window::saveSelectedRecordLabel(){auto* item=records_->currentItem();if(!item||captureActive_||isAnalyzing())return;const auto path=item->data(Qt::UserRole).toString();RecordLabel label;if(path.isEmpty()||!recordLabelFromKey(recordLabelEditor_->currentData().toString(),&label))return;QString error;if(!saveRecordLabel(path,label,{},nullptr,&error)){recordAnnotationStatus_->setText(QStringLiteral("标注未保存：")+error);return;}refreshRecords();for(int i=0;i<records_->count();++i)if(records_->item(i)->data(Qt::UserRole).toString()==path)records_->setCurrentRow(i);recordAnnotationStatus_->setText(QStringLiteral("已保存人工标注：%1").arg(recordLabelText(label)));}
void Window::addSelectedReference(){auto* item=records_->currentItem();if(!item||captureActive_||isAnalyzing())return;const auto path=item->data(Qt::UserRole).toString();RecordLabelAnnotation a;QString error;if(path.isEmpty()||!loadRecordLabel(path,&a,&error)||a.label==RecordLabel::Unlabeled||a.synthetic){recordAnnotationStatus_->setText(error.isEmpty()?QStringLiteral("请先为真实记录保存工况标注"):error);return;}recordDialog_->accept();setView(3);detectionPage_->showReferenceLibrary();detectionPage_->addReferenceFile(path,recordLabelKey(a.label));}
void Window::openCapture(const QString& path){if(isAnalyzing()||captureActive_)return;if(analysis_->isLive())analysis_->endLive();liveJob_.clear();path_=QFileInfo(path).absoluteFilePath();directory_=QFileInfo(path_).absolutePath();if(!protectedHistory_.contains(path_))protectedHistory_.append(path_);config_.signal_kind=SignalKind::Unknown;config_.calibration_confirmed=false;refreshRecords();analyze();setView(viewButtons_[3]->isChecked()?3:lastPlotView_);}
void Window::analyze(){
    if(path_.isEmpty()||isAnalyzing())return;analysis_->clear();detectionPage_->clear();result_.reset();capture_.reset();setDocumentState(QStringLiteral("正在分析"));setFootnote(QStringLiteral("读取并校验记录"));
    auto* task=new LoadJob(path_,config_,this);worker_=task;setBusy(true);
    connect(task,&QThread::finished,this,[this,task]{worker_=nullptr;if(task->result){capture_=std::move(task->capture);result_=std::move(task->result);config_=result_->config;analysis_->setResult(capture_,result_.get());updateDetection();detectionPage_->setCapture(capture_,config_);
            const bool synthetic=result_->source_metadata.synthetic;setDocumentState(synthetic?QStringLiteral("合成演示"):config_.calibration_confirmed?QStringLiteral("已标定"):QStringLiteral("未标定"));
            state_->setStyleSheet("color:#344255;font-size:16px;");
            setFootnote(recordSummary(*result_));
        }else{setDocumentState(QStringLiteral("无法读取"));setFootnote(task->error);footnote_->setToolTip(task->error);updateDetection();}setBusy(false);task->deleteLater();if(!pendingCompletedPath_.isEmpty()){const auto path=pendingCompletedPath_;pendingCompletedPath_.clear();openCapture(path);}else if(!pendingLiveFrames_.isEmpty()){const auto frames=std::move(pendingLiveFrames_);pendingLiveFrames_={};analysis_->beginLiveBatch();for(const auto& frame:frames)liveFrame(frame);analysis_->endLiveBatch();}});task->start();
}
void Window::setBusy(bool busy){capturePage_->setAnalysisBusy(busy);syncCaptureControls();}
void Window::syncCaptureControls(){
    const bool active=captureActive_||capturePage_->property("captureActive").toBool(),busy=isAnalyzing();
    for(auto* button:{settings_,open_,folder_,acquire_,recordBrowser_})button->setEnabled(!busy&&!active);
    sensitivity_->setEnabled(!busy);sensitivityButton_->setEnabled(!busy);
    captureLabelControls_->setEnabled(!active&&!busy);
    for(int item=1;item<captureLabelChoice_->count();++item){RecordLabel label;if(recordLabelFromKey(captureLabelChoice_->itemData(item).toString(),&label)){const auto text=label==RecordLabel::Unlabeled?QStringLiteral("待检测"):recordLabelText(label);captureLabelChoice_->setItemText(item,active&&item==captureLabelChoice_->currentIndex()?QStringLiteral("本次：%1 · 锁定").arg(text):QStringLiteral("采集工况：")+text);}}
    records_->setEnabled(!busy&&!active);viewButtons_[3]->setEnabled(true);export_->setEnabled(!busy&&!active&&bool(result_));
    start_->setEnabled(active?(capturePage_->property("captureCanStop").toBool()&&!cancelRequested_):(!busy&&capturePage_->property("captureCanStart").toBool()));start_->setText(active?(cancelRequested_?QStringLiteral("正在停止…"):QStringLiteral("停止采集")):QStringLiteral("开始采集"));reconnect_->setEnabled(!active);dataMenuButton_->setEnabled(!busy&&!active);optionsButton_->setEnabled(!busy);
    const bool connected=capturePage_->property("captureAvailable").toBool();device_->setText(connected?QStringLiteral("设备已连接"):QStringLiteral("设备离线"));reconnect_->setVisible(!connected);const auto fullPlan=capturePage_->planSummary();const auto planParts=fullPlan.split(QStringLiteral(" · "));plan_->setFullText(planParts.size()>=2?planParts.first()+QStringLiteral(" · ")+QString(planParts[1]).replace(QStringLiteral("持续采集"),QStringLiteral("持续")).remove(QStringLiteral("定时 ")):fullPlan);plan_->setToolTip(fullPlan);acquire_->setToolTip(fullPlan);
    if(active&&captureDialog_->isVisible())captureDialog_->accept();
}
void Window::liveFrame(const QJsonObject& frame){
    if(!captureActive_&&!liveJob_.isEmpty())return;
    if(previewQuarantined_)return;
    if(isAnalyzing()){
        pendingLiveFrames_.append(frame);
        quint64 records=0,bytes=0;
        for(const auto& queued:pendingLiveFrames_){records+=quint64(std::max(0,queued.value("record_count").toInt()));bytes+=8192+2*quint64(queued.value("raw_base64").toString().size());}
        const quint64 limit=quint64(std::clamp(frame.value("sample_rate_hz").toInt(),5000,50000))*2;
        while(!pendingLiveFrames_.isEmpty()&&(pendingLiveFrames_.size()>64||records>limit||bytes>16777216)){
            const auto& oldest=pendingLiveFrames_.first();const auto removed=quint64(std::max(0,oldest.value("record_count").toInt()));
            records-=removed;bytes-=8192+2*quint64(oldest.value("raw_base64").toString().size());pendingLiveFrames_.removeFirst();
            setProperty("deferredPreviewDroppedRecords",property("deferredPreviewDroppedRecords").toULongLong()+removed);
        }
        return;
    }
    const auto job=frame.value("job_id").toString();if(job.isEmpty()||(!liveJob_.isEmpty()&&job!=liveJob_))return;
    QString error;const bool beginning=!analysis_->isLive();
    if(beginning)analysis_->setAnalysisConfig(config_);
    const bool accepted=beginning?analysis_->beginLive(frame,true,&error):analysis_->acceptLiveFrame(frame,&error);
    if(!accepted){setDocumentState(QStringLiteral("预览暂停"));setFootnote(error);return;}
    if(state_->text()==QStringLiteral("预览帧已拒绝")){state_->setToolTip({});setFootnote(QStringLiteral("实时预览已恢复；完整采集继续"));footnote_->setToolTip({});}
    if(beginning){liveJob_=job;captureActive_=true;result_.reset();capture_.reset();path_.clear();records_->clearSelection();detectionPage_->clear();setFootnote(QStringLiteral("实时预览 · %1 kHz").arg(frame.value("sample_rate_hz").toDouble()/1000.,0,'g',4));setView(viewButtons_[3]->isChecked()?3:lastPlotView_);}
    const auto amplitudeState=config_.amplitude_unit==AmplitudeUnit::Voltage?QStringLiteral("ADC电压 · V"):QStringLiteral("%1 · %2").arg(QString::fromStdString(amplitude_unit_symbol(config_)),config_.calibration_confirmed?QStringLiteral("已标定"):QStringLiteral("参数换算，未标定"));
    setDocumentState(cancelRequested_?(capturePage_->property("captureStreaming").toBool()?QStringLiteral("正在停止并保存末段"):QStringLiteral("正在取消")):frame.value("synthetic").toBool()?QStringLiteral("实时 · 合成自检"):QStringLiteral("实时 · %1").arg(amplitudeState));
    detectionPage_->acceptLive(frame,config_);
    start_->setEnabled(!cancelRequested_);if(beginning)setBusy(false);
}
void Window::captureState(const QJsonObject& state){
    const auto job=state.value("job_id").toString();const auto phase=state.value("state").toString();
    if(!QStringList{"preparing","running","validating","saving","stopping","completed","stopped","failed","cancelled"}.contains(phase))return;
    if(phase=="preparing"&&job!=liveJob_){if(analysis_->isLive())analysis_->endLive();liveJob_=job;pendingLiveFrames_={};cancelRequested_=false;previewQuarantined_=false;state_->setToolTip({});footnote_->setToolTip({});}
    if(!liveJob_.isEmpty()&&!job.isEmpty()&&job!=liveJob_)return;
    if(state.value("preview_only").toBool()){
        if(!captureActive_)return;
        const auto reason=state.value("preview_error").toString();
        if(state.contains("preview_available")&&!state.value("preview_available").toBool()){
            previewQuarantined_=true;pendingLiveFrames_={};
            analysis_->endLive(QStringLiteral("预览已隔离；完整采集继续"));detectionPage_->invalidateLive(QStringLiteral("预览输入已隔离；完整采集保存继续。"));
            setDocumentState(QStringLiteral("预览已隔离"));
        }else setDocumentState(QStringLiteral("预览帧已拒绝"));
        state_->setToolTip(reason);setFootnote(QStringLiteral("%1；完整采集继续").arg(reason));footnote_->setToolTip(reason);
        return;
    }
    const bool wasActive=captureActive_;
    captureActive_=phase=="preparing"||phase=="running"||phase=="validating"||phase=="saving"||phase=="stopping";
    if(phase=="stopping"){cancelRequested_=true;setDocumentState(QStringLiteral("正在停止并保存末段"));}
    if(captureActive_&&!wasActive)detectionPage_->clear();
    if(!captureActive_)cancelRequested_=false;
    if(captureActive_&&!wasActive)setView(viewButtons_[3]->isChecked()?3:lastPlotView_);setBusy(isAnalyzing());
    if(phase=="failed"||phase=="cancelled"){
        const QString outcome=state.value("connection_lost").toBool()?QStringLiteral("连接中断，设备状态未知"):phase=="failed"?QStringLiteral("采集失败"):QStringLiteral("采集已取消");
        pendingLiveFrames_={};analysis_->endLive(outcome);detectionPage_->invalidateLive(outcome);
        setDocumentState(outcome);title_->setFullText(QStringLiteral("采集预览"));
    }else if(phase=="completed"||phase=="stopped"){
        pendingLiveFrames_={};analysis_->endLive(phase=="stopped"?QStringLiteral("已停止并保存"):QStringLiteral("采集完成"));
        const auto path=state.value("output_path").toString();if(!path.isEmpty()){if(isAnalyzing())pendingCompletedPath_=path;else openCapture(path);}
    }
    if(!captureActive_&&closeAfterStop_)QTimer::singleShot(0,this,[this]{close();});
}
void Window::updateDetection(){detection_=DetectionResult{};detection_.setup_id=setupId_.toUtf8().toStdString();detection_.amplitude_unit=amplitude_unit_symbol(result_?result_->config:config_);detection_.baseline_amplitude_unit.clear();if(!profileError_.isEmpty()){detection_.status="invalid_profile";detection_.limitations.push_back(profileError_.toStdString());}else if(!result_)detection_.status="no_record";else if(!profile_)detection_.status="not_configured";else try{detection_=evaluate_detection(*result_,*profile_,setupId_.toUtf8().toStdString());}catch(const std::exception& e){detection_.status="invalid_profile";detection_.limitations.push_back(e.what());}detectionPage_->setResult(result_.get(),&detection_);}
std::vector<std::string> Window::protectedPaths() const{
    std::vector<std::string> paths;
    const auto protectCapture=[&](const QString& file){
        if(file.isEmpty())return;
        paths.push_back(file.toUtf8().toStdString());
        const QFileInfo info(file);
        // Protect the reserved sidecar even when it does not yet exist.
        paths.push_back((info.absolutePath()+"/"+info.completeBaseName()+".meta.json").toUtf8().toStdString());
        paths.push_back(recordLabelSidecarPath(file).toUtf8().toStdString());
    };
    for(const auto& file:protectedHistory_)protectCapture(file);
    if(!profilePath_.isEmpty())paths.push_back(profilePath_.toUtf8().toStdString());
    for(const auto& reserved: {"reference-library.json", "detection.model", "calibration.json"})
        paths.push_back(QDir(dataRoot_).filePath(QStringLiteral(".vibration-workbench/")+QString::fromLatin1(reserved)).toUtf8().toStdString());
    if(profile_&&!profile_->source_path.empty())protectCapture(QString::fromUtf8(profile_->source_path.data(),int(profile_->source_path.size())));
    return paths;
}
void Window::exportResult(){
    if(!result_)return;QString filter=QStringLiteral("JSON (*.json)");const auto file=touchFile(this,QStringLiteral("导出分析"),QFileInfo(path_).absolutePath()+"/analysis.json",QStringLiteral("JSON (*.json);;CSV (*.csv)"),true,&filter);if(file.isEmpty())return;
    auto object=QJsonDocument::fromJson(QByteArray::fromStdString(to_json(*result_))).object();object["detection"]=QJsonDocument::fromJson(QByteArray::fromStdString(detection_to_json(detection_))).object();object["candidate_model"]=QJsonDocument::fromJson(QByteArray::fromStdString(detectionPage_->candidateJson())).object();object["profile_path"]=profilePath_;
    QByteArray data=QJsonDocument(object).toJson(QJsonDocument::Indented);
    if(filter.startsWith("CSV")){QString table="field,channel,value,unit\r\n";auto add=[&](QString field,QString channel,QString value,QString unit){table+=csvCell(field)+','+csvCell(channel)+','+csvCell(value)+','+csvCell(unit)+"\r\n";};
        const auto configuration=object.value("configuration").toObject();
        const auto sourceKind=result_->source_metadata.synthetic?QStringLiteral("synthetic"):configuration.value("signal_kind").toString(QStringLiteral("unknown"));
        add("source_path",{},path_,{});add("source_kind",{},sourceKind,{});add("classification",{},"not_classified",{});add("calibration_confirmed",{},config_.calibration_confirmed?"true":"false",{});
        for(int c=0;c<3;++c){add("ac_rms",QString::number(c+1),QString::number(result_->time_stats[c].ac_rms,'g',16),QString::fromStdString(result_->amplitude_unit));add("peak",QString::number(c+1),QString::number(result_->time_stats[c].abs_peak,'g',16),QString::fromStdString(result_->amplitude_unit));}
        add("analysis.json",{},QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)),{});data=table.toUtf8();}
    try{write_detection_text(file.toUtf8().toStdString(),data.toStdString(),protectedPaths());setFootnote(QStringLiteral("已导出 %1").arg(QFileInfo(file).fileName()));}catch(const std::exception& e){QMessageBox::warning(this,QStringLiteral("导出未完成"),QString::fromUtf8(e.what()));}
}
void Window::loadCalibrationSettings(){
    calibrationSettingsPath_=QDir(dataRoot_).filePath(".vibration-workbench/calibration.json");
    setProperty("calibrationSettingsPath",calibrationSettingsPath_);
    const QFileInfo root(dataRoot_),appdir(QDir(dataRoot_).filePath(".vibration-workbench")),saved(calibrationSettingsPath_);
    QString error;
    if(dataRoot_=="/"||dataRoot_=="/boot"||dataRoot_.startsWith("/boot/")||!root.isDir()||root.isSymLink()||root.canonicalFilePath()!=root.absoluteFilePath())
        error=QStringLiteral("标定参数未加载：数据分区不可用，默认显示原始电压 V。");
    else if((appdir.exists()&&(!appdir.isDir()||appdir.isSymLink()))||saved.isSymLink())
        error=QStringLiteral("标定参数未加载：参数路径不是安全的普通目录或文件。");
    else if(saved.exists()){
        QFile file(calibrationSettingsPath_);
        if(!saved.isFile()||saved.size()>65536||!file.open(QIODevice::ReadOnly))error=QStringLiteral("标定参数未加载：无法读取参数文件。");
        else{
            QJsonParseError parse;const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
            if(parse.error!=QJsonParseError::NoError||!document.isObject())error=QStringLiteral("标定参数未加载：参数 JSON 无效。");
            else if(!apply_calibration_settings_json(document.object(),config_,&error))error=QStringLiteral("标定参数未加载：%1").arg(error);
        }
    }
    setProperty("calibrationStorageError",error);if(!error.isEmpty()){setFootnote(error);footnote_->setToolTip(error);}
}
bool Window::saveCalibrationSettings(const AnalysisConfig& config,QString* error){
    auto fail=[&](const QString& message){setProperty("calibrationStorageError",message);if(error)*error=message;return false;};
    const QFileInfo root(dataRoot_);const auto appPath=QDir(dataRoot_).filePath(".vibration-workbench");
    if(dataRoot_=="/"||dataRoot_=="/boot"||dataRoot_.startsWith("/boot/")||!root.isDir()||root.isSymLink()||root.canonicalFilePath()!=root.absoluteFilePath()||!root.isWritable())
        return fail(QStringLiteral("参数未应用：数据分区不可用或不可写，请恢复数据分区后重试。"));
    QFileInfo appdir(appPath);if(appdir.exists()&&(!appdir.isDir()||appdir.isSymLink()))return fail(QStringLiteral("参数未应用：应用参数目录被占用或为链接。"));
    if(!appdir.exists()&&!QDir(dataRoot_).mkdir(".vibration-workbench"))return fail(QStringLiteral("参数未应用：无法在数据分区创建应用参数目录。"));
    appdir.refresh();if(!appdir.isDir()||appdir.isSymLink()||appdir.canonicalFilePath()!=appdir.absoluteFilePath())return fail(QStringLiteral("参数未应用：应用参数目录校验失败。"));
    const QFileInfo saved(calibrationSettingsPath_);if(saved.isSymLink()||(saved.exists()&&!saved.isFile()))return fail(QStringLiteral("参数未应用：参数文件路径不是普通文件。"));
    QByteArray bytes;try{bytes=QJsonDocument(calibration_settings_json(config)).toJson(QJsonDocument::Indented);}catch(const std::exception& e){return fail(QString::fromUtf8(e.what()));}
    QSaveFile file(calibrationSettingsPath_);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())return fail(QStringLiteral("参数未应用：无法安全保存到数据分区：%1").arg(file.errorString()));
    // QSaveFile commits by rename. VFAT also needs the renamed inode and
    // directory chain synchronized before saved parameters are acknowledged.
    for(const auto& path:{calibrationSettingsPath_,appPath,dataRoot_}){
        const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)return fail(QStringLiteral("参数未应用：无法校验参数持久保存。"));
        const int flushed=::fsync(fd);const int closed=::close(fd);
        if(flushed!=0||closed!=0)return fail(QStringLiteral("参数未应用：存储同步失败，请检查 SD 卡。"));
    }
    setProperty("calibrationStorageError",QString());if(error)error->clear();return true;
}
void Window::settings(){
    if(isAnalyzing()||captureActive_||capturePage_->property("captureActive").toBool())return;QDialog dialog(this);dialog.setObjectName("workbenchSettingsDialog");dialog.setWindowTitle(QStringLiteral("分析参数"));dialog.resize(640,std::min(540,height()-36));auto* layout=new QVBoxLayout(&dialog);layout->setContentsMargins(20,16,20,16);layout->setSpacing(10);auto* tabs=new QTabWidget;tabs->setObjectName("workbenchSettingsTabs");layout->addWidget(tabs,1);
    auto* signal=new QWidget;auto* form=new QFormLayout(signal);form->setContentsMargins(12,16,12,12);form->setSpacing(10);
    auto number=[&](const QString& name,double lo,double hi,double value,int decimals){auto* n=new NumberField;n->setAccessibleName(name);n->setRange(lo,hi);n->setDecimals(decimals);n->setValue(value);form->addRow(name,n);return n;};
    auto* maximum=number(QStringLiteral("频谱上限 · Hz"),1,100000,config_.spectrum_max_hz,1);maximum->setObjectName("analysisMaxHz");
    auto* ppr=number(QStringLiteral("每转脉冲 PPR"),0,4096,config_.ppr_override.value_or(0),0);ppr->setObjectName("analysisPpr");ppr->setSpecialValueText(QStringLiteral("使用文件 / 未知"));ppr->setToolTip(QStringLiteral("0 使用文件中的 PPR；文件未知时保持未知。非零值须经实际确认。"));
    auto* fft=new ChoiceField;fft->setObjectName("analysisFft");for(int n:{4096,8192,16384,32768,65536,131072})fft->addItem(QString::number(n),n);fft->setCurrentIndex(fft->findData(int(config_.fft_size)));form->addRow(QStringLiteral("FFT 点数"),fft);
    auto* source=new ChoiceField;source->setObjectName("analysisSource");source->addItems({QStringLiteral("来源未确认"),QStringLiteral("测试信号"),QStringLiteral("机械振动已确认")});source->setCurrentIndex(int(config_.signal_kind));
    const bool synthetic=result_&&result_->source_metadata.synthetic;
    const bool invalidMetadata=result_&&result_->source_metadata.status=="invalid_or_unreadable_sidecar";
    const bool sourceLocked=synthetic||invalidMetadata;source->setEnabled(!sourceLocked);form->addRow(QStringLiteral("信号来源"),source);
    auto* calibrated=new QCheckBox(QStringLiteral("已完成实际标定"));calibrated->setObjectName("analysisCalibration");calibrated->setChecked(!sourceLocked&&config_.calibration_confirmed);calibrated->setEnabled(!sourceLocked);form->addRow({},calibrated);
    auto* sourceNotice=muted(synthetic?QStringLiteral("合成记录不能确认为机械测量或已标定。"):invalidMetadata?QStringLiteral("来源信息无效，请先修复，再确认来源和标定。"):QStringLiteral("PPR、来源和标定须以实际确认结果为准。"));sourceNotice->setObjectName("analysisSourceNotice");sourceNotice->setWordWrap(true);form->addRow({},sourceNotice);
    auto* signalScroll=new QScrollArea;signalScroll->setObjectName("analysisSettingsScroll");signalScroll->setFrameShape(QFrame::NoFrame);signalScroll->setWidgetResizable(true);signalScroll->setWidget(signal);tabs->addTab(signalScroll,QStringLiteral("分析参数"));
    auto* detection=new QWidget;auto* rules=new QVBoxLayout(detection);rules->setContentsMargins(12,16,12,12);rules->setSpacing(12);auto* setup=new QLineEdit(setupId_);setup->setMaxLength(128);setup->setPlaceholderText(QStringLiteral("台架与安装编号"));rules->addWidget(setup);
    auto* profileName=muted(profilePath_.isEmpty()?QStringLiteral("尚未加载实验规则"):QFileInfo(profilePath_).fileName());profileName->setWordWrap(true);rules->addWidget(profileName);auto* load=action(QStringLiteral("加载规则"),Icon::Folder,"loadWorkbenchProfile");auto* clear=action(QStringLiteral("清除规则"),Icon::Close,"clearWorkbenchProfile");auto* ruleActions=new QHBoxLayout;ruleActions->addWidget(load);ruleActions->addWidget(clear);rules->addLayout(ruleActions);
    auto* draft=action(QStringLiteral("保存基线草稿"),Icon::Export,"saveWorkbenchBaseline");draft->setEnabled(bool(result_));rules->addWidget(draft);auto* note=muted(QStringLiteral("实验规则只产生候选结果；基线草稿不包含识别规则。"));note->setWordWrap(true);rules->addWidget(note);rules->addStretch();tabs->addTab(detection,QStringLiteral("检测配置"));
    connect(load,&QPushButton::clicked,&dialog,[&]{const auto file=touchFile(&dialog,QStringLiteral("加载实验规则"),directory_,QStringLiteral("规则 (*.profile);;全部文件 (*)"),false);if(file.isEmpty())return;profile_.reset();profileError_.clear();profilePath_=QFileInfo(file).absoluteFilePath();if(!protectedHistory_.contains(profilePath_))protectedHistory_.append(profilePath_);try{profile_=load_detection_profile(profilePath_.toUtf8().toStdString());const auto baseline=QString::fromUtf8(profile_->source_path.c_str());if(!baseline.isEmpty()&&!protectedHistory_.contains(baseline))protectedHistory_.append(baseline);profileName->setText(QFileInfo(file).fileName());}catch(const std::exception& e){profileError_=QString::fromUtf8(e.what());profileName->setText(QStringLiteral("规则无效：%1").arg(profileError_));}updateDetection();});
    connect(clear,&QPushButton::clicked,&dialog,[&]{profile_.reset();profilePath_.clear();profileError_.clear();profileName->setText(QStringLiteral("尚未加载实验规则"));updateDetection();});
    connect(draft,&QPushButton::clicked,&dialog,[&]{if(!result_)return;const auto file=touchFile(&dialog,QStringLiteral("保存基线草稿"),directory_+"/baseline.profile",QStringLiteral("规则 (*.profile)"),true);if(file.isEmpty())return;try{const auto baseline=make_baseline_draft(*result_,setup->text().toUtf8().toStdString());write_detection_text(file.toUtf8().toStdString(),detection_profile_text(baseline),protectedPaths());profileName->setText(QStringLiteral("草稿已保存"));}catch(const std::exception& e){profileName->setText(QString::fromUtf8(e.what()));}});
    auto* error=muted({});error->setObjectName("analysisSettingsError");error->setWordWrap(true);layout->addWidget(error);auto* buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Close);buttons->button(QDialogButtonBox::Apply)->setObjectName("applyWorkbenchSettings");buttons->button(QDialogButtonBox::Apply)->setText(QStringLiteral("应用"));buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply),&QPushButton::clicked,&dialog,[&]{
        error->clear();auto candidate=config_;QString message;
        if(capture_&&capture_->header.pulses_per_rev&&ppr->value()>0&&std::abs(ppr->value()-capture_->header.pulses_per_rev)>1e-9){error->setText(QStringLiteral("PPR 与文件中的已知值冲突，请使用文件值。"));return;}
        candidate.spectrum_max_hz=maximum->value();candidate.fft_size=std::size_t(fft->currentData().toInt());candidate.ppr_override=ppr->value()>0?std::optional<double>(ppr->value()):std::nullopt;
        candidate.calibration_confirmed=!sourceLocked&&calibrated->isChecked();candidate.signal_kind=sourceLocked?(synthetic?SignalKind::TestSignal:SignalKind::Unknown):SignalKind(source->currentIndex());
        if(!calibration_config_equal(config_,candidate)&&!saveCalibrationSettings(candidate,&message)){error->setText(message);return;}
        config_=candidate;analysis_->setAnalysisConfig(config_);setupId_=setup->text().trimmed();dialog.accept();
    });
    if(dialog.exec()==QDialog::Accepted){if(result_)analyze();else updateDetection();}
}
}
