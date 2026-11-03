#include "capture_page.hpp"
#include "controls.hpp"
#include "capture_client.hpp"
#include "touch_directory_dialog.hpp"
#include "touch_keyboard.hpp"
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <cmath>
#include <set>
#include <algorithm>

namespace vibration::workbench {
namespace {
QLabel* text(const QString& value,const char* id,int pixels=16){
    auto* label=new QLabel(value);label->setObjectName(id);label->setTextFormat(Qt::PlainText);
    label->setStyleSheet(QString("font-size:%1px;color:#344255;").arg(std::max(16,pixels)));return label;
}
QPushButton* action(const QString& value,const char* id){
    auto* button=new QPushButton(value);button->setObjectName(id);button->setMinimumHeight(44);return button;
}
bool integer(const QJsonValue& value,double minimum,double maximum){
    const double n=value.toDouble(-1);return value.isDouble()&&std::isfinite(n)&&std::floor(n)==n&&n>=minimum&&n<=maximum;
}
QString sampleError(const QString& value,int rate,bool streaming=false){
    bool ok=!value.isEmpty();
    for(const auto c:value)if(c<'0'||c>'9')ok=false;
    bool parsed=false;const auto count=value.toLongLong(&parsed);
    if(!ok||!parsed||count<=0)return QStringLiteral("每通道采样点数须为正整数。");
    if(!streaming&&count>2097024)return QStringLiteral("每通道采样点数不能超过 2,097,024。");
    const int maximum=streaming?86400:40;
    if(rate<=0||count<rate||count>qint64(rate)*maximum||count%rate)
        return QStringLiteral("当前采样率仅支持 %1–%2 点，且须为 %1 的整数倍（1–%3 秒）。").arg(rate).arg(qint64(rate)*maximum).arg(maximum);
    return {};
}
bool decimal(const QJsonValue& value,quint64& result){
    if(!value.isString())return false;const auto raw=value.toString();if(raw.isEmpty()||(raw.size()>1&&raw[0]=='0'))return false;
    for(auto c:raw)if(c<'0'||c>'9')return false;bool ok=false;result=raw.toULongLong(&ok);return ok;
}
QString sourceName(const QJsonObject& value){const auto source=value.value("source").toString();return source=="hardware_test"?QStringLiteral("硬件合成自检"):value.value("synthetic").toBool()?QStringLiteral("主机合成自检"):QStringLiteral("ADC 原始输入 · 未标定");}
// Match NumberField's touch keypad while retaining raw invalid input in the
// outer line edit. A spin box would silently fix up out-of-range point counts.
class SampleCountField final : public TouchLineEdit {
public:
    SampleCountField(){setKeyboardMode(TouchKeyboardDialog::Mode::Integer);}
};
}
CapturePage::CapturePage(std::function<QDoubleSpinBox*()> factory,QWidget* parent,const QString& dataRoot):QWidget(parent),dataRoot_(QDir(dataRoot).absolutePath()){
    setObjectName("capturePage");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(24,18,24,16);layout->setSpacing(12);
    auto* heading=new QHBoxLayout;
    auto* headingText=text(QStringLiteral("采集参数"),"captureHeading",22);headingText->setStyleSheet("font-size:22px;color:#263345;font-weight:600;");heading->addWidget(headingText);heading->addSpacing(16);
    state_=text({},"captureState",14);heading->addWidget(state_);heading->addSpacing(12);
    source_=text({},"captureSource",14);heading->addWidget(source_);heading->addStretch();
    retry_=action(QStringLiteral("重新连接"),"captureReconnect");heading->addWidget(retry_);layout->addLayout(heading);
    auto* centerRow=new QHBoxLayout;
    auto* fields=new QWidget;fields->setObjectName("captureParameters");fields->setMaximumWidth(840);
    auto* form=new QVBoxLayout(fields);form->setContentsMargins(0,10,0,0);form->setSpacing(8);
    modeLabel_=text(QStringLiteral("采集方式"),"captureModeLabel");form->addWidget(modeLabel_);
    mode_=new ChoiceField;mode_->setObjectName("captureMode");mode_->setMinimumHeight(44);mode_->addItem(QStringLiteral("定时采集"),"timed");mode_->addItem(QStringLiteral("持续采集 · 手动停止"),"continuous");form->addWidget(mode_);
    auto* grid=new QGridLayout;fieldGrid_=grid;grid->setHorizontalSpacing(16);grid->setVerticalSpacing(8);
    rate_=new ChoiceField;rate_->setObjectName("captureRate");rate_->setMinimumHeight(44);
    for(int hz:{50000,25000,10000,5000})rate_->addItem(QString::number(hz/1000)+" kHz",hz);
    duration_=factory?factory():new QDoubleSpinBox;duration_->setObjectName("captureDuration");duration_->setAccessibleName(QStringLiteral("采集时长"));duration_->setDecimals(0);duration_->setRange(1,86400);duration_->setValue(10);duration_->setSuffix(QStringLiteral(" 秒"));duration_->setMinimumHeight(44);
    auto* samples=new SampleCountField;samples_=samples;samples_->setObjectName("captureSamples");samples_->setAccessibleName(QStringLiteral("每通道采样点数"));samples_->setMinimumHeight(44);samples_->setInputMethodHints(Qt::ImhDigitsOnly);samples_->setMaxLength(28);
    samples->setValidator([this](const QString& value){return sampleError(value,rate_->currentData().toInt(),streaming_);});
    samples_->setToolTip(QStringLiteral("每通道点数 = 采样率 × 整数秒数；三路数值总数为其 3 倍。采集点数与离线分析的 FFT 点数分别设置。"));
    ppr_=factory?factory():new QDoubleSpinBox;ppr_->setObjectName("capturePpr");ppr_->setAccessibleName(QStringLiteral("每转脉冲数"));ppr_->setDecimals(0);ppr_->setRange(1,4096);ppr_->setValue(1);ppr_->setMinimumHeight(44);
    const QStringList labels{QStringLiteral("采样率"),QStringLiteral("每通道采样点数"),QStringLiteral("采集时长"),QStringLiteral("每转脉冲 PPR")};
    const QList<QWidget*> inputs{rate_,samples_,duration_,ppr_};
    for(int i=0;i<inputs.size();++i){const int row=(i/2)*2,column=i%2;auto* label=text(labels[i],"captureFieldLabel");if(i==1)samplesLabel_=label;if(i==2)durationLabel_=label;if(i==3)pprLabel_=label;grid->addWidget(label,row,column);grid->addWidget(inputs[i],row+1,column);grid->setColumnStretch(column,1);}form->addLayout(grid);
    parameterError_=text({},"captureParameterError",16);parameterError_->setStyleSheet("font-size:16px;color:#9a4b32;");parameterError_->setWordWrap(true);form->addWidget(parameterError_);parameterError_->hide();
    form->addWidget(text(QStringLiteral("保存位置"),"captureDirectoryLabel"));
    auto* pathRow=new QHBoxLayout;pathRow->setSpacing(8);
    directory_=new QLineEdit(QDir(dataRoot_).filePath("vibration"));directory_->setObjectName("captureDirectory");directory_->setReadOnly(true);directory_->setMinimumWidth(0);directory_->setMinimumHeight(44);
    browse_=action(QStringLiteral("选择目录"),"captureBrowse");pathRow->addWidget(directory_,1);pathRow->addWidget(browse_);form->addLayout(pathRow);
    error_=text({},"captureError",16);error_->setStyleSheet("font-size:16px;color:#9a4b32;");error_->setWordWrap(true);form->addWidget(error_);error_->hide();form->addStretch();
    auto* parameterScroll=new QScrollArea;parameterScroll->setObjectName("captureParameterScroll");parameterScroll->setFrameShape(QFrame::NoFrame);parameterScroll->setWidgetResizable(true);parameterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);parameterScroll->setWidget(fields);centerRow->addWidget(parameterScroll,20);centerRow->addStretch(1);layout->addLayout(centerRow,1);
    auto* bottom=new QFrame;bottom->setObjectName("captureActionBar");
    auto* bottomLayout=new QVBoxLayout(bottom);bottomLayout->setContentsMargins(0,12,0,0);bottomLayout->setSpacing(10);
    progress_=new QProgressBar;progress_->setObjectName("captureProgress");progress_->setRange(0,1000);progress_->setValue(0);progress_->setTextVisible(false);progress_->setFixedHeight(3);
    progress_->setStyleSheet("QProgressBar{border:0;background:#e4eaf1;}QProgressBar::chunk{background:#2b65ad;}");bottomLayout->addWidget(progress_);
    auto* actions=new QHBoxLayout;progressText_=text({},"captureProgressText",12);actions->addWidget(progressText_,1);
    view_=action(QStringLiteral("打开记录"),"captureView");view_->hide();actions->addWidget(view_);
    cancel_=action(QStringLiteral("停止采集"),"captureCancel");actions->addWidget(cancel_);
    start_=action(QStringLiteral("开始采集"),"captureStart");start_->setProperty("primary",true);start_->setMinimumWidth(112);actions->addWidget(start_);bottomLayout->addLayout(actions);layout->addWidget(bottom);
    client_=new CaptureClient(this);client_->setStreaming(true);poll_=new QTimer(this);poll_->setInterval(250);poll_->start();
    connect(client_,&CaptureClient::responseReceived,this,&CapturePage::response);
    connect(client_,&CaptureClient::connectionFailed,this,&CapturePage::unavailable);
    connect(client_,&CaptureClient::liveFrame,this,[this](const QJsonObject& frame){if(active_&&!jobId_.isEmpty()&&frame.value("job_id").toString()==jobId_)emit liveFrame(frame);});
    connect(client_,&CaptureClient::previewBatchStarted,this,&CapturePage::previewBatchStarted);
    connect(client_,&CaptureClient::previewBatchFinished,this,&CapturePage::previewBatchFinished);
    connect(client_,&CaptureClient::previewRejected,this,[this](const QString& reason,bool quarantined){
        if(!active_||jobId_.isEmpty())return;
        auto status=lastJobStatus_;status["job_id"]=jobId_;status["preview_error"]=reason;status["preview_only"]=true;if(quarantined)status["preview_available"]=false;emit captureState(status);
    });
    connect(client_,&CaptureClient::jobUpdated,this,[this](const QJsonObject& data){
        if(active_&&!jobId_.isEmpty()){
            if(data.value("job_id").toString()!=jobId_){unavailable("Unexpected capture job ID");return;}jobStatus(data);
        }
    });
    connect(client_,&CaptureClient::requestRejected,this,[this](const QString& command,const QString& code,const QString& detail){
        if(command=="capabilities"||command=="status"||command=="cancel"||command=="stop"){unavailable(code+": "+detail);return;}
        active_=false;cancelling_=false;jobId_.clear();setStatus(QStringLiteral("未开始"));
        showError(code=="UNAVAILABLE"?QStringLiteral("设备尚未就绪，请重新连接。"):QStringLiteral("未能开始，请检查参数和保存位置。"),code+": "+detail);controls();
    });
    connect(poll_,&QTimer::timeout,this,[this]{if(active_&&!jobId_.isEmpty()&&!client_->hasPending("status"))client_->request("status",streaming_?QJsonObject{}:QJsonObject{{"job_id",jobId_}});});
    connect(start_,&QPushButton::clicked,this,&CapturePage::startCapture);
    connect(cancel_,&QPushButton::clicked,this,&CapturePage::cancelCurrent);
    connect(view_,&QPushButton::clicked,this,[this]{if(!active_&&!analysisBusy_&&!completedPath_.isEmpty())emit captureReady(completedPath_);});
    connect(retry_,&QPushButton::clicked,this,&CapturePage::reconnect);
    connect(browse_,&QPushButton::clicked,this,[this]{
        const auto directory=TouchDirectoryDialog::getExistingDirectory(this,directory_->text(),dataRoot_);
        if(!directory.isEmpty()){directory_->setText(directory);directory_->setToolTip(directory);updateParameterState();}
    });
    connect(rate_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{samplesFromDuration();});
    connect(duration_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{samplesFromDuration();});
    connect(samples_,&QLineEdit::textChanged,this,[this]{durationFromSamples();});
    connect(mode_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{updateMode();});
    connect(ppr_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{updateParameterState();});
    samplesFromDuration();setStreaming(true);
}
CapturePage::~CapturePage(){poll_->stop();client_->stop();}
void CapturePage::showEvent(QShowEvent* event){QWidget::showEvent(event);if(!connectedOnce_)reconnect();}
void CapturePage::ensureConnected(){if(!connectedOnce_)reconnect();}
void CapturePage::setParameterOnly(bool enabled){if(auto* actions=findChild<QFrame*>("captureActionBar"))actions->setVisible(!enabled);}
QString CapturePage::outputDirectory()const{return directory_->text();}
QString CapturePage::progressSummary()const{return progressText_->text();}
bool CapturePage::setNextRecordLabel(RecordLabel label){
    if(active_||recordLabelKey(label).isEmpty())return false;
    if(nextRecordLabel_!=label){nextRecordLabel_=label;setProperty("captureNextRecordLabel",recordLabelKey(label));emit controlsChanged();}
    return true;
}
void CapturePage::annotateCommittedRecord(const QString& path){
    if(activeRecordLabel_==RecordLabel::Unlabeled||annotatedSegments_.contains(path))return;
    QString diagnostic;
    if(!saveRecordLabel(path,activeRecordLabel_,jobId_,nullptr,&diagnostic)){
        const auto visible=QStringLiteral("数据已保存，但工况标签保存失败。请在“记录”中重新标注。");
        setProperty("captureLabelError",diagnostic);setProperty("captureLabelFailedPath",path);
        showError(visible,path+"\n"+diagnostic);emit recordLabelSaveFailed(path,diagnostic);return;
    }
    annotatedSegments_.insert(path);emit recordLabelSaved(path);
}
QString CapturePage::planSummary()const{
    const auto rate=rate_->currentData().toInt();
    if(!parameterError().isEmpty())return QStringLiteral("采集参数需修正");
    const bool continuous=streaming_&&mode_->currentData().toString()=="continuous";
    return QStringLiteral("%1 kHz · %2 · PPR %3").arg(rate/1000.,0,'g',4)
        .arg(continuous?QStringLiteral("持续采集"):QStringLiteral("定时 %1 s · %2 点/通道").arg(int(duration_->value())).arg(samples_->text()))
        .arg(int(ppr_->value()));
}
void CapturePage::setServiceCommand(const QString& program,const QStringList& arguments){
    client_->setCommand(program,arguments);available_=false;active_=false;cancelling_=false;jobId_.clear();completedPath_.clear();setProperty("captureLatestSegment",QString());view_->hide();connectedOnce_=false;controls();if(isVisible())reconnect();
}
void CapturePage::setAnalysisBusy(bool busy){analysisBusy_=busy;controls();}
void CapturePage::setStreaming(bool enabled){if(active_)return;streaming_=enabled;client_->setStreaming(enabled);duration_->setMaximum(enabled?86400:40);cancel_->setText(enabled?QStringLiteral("停止采集"):QStringLiteral("取消"));view_->setText(enabled?QStringLiteral("打开最新分段"):QStringLiteral("打开记录"));updateMode();}
void CapturePage::updateMode(){
    const bool continuous=streaming_&&mode_->currentData().toString()=="continuous";
    mode_->setVisible(streaming_);modeLabel_->setVisible(streaming_);samples_->setVisible(!continuous);samplesLabel_->setVisible(!continuous);duration_->setVisible(!continuous);durationLabel_->setVisible(!continuous);
    fieldGrid_->removeWidget(pprLabel_);fieldGrid_->removeWidget(ppr_);fieldGrid_->addWidget(pprLabel_,continuous?0:2,1);fieldGrid_->addWidget(ppr_,continuous?1:3,1);progress_->setRange(0,continuous?0:1000);updateParameterState();
}
void CapturePage::cancelCurrent(){
    if(!active_||cancelling_)return;
    cancelling_=true;setStatus(streaming_?QStringLiteral("正在停止并保存末段"):QStringLiteral("正在取消"));controls();
    if(!jobId_.isEmpty())client_->request(streaming_?"stop":"cancel",streaming_?QJsonObject{}:QJsonObject{{"job_id",jobId_}});
}
void CapturePage::reconnect(){if(active_)return;connectedOnce_=true;available_=false;showError({});setStatus(QStringLiteral("正在连接设备"));source_->clear();controls();client_->connectService();}
void CapturePage::setStatus(const QString& value){state_->setText(value);setProperty("captureStatus",value);emit statusChanged(value);}
void CapturePage::showError(const QString& visible,const QString& diagnostic){error_->setText(visible);error_->setToolTip(diagnostic.left(2048));error_->setVisible(!visible.isEmpty());setProperty("captureDiagnostic",diagnostic.left(2048));}
void CapturePage::unavailable(const QString& diagnostic){
    if(active_&&!jobId_.isEmpty()){
        auto status=lastJobStatus_;status["job_id"]=jobId_;status["state"]="failed";status["connection_lost"]=true;status["driver_stop_confirmed"]=false;status["output_path"]="";status["error_code"]="CONNECTION_LOST";status["error_message"]=diagnostic;emit captureState(status);
    }
    client_->stop();available_=false;active_=false;cancelling_=false;jobId_.clear();setStatus(QStringLiteral("设备未连接"));
    showError(QStringLiteral("连接设备后点击“重新连接”。已存记录仍可打开。"),diagnostic);source_->clear();controls();
}
QJsonObject CapturePage::plan()const{if(!parameterError().isEmpty())return {};QJsonObject p{{"sample_rate_hz",rate_->currentData().toInt()},{"pulses_per_rev",int(ppr_->value())},{"output_directory",directory_->text()}};const bool continuous=streaming_&&mode_->currentData().toString()=="continuous";if(streaming_)p["mode"]=continuous?"continuous":"timed";if(!continuous)p["duration_seconds"]=int(duration_->value());return p;}
void CapturePage::startCapture(){
    if(active_||!available_)return;
    if(!parameterError().isEmpty()){updateParameterState();return;}
    const auto directory=directory_->text();
    if(directory=="/tmp/vibration-captures"&&!QDir(directory).exists()&&!QDir().mkpath(directory)){
        showError(QStringLiteral("无法创建临时目录，请选择其他保存位置。"));return;
    }
    planned_=plan();activeRecordLabel_=nextRecordLabel_;annotatedSegments_.clear();setProperty("captureActiveRecordLabel",recordLabelKey(activeRecordLabel_));setProperty("captureLabelError",QString());setProperty("captureLabelFailedPath",QString());active_=true;cancelling_=false;jobId_.clear();completedPath_.clear();setProperty("captureLatestSegment",QString());view_->hide();showError({});progress_->setValue(0);progressText_->setText(QStringLiteral("检查保存位置"));setStatus(QStringLiteral("准备采集"));controls();client_->request(streaming_?"start":"validate_plan",{{"plan",planned_}});
}
void CapturePage::response(const QString& command,const QJsonObject& data){
    if(command=="capabilities"){capabilities(data);return;}
    if(streaming_){
        if(command=="start"){
            const auto id=data.value("job_id").toString();if(id.isEmpty()||id.size()>128){unavailable("Invalid stream start response");return;}jobId_=id;
            if(cancelling_)client_->request("stop");jobStatus(data);return;
        }
        if(command=="status"||command=="stop"){if(active_&&!jobId_.isEmpty()){if(data.value("job_id").toString()!=jobId_){unavailable("Stream session ID mismatch");return;}jobStatus(data);}return;}
    }
    if(command=="validate_plan"){
        if(!data.value("valid").isBool()||!data.value("ready").isBool()){unavailable("Invalid plan validation response");return;}
        if(!data.value("valid").toBool()||!data.value("ready").toBool()){active_=false;capabilities(data.value("capabilities").toObject());return;}
        if(cancelling_){active_=false;cancelling_=false;setStatus(QStringLiteral("已取消"));controls();return;}
        client_->request("start",{{"plan",planned_},{"preview",true}});return;
    }
    if(command=="start"){
        const auto id=data.value("job_id").toString();
        if(id.isEmpty()||id.size()>128||data.value("state")!="preparing"){unavailable("Invalid start response");return;}
        jobId_=id;setStatus(QStringLiteral("准备采集"));
        lastJobStatus_=QJsonObject{{"job_id",id},{"state","preparing"},{"records_completed",0},{"requested_records",planned_.value("sample_rate_hz").toInt()*planned_.value("duration_seconds").toInt()},{"hardware_confirmed",capabilities_.value("hardware_confirmed")},{"synthetic",capabilities_.value("synthetic")},{"preview_available",data.value("preview_available").toBool()},{"preview_reason",data.value("preview_reason")},{"plan",planned_},{"output_path",""}};
        emit captureState(lastJobStatus_);if(cancelling_)client_->request("cancel",{{"job_id",jobId_}});controls();return;
    }
    if(command=="status"){
        if(active_){if(data.value("job_id").toString()!=jobId_){unavailable("Status job ID mismatch");return;}jobStatus(data);}return;
    }
    if(command=="cancel"&&(data.value("job_id").toString()!=jobId_||!data.value("accepted").isBool()))unavailable("Cancel response mismatch");
}
void CapturePage::capabilities(const QJsonObject& data){
    if(!data.value("available").isBool()||!data.value("hardware_confirmed").isBool()||!data.value("synthetic").isBool()){unavailable("Invalid capability response");return;}
    const bool available=data.value("available").toBool(),synthetic=data.value("synthetic").toBool();
    if(!available){unavailable(data.value("reason").toString());return;}
    if(!synthetic&&!data.value("hardware_confirmed").toBool()){unavailable("Unconfirmed capture backend");return;}
    if(data.value("clock_hz").toInt()!=50000000||data.value("adc_range_uv").toInt()!=5000000||data.value("channel_map").toInt()!=0x50301){unavailable("Unsupported acquisition capability");return;}
    const auto values=data.value("supported_rates_hz").toArray();std::set<int> rates;
    for(const auto& value:values){
        if(!integer(value,1,50000)||!std::set<int>{50000,25000,10000,5000}.count(value.toInt())||!rates.insert(value.toInt()).second){unavailable("Invalid supported-rate list");return;}
    }
    if(rates.empty()){unavailable("No supported capture rate");return;}
    const int selected=rate_->currentData().toInt();rate_->clear();
    for(int hz:{50000,25000,10000,5000})if(rates.count(hz))rate_->addItem(QString::number(hz/1000)+" kHz",hz);
    const int index=rate_->findData(selected);rate_->setCurrentIndex(index<0?0:index);
    capabilities_=data;available_=true;showError({});setStatus(QStringLiteral("设备就绪"));
    source_->setText(sourceName(data));source_->setToolTip(synthetic?QStringLiteral("合成测试数据，不代表机械测量。"):QStringLiteral("传感器与实际标定仍需确认。"));controls();updateParameterState();
}
void CapturePage::jobStatus(const QJsonObject& data){
    if(streaming_){streamStatus(data);return;}
    const auto state=data.value("state").toString();
    const QStringList states{"preparing","running","validating","saving","completed","failed","cancelled"};
    if(!states.contains(state)||!integer(data.value("records_completed"),0,2097024)||!integer(data.value("requested_records"),1,2097024)||!data.value("synthetic").isBool()||!data.value("hardware_confirmed").isBool()){
        unavailable("Invalid capture job status");return;
    }
    const int done=data.value("records_completed").toInt(),total=data.value("requested_records").toInt();
    if(done>total||total!=planned_.value("sample_rate_hz").toInt()*planned_.value("duration_seconds").toInt()||(!data.value("synthetic").toBool()&&!data.value("hardware_confirmed").toBool())){unavailable("Capture status does not match plan/source");return;}
    progress_->setValue(int(qint64(done)*1000/total));progressText_->setText(QStringLiteral("%1 / %2 条").arg(done).arg(total));
    const QStringList names{QStringLiteral("准备采集"),QStringLiteral("正在采集"),QStringLiteral("检查数据"),QStringLiteral("保存数据"),QStringLiteral("采集完成"),QStringLiteral("采集未完成"),QStringLiteral("已取消")};
    setStatus(cancelling_&&state!="completed"&&state!="failed"&&state!="cancelled"?QStringLiteral("正在取消"):names[states.indexOf(state)]);
    source_->setText(sourceName(data));
    if(state=="completed"){
        const auto path=data.value("output_path").toString();QFileInfo file(path);
        if(done!=total||!file.isAbsolute()||!file.isFile()||file.isSymLink()||file.absolutePath()!=QDir(planned_.value("output_directory").toString()).absolutePath()||file.suffix().compare("BIN",Qt::CaseInsensitive)!=0){unavailable("Invalid completed-capture path");return;}
        completedPath_=file.absoluteFilePath();annotateCommittedRecord(completedPath_);view_->show();progressText_->setToolTip(file.fileName());
    }
    if(state=="failed")showError(QStringLiteral("此次采集未保存，请检查设备和保存位置。"),data.value("error_code").toString()+": "+data.value("error_message").toString());
    lastJobStatus_=data;
    if(state=="completed"||state=="failed"||state=="cancelled"){active_=false;cancelling_=false;controls();}
    emit captureState(lastJobStatus_);
}
void CapturePage::streamStatus(const QJsonObject& data){
    if(data.value("job_id").toString()!=jobId_){unavailable("Stream event session mismatch");return;}
    if(data.value("event")=="segment_ready"){
        QFileInfo file(data.value("path").toString());const auto root=QDir(planned_.value("output_directory").toString()).canonicalPath();
        if(root.isEmpty()||!file.isAbsolute()||!file.isFile()||file.isSymLink()||!file.canonicalFilePath().startsWith(root+"/")||file.suffix().compare("BIN",Qt::CaseInsensitive)){unavailable("Invalid committed stream segment path");return;}
        completedPath_=file.absoluteFilePath();annotateCommittedRecord(completedPath_);view_->setText(QStringLiteral("打开最新分段"));view_->setToolTip(completedPath_+QStringLiteral("\n仅分析此分段，不代表全程记录。"));view_->show();setProperty("captureLatestSegment",completedPath_);controls();emit committedSegmentReady(completedPath_);return;
    }
    const auto phase=data.value("state").toString();quint64 received=0,durable=0,total=0;
    const QStringList states{"preparing","running","stopping","completed","stopped","failed"};
    if(!states.contains(phase)||!decimal(data.value("records_received"),received)||!decimal(data.value("durable_records"),durable)||!decimal(data.value("total_records"),total)||durable>received||!data.value("synthetic").isBool()||!data.value("hardware_confirmed").isBool()) {unavailable("Invalid stream counters/state/source");return;}
    const quint64 expected=planned_.value("mode")=="continuous"?0:quint64(planned_.value("sample_rate_hz").toInt())*quint64(planned_.value("duration_seconds").toInt());
    if(total!=expected||(total&&received>total)||data.value("synthetic")!=capabilities_.value("synthetic")){unavailable("Stream status differs from requested plan/source");return;}
    const bool terminal=phase=="completed"||phase=="stopped"||phase=="failed";
    if(terminal&&phase!="failed"&&durable!=received){unavailable("Stream ended before all received records were saved");return;}
    if(total){progress_->setRange(0,1000);progress_->setValue(int(received*1000/total));}else{progress_->setRange(0,terminal?1000:0);if(terminal)progress_->setValue(1000);}
    progressText_->setText(QStringLiteral("已采 %1 · 已保存 %2 条").arg(received).arg(durable));
    const QStringList names{QStringLiteral("准备采集"),QStringLiteral("正在采集"),QStringLiteral("正在停止并保存末段"),QStringLiteral("定时采集完成"),QStringLiteral("已停止并保存"),QStringLiteral("采集未完成")};
    setStatus(cancelling_&&!terminal?QStringLiteral("正在停止并保存末段"):names[states.indexOf(phase)]);
    source_->setText(sourceName(data));
    if(phase=="failed")showError(QStringLiteral("采集已结束，已保存的分段保留。"),data.value("error_code").toString()+": "+data.value("error_message").toString());
    lastJobStatus_=data;lastJobStatus_["analysis_scope"]="segment";lastJobStatus_["output_path"]=completedPath_;
    if(terminal){active_=false;cancelling_=false;controls();}
    emit captureState(lastJobStatus_);
}
void CapturePage::controls(){
    mode_->setEnabled(!active_);rate_->setEnabled(!active_);samples_->setEnabled(!active_);duration_->setEnabled(!active_);ppr_->setEnabled(!active_);browse_->setEnabled(!active_);retry_->setEnabled(!active_);
    start_->setEnabled(available_&&!active_&&parameterError().isEmpty());cancel_->setEnabled(active_&&!jobId_.isEmpty()&&!cancelling_);view_->setEnabled(!active_&&!analysisBusy_&&!completedPath_.isEmpty());
    setProperty("captureActive",active_);setProperty("captureAvailable",available_);setProperty("captureStreaming",streaming_);
    setProperty("captureCanStart",start_->isEnabled());setProperty("captureCanStop",cancel_->isEnabled());
    emit controlsChanged();
}
void CapturePage::updateParameterState(){
    const auto reason=parameterError();parameterError_->setText(reason);parameterError_->setVisible(!reason.isEmpty());setProperty("captureParametersValid",reason.isEmpty());
    controls();
}
QString CapturePage::parameterError()const{return streaming_&&mode_->currentData().toString()=="continuous"?QString():sampleError(samples_->text(),rate_->currentData().toInt(),streaming_);}
void CapturePage::samplesFromDuration(){const QSignalBlocker blocker(samples_);samples_->setText(QString::number(qint64(rate_->currentData().toInt())*int(duration_->value())));updateParameterState();}
void CapturePage::durationFromSamples(){if(sampleError(samples_->text(),rate_->currentData().toInt(),streaming_).isEmpty()){const QSignalBlocker blocker(duration_);duration_->setValue(samples_->text().toLongLong()/rate_->currentData().toInt());}updateParameterState();}
}
