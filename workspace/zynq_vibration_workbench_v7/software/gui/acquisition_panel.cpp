#include "acquisition_panel.hpp"
#include "native_icons.hpp"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QProgressBar>
#include <QProcess>
#include <QTimer>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFrame>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QShowEvent>
#include <QStyle>
#include <cmath>

namespace vibration::gui {
namespace {
constexpr auto schema="vibration.capture.v2";
QLabel* label(const QString& text, int size=14, const QString& color="#526479") {
    auto* result=new QLabel(text); result->setTextFormat(Qt::PlainText);
    result->setStyleSheet(QString("font-size:%1px;color:%2;").arg(size).arg(color));
    return result;
}
QPushButton* button(const QString& text,const char* id,NativeIcon icon) {
    auto* b=new QPushButton(text);b->setObjectName(id);b->setMinimumHeight(44);
    b->setIcon(nativeIcon(icon));b->setIconSize(QSize(20,20));return b;
}
bool integer(const QJsonValue& value,double lo,double hi) {
    const double n=value.toDouble(-1);return value.isDouble()&&std::isfinite(n)&&n==std::floor(n)&&n>=lo&&n<=hi;
}
}
AcquisitionPanel::AcquisitionPanel(std::function<QDoubleSpinBox*()> factory,QWidget* parent):QWidget(parent) {
    setObjectName("acquisitionPanel");
    auto* outer=new QHBoxLayout(this);outer->setContentsMargins(8,0,8,0);outer->addStretch(1);
    auto* body=new QWidget;body->setMaximumWidth(760);auto* layout=new QVBoxLayout(body);layout->setContentsMargins(12,8,12,4);layout->setSpacing(12);outer->addWidget(body,20);outer->addStretch(1);
    auto* heading=new QHBoxLayout;heading->setSpacing(12);
    state_=label(QStringLiteral("等待连接"),21,"#30363e");state_->setObjectName("captureState");heading->addWidget(state_,1);
    retry_=button({},"captureReconnect",NativeIcon::Refresh);retry_->setFixedWidth(44);retry_->setAccessibleName(QStringLiteral("重新连接采集服务"));retry_->setProperty("segment",true);heading->addWidget(retry_);layout->addLayout(heading);
    source_=label(QStringLiteral("连接采集服务后可设置并开始"),12,"#929aa6");source_->setObjectName("captureSource");source_->setWordWrap(true);layout->addWidget(source_);layout->addSpacing(8);
    auto* grid=new QGridLayout;grid->setHorizontalSpacing(14);grid->setVerticalSpacing(7);
    rate_=new QComboBox;rate_->setObjectName("captureRate");rate_->setMinimumHeight(44);
    for(int r:{50000,25000,10000,5000})rate_->addItem(QString::number(r/1000)+" kHz",r);
    duration_=factory();duration_->setObjectName("captureDuration");duration_->setDecimals(0);duration_->setRange(1,40);duration_->setValue(10);duration_->setSuffix(QStringLiteral(" 秒"));duration_->setAccessibleName(QStringLiteral("采集时长"));duration_->setMinimumHeight(44);
    ppr_=factory();ppr_->setObjectName("capturePpr");ppr_->setDecimals(0);ppr_->setRange(1,4096);ppr_->setValue(1);ppr_->setAccessibleName(QStringLiteral("每转脉冲数"));ppr_->setMinimumHeight(44);
    grid->addWidget(label(QStringLiteral("采样率"),12,"#7e8794"),0,0);grid->addWidget(rate_,1,0);
    grid->addWidget(label(QStringLiteral("时长"),12,"#7e8794"),0,1);grid->addWidget(duration_,1,1);
    grid->addWidget(label(QStringLiteral("每转脉冲"),12,"#7e8794"),0,2);grid->addWidget(ppr_,1,2);for(int col=0;col<3;++col)grid->setColumnStretch(col,1);layout->addLayout(grid);
    layout->addWidget(label(QStringLiteral("保存到"),12,"#7e8794"));
    auto* pathRow=new QHBoxLayout;pathRow->setSpacing(8);directory_=new QLineEdit("/tmp/vibration-captures");directory_->setObjectName("captureDirectory");directory_->setReadOnly(true);directory_->setMinimumWidth(0);
    browse_=button({},"captureBrowse",NativeIcon::Folder);browse_->setFixedWidth(48);browse_->setAccessibleName(QStringLiteral("选择保存目录"));pathRow->addWidget(directory_,1);pathRow->addWidget(browse_);layout->addLayout(pathRow);
    device_=label(QStringLiteral("ADC 参数等待设备确认"),12,"#929aa6");device_->setWordWrap(true);layout->addWidget(device_);layout->addStretch(1);
    auto* actionBox=new QFrame;actionBox->setObjectName("captureActionBox");actionBox->setStyleSheet("QFrame#captureActionBox{background:#fafbfd;border:1px solid #e9edf3;border-radius:15px;}");auto* bottom=new QVBoxLayout(actionBox);bottom->setContentsMargins(16,13,16,14);bottom->setSpacing(10);
    auto* counters=new QHBoxLayout;estimate_=label({},12,"#7c8798");estimate_->setObjectName("captureEstimate");estimate_->setWordWrap(true);counters->addWidget(estimate_,1);
    progressText_=label(QStringLiteral("尚未开始"),12,"#929baa");progressText_->setObjectName("captureProgressText");counters->addWidget(progressText_);bottom->addLayout(counters);
    progress_=new QProgressBar;progress_->setObjectName("captureProgress");progress_->setRange(0,1000);progress_->setValue(0);progress_->setTextVisible(false);progress_->setFixedHeight(3);progress_->setStyleSheet("QProgressBar{border:0;background:#e8edf3;border-radius:1px;}QProgressBar::chunk{background:#647a97;border-radius:1px;}");bottom->addWidget(progress_);
    details_=label({},12,"#93734a");details_->setObjectName("captureDetails");details_->setWordWrap(true);details_->setMaximumHeight(32);bottom->addWidget(details_);
    auto* actions=new QHBoxLayout;view_=button(QStringLiteral("查看记录"),"captureView",NativeIcon::Wave);view_->hide();actions->addWidget(view_);actions->addStretch();
    start_=button(QStringLiteral("开始采集"),"captureStart",NativeIcon::Source);start_->setMinimumWidth(132);start_->setProperty("primary",true);cancel_=button(QStringLiteral("取消"),"captureCancel",NativeIcon::Left);cancel_->setFixedWidth(88);actions->addWidget(cancel_);actions->addWidget(start_);bottom->addLayout(actions);layout->addWidget(actionBox);
    process_=new QProcess(this);process_->setProcessChannelMode(QProcess::SeparateChannels);
    program_=QCoreApplication::applicationDirPath()+"/vibration_capture_service";
    timer_=new QTimer(this);timer_->setInterval(250);
    connect(rate_,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this]{estimate();});
    connect(duration_,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{estimate();});
    connect(browse_,&QPushButton::clicked,this,[this]{const QString dir=QFileDialog::getExistingDirectory(this,QStringLiteral("选择可写保存目录"),directory_->text());if(!dir.isEmpty()){directory_->setText(dir);estimate();}});
    connect(start_,&QPushButton::clicked,this,&AcquisitionPanel::startCapture);
    connect(cancel_,&QPushButton::clicked,this,[this]{if(active_&&!jobId_.isEmpty()&&!cancelling_){cancelling_=true;request("cancel",{{"job_id",jobId_}});state_->setText(QStringLiteral("正在取消"));controls();}});
    connect(retry_,&QPushButton::clicked,this,&AcquisitionPanel::connectService);
    connect(view_,&QPushButton::clicked,this,[this]{if(!analysisBusy_&&!completedPath_.isEmpty())emit captureReady(completedPath_);});
    connect(process_,&QProcess::started,this,[this]{timer_->start();request("capabilities");});
    connect(process_,&QProcess::readyReadStandardOutput,this,&AcquisitionPanel::receive);
    connect(process_,&QProcess::readyReadStandardError,this,[this]{process_->readAllStandardError();});
    connect(process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError){if(!stopping_)fail(QStringLiteral("采集服务未启动：%1").arg(process_->errorString()));});
    connect(process_,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this](int,QProcess::ExitStatus){if(!stopping_)fail(QStringLiteral("采集服务已退出，请重新连接"));});
    connect(timer_,&QTimer::timeout,this,[this]{
        const auto now=QDateTime::currentMSecsSinceEpoch();
        for(auto it=pending_.cbegin();it!=pending_.cend();++it)if(now>it->deadline){fail(QStringLiteral("设备响应超时，请重新连接"));return;}
        bool polling=false;for(auto it=pending_.cbegin();it!=pending_.cend();++it)polling|=it->command=="status";
        if(active_&&!jobId_.isEmpty()&&!polling)request("status",{{"job_id",jobId_}});
    });estimate();controls();
}
AcquisitionPanel::~AcquisitionPanel(){stopService();}
void AcquisitionPanel::showEvent(QShowEvent* e){QWidget::showEvent(e);if(sequence_==0)connectService();}
void AcquisitionPanel::setServiceCommand(const QString& program,const QStringList& args){stopService();program_=program;arguments_=args;available_=false;active_=false;completedPath_.clear();view_->hide();controls();if(isVisible())connectService();}
void AcquisitionPanel::setAnalysisBusy(bool busy){analysisBusy_=busy;controls();}
void AcquisitionPanel::stopService(){
    stopping_=true;timer_->stop();pending_.clear();buffer_.clear();
    if(process_->state()!=QProcess::NotRunning){process_->closeWriteChannel();if(!process_->waitForFinished(1500)){process_->terminate();if(!process_->waitForFinished(500)){process_->kill();process_->waitForFinished(500);}}}
    stopping_=false;
}
void AcquisitionPanel::connectService(){if(active_)return;stopService();available_=false;state_->setText(QStringLiteral("正在连接"));detail({});source_->setText(QStringLiteral("检查本机采集设备"));controls();process_->start(program_,arguments_);}
void AcquisitionPanel::detail(const QString& text){details_->setText(text.left(240));details_->setToolTip(text.left(2048));}
void AcquisitionPanel::fail(const QString& text){available_=false;active_=false;cancelling_=false;jobId_.clear();state_->setText(QStringLiteral("设备不可用"));detail(text);source_->setText(QStringLiteral("已存记录仍可打开分析"));stopService();controls();}
void AcquisitionPanel::request(const QString& command,QJsonObject fields){
    if(process_->state()!=QProcess::Running)return;
    const QString id=QString("gui_%1").arg(++sequence_);fields["schema"]=schema;fields["request_id"]=id;fields["command"]=command;
    pending_.insert(id,{command,QDateTime::currentMSecsSinceEpoch()+8000});process_->write(QJsonDocument(fields).toJson(QJsonDocument::Compact)+'\n');
}
void AcquisitionPanel::receive(){
    if(stopping_)return;buffer_+=process_->readAllStandardOutput();
    while(true){const int end=buffer_.indexOf('\n');if(end<0){if(buffer_.size()>8192)fail(QStringLiteral("采集服务返回过长消息"));return;}
        if(end>8192){fail(QStringLiteral("采集服务返回过长消息"));return;}const auto line=buffer_.left(end);buffer_.remove(0,end+1);
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(line,&error);if(error.error!=QJsonParseError::NoError||!doc.isObject()){fail(QStringLiteral("采集服务消息格式错误"));return;}message(doc.object());if(process_->state()!=QProcess::Running)return;
    }
}
void AcquisitionPanel::message(const QJsonObject& o){
    if(o.value("schema").toString()!=schema){fail(QStringLiteral("采集服务版本不匹配"));return;}
    if(o.value("type")=="event"){if(o.value("event")!="job_status"){fail(QStringLiteral("未知设备事件"));return;}if(active_&&o.value("job_id").toString()==jobId_)jobStatus(o);return;}
    if(o.value("type")!="response"||!o.value("request_id").isString()){fail(QStringLiteral("无效设备响应"));return;}
    const QString id=o.value("request_id").toString();if(!pending_.contains(id))return;
    const QString command=pending_.take(id).command;
    if(o.value("command").toString()!=command||!o.value("ok").isBool()){fail(QStringLiteral("设备响应与请求不匹配"));return;}
    if(!o.value("ok").toBool()){
        const auto err=o.value("error").toObject();detail(err.value("code").toString()+": "+err.value("message").toString());
        if(command=="capabilities"||command=="status"||command=="cancel"){fail(details_->text());return;}
        active_=false;state_->setText(QStringLiteral("未开始"));controls();return;
    }
    if(!o.value("data").isObject()){fail(QStringLiteral("设备响应缺少内容"));return;}const auto d=o.value("data").toObject();
    if(command=="capabilities"){capabilities(d);return;}
    if(command=="validate_plan"){
        if(!d.value("valid").toBool()||!d.value("ready").toBool()){active_=false;capabilities(d.value("capabilities").toObject());return;}
        request("start",{{"plan",planned_}});return;
    }
    if(command=="start"){
        const auto id=d.value("job_id").toString();if(id.isEmpty()||id.size()>128||d.value("state")!="preparing"){fail(QStringLiteral("无效采集任务"));return;}jobId_=id;state_->setText(QStringLiteral("准备采集"));controls();return;
    }
    if(command=="status"){if(active_&&d.value("job_id").toString()==jobId_)jobStatus(d);return;}
    if(command=="cancel"){if(d.value("job_id").toString()!=jobId_){fail(QStringLiteral("取消响应不匹配"));return;}}
}
void AcquisitionPanel::capabilities(const QJsonObject& d){
    if(!d.value("available").isBool()||!d.value("hardware_confirmed").isBool()||!d.value("synthetic").isBool()){fail(QStringLiteral("设备能力信息无效"));return;}
    available_=d.value("available").toBool();
    const bool confirmed=d.value("hardware_confirmed").toBool(),synthetic=d.value("synthetic").toBool();
    if(available_&&!confirmed&&!synthetic){fail(QStringLiteral("设备未经确认"));return;}
    if(available_){
        auto rates=d.value("supported_rates_hz").toArray();if(rates.isEmpty()){fail(QStringLiteral("设备无可用采样率"));return;}
        const int selected=rate_->currentData().toInt();rate_->clear();for(int r:{50000,25000,10000,5000})if(rates.contains(r))rate_->addItem(QString::number(r/1000)+" kHz",r);
        if(!rate_->count()){fail(QStringLiteral("采样率不受支持"));return;}int index=rate_->findData(selected);rate_->setCurrentIndex(index<0?0:index);
        if(!integer(d.value("adc_range_uv"),1,10000000)||!integer(d.value("clock_hz"),1,1000000000)){fail(QStringLiteral("设备量程或时钟无效"));return;}
        device_->setText(QStringLiteral("ADC 1 / 3 / 5  ·  ±%1 V  ·  %2 MHz").arg(d.value("adc_range_uv").toInt()/1e6,0,'g',3).arg(d.value("clock_hz").toInt()/1e6,0,'g',3));
    }
    state_->setText(available_?QStringLiteral("就绪"):QStringLiteral("设备未就绪"));
    source_->setText(synthetic?QStringLiteral("合成自检 · 不代表机械测量"):QStringLiteral("ADC 原始记录 · 传感器与标定需确认"));
    detail(available_?QString():d.value("reason").toString());controls();estimate();
}
QJsonObject AcquisitionPanel::plan() const{return {{"sample_rate_hz",rate_->currentData().toInt()},{"duration_seconds",int(duration_->value())},{"pulses_per_rev",int(ppr_->value())},{"output_directory",directory_->text()}};}
void AcquisitionPanel::startCapture(){
    if(active_||!available_)return;
    const auto path=directory_->text();
    // Creation occurs only on an explicit Start. All other paths must be chosen
    // from existing directories; the service separately rejects symlink chains.
    if(path=="/tmp/vibration-captures"&&!QDir(path).exists()&&!QDir().mkpath(path)){detail(QStringLiteral("无法创建临时保存目录"));return;}
    active_=true;cancelling_=false;jobId_.clear();completedPath_.clear();view_->hide();detail({});progress_->setValue(0);progressText_->setText(QStringLiteral("正在检查参数与目录"));state_->setText(QStringLiteral("准备采集"));planned_=plan();controls();request("validate_plan",{{"plan",planned_}});
}
void AcquisitionPanel::jobStatus(const QJsonObject& d){
    const QString state=d.value("state").toString();
    const QStringList states{"preparing","running","validating","saving","completed","failed","cancelled"};
    if(!states.contains(state)||!integer(d.value("records_completed"),0,2097024)||!integer(d.value("requested_records"),1,2097024)||!d.value("synthetic").isBool()||!d.value("hardware_confirmed").isBool()){fail(QStringLiteral("采集状态无效"));return;}
    const int done=d.value("records_completed").toInt(),total=d.value("requested_records").toInt();if(done>total){fail(QStringLiteral("采集记录数异常"));return;}
    progress_->setValue(int(qint64(done)*1000/total));progressText_->setText(QStringLiteral("%1 / %2 条记录").arg(done).arg(total));
    const QStringList names{QStringLiteral("准备采集"),QStringLiteral("正在采集"),QStringLiteral("校验记录"),QStringLiteral("保存记录"),QStringLiteral("采集完成"),QStringLiteral("采集失败"),QStringLiteral("已取消")};state_->setText(cancelling_&&state!="completed"&&state!="failed"&&state!="cancelled"?QStringLiteral("正在取消"):names[states.indexOf(state)]);
    source_->setText(d.value("synthetic").toBool()?QStringLiteral("合成自检 · 不代表机械测量"):QStringLiteral("ADC 原始记录 · 未确认信号与标定"));
    if(state=="completed"){
        const QString path=d.value("output_path").toString();QFileInfo info(path);
        if(done!=total||!info.isAbsolute()||!info.isFile()||info.isSymLink()||info.absolutePath()!=QFileInfo(planned_.value("output_directory").toString()).absoluteFilePath()||info.suffix().compare("BIN",Qt::CaseInsensitive)!=0){fail(QStringLiteral("完成记录路径或数量无效"));return;}
        completedPath_=info.absoluteFilePath();view_->show();detail(info.fileName());
    }
    if(state=="failed")detail(d.value("error_code").toString()+": "+d.value("error_message").toString());
    if(state=="completed"||state=="failed"||state=="cancelled"){active_=false;cancelling_=false;controls();}
}
void AcquisitionPanel::controls(){
    rate_->setEnabled(!active_);duration_->setEnabled(!active_);ppr_->setEnabled(!active_);browse_->setEnabled(!active_);
    start_->setEnabled(available_&&!active_);cancel_->setEnabled(active_&&!jobId_.isEmpty()&&!cancelling_);retry_->setEnabled(!active_);
    view_->setEnabled(!analysisBusy_&&!completedPath_.isEmpty());setProperty("captureActive",active_);setProperty("captureAvailable",available_);
}
void AcquisitionPanel::estimate(){const qint64 n=qint64(rate_->currentData().toInt())*int(duration_->value());const bool temporary=directory_->text()=="/tmp/vibration-captures"||directory_->text().startsWith("/tmp/");estimate_->setText(QStringLiteral("%1 条 · %2 MiB%3").arg(n).arg((4096+n*32)/1048576.0,0,'f',1).arg(temporary?QStringLiteral("\n临时目录，断电后不保留"):QString()));}
}
