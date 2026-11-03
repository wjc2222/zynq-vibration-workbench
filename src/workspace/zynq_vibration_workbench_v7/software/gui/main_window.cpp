#include "main_window.hpp"
#include "acquisition_panel.hpp"
#include "plot_widget.hpp"
#include "view_math.hpp"
#include "native_icons.hpp"
#include <QApplication>
#include <QDir>
#include <QListWidget>
#include <QStackedWidget>
#include <QScrollArea>
#include <QFile>
#include <QSaveFile>
#include <QButtonGroup>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStringList>
#include <QTextBrowser>
#include <QTabWidget>
#include <QTabBar>
#include <QScroller>
#include <QThread>
#include <QVBoxLayout>
#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <filesystem>
#include <utility>

namespace vibration::gui {
namespace {
const std::array<QColor, 3> colors{QColor("#2c67c5"), QColor("#238575"), QColor("#bb7a28")};
const std::array<QString, 3> channelNames{QStringLiteral("CH1"), QStringLiteral("CH2"), QStringLiteral("CH3")};

QString text(const std::string& value) { return QString::fromUtf8(value.data(), int(value.size())); }
QString format(double value, int places = 3) { return QString::number(value, 'f', places); }
QString optionalNumber(const std::optional<double>& value, int places = 1) {
    return value && std::isfinite(*value) ? format(*value, places) : QStringLiteral("未确定");
}
QString candidateName(const std::string& label) {
    if(label=="normal")return QStringLiteral("正常状态候选");
    if(label=="imbalance")return QStringLiteral("不平衡候选");
    if(label=="misalignment")return QStringLiteral("不对中候选");
    if(label=="looseness")return QStringLiteral("松动候选");
    return text(label)+QStringLiteral("候选");
}
QString featureName(const std::string& feature) {
    if(feature=="mean")return QStringLiteral("均值");
    if(feature=="rms")return QStringLiteral("总 RMS");
    if(feature=="ac_rms")return QStringLiteral("交流 RMS");
    if(feature=="abs_peak")return QStringLiteral("绝对峰值");
    if(feature=="peak_to_peak")return QStringLiteral("峰峰值");
    if(feature=="crest_factor")return QStringLiteral("峰值因子");
    if(feature=="pearson_kurtosis")return QStringLiteral("峭度");
    if(feature=="order1_rms")return QStringLiteral("1X RMS");
    if(feature=="order2_rms")return QStringLiteral("2X RMS");
    if(feature=="order3_rms")return QStringLiteral("3X RMS");
    return text(feature);
}
QString gateName(const std::string& id) {
    if(id=="setup")return QStringLiteral("台架与安装标识");
    if(id=="source")return QStringLiteral("来源声明");
    if(id=="calibration")return QStringLiteral("实际标定");
    if(id=="analysis_configuration")return QStringLiteral("分析参数一致性");
    if(id=="ppr")return QStringLiteral("每转脉冲数");
    if(id=="record_count")return QStringLiteral("记录长度");
    if(id=="adc_endpoints")return QStringLiteral("削顶与饱和");
    if(id=="finite_features")return QStringLiteral("特征完整性");
    if(id=="required_features")return QStringLiteral("配置所需特征");
    if(id=="comparison_domain")return QStringLiteral("规则计算条件");
    if(id=="speed")return QStringLiteral("测速与工况范围");
    return text(id);
}
QString gateDetail(const DetectionGate& gate) {
    if(gate.id=="setup")return QStringLiteral("当前台架与安装标识须与配置完全一致。");
    if(gate.id=="source")return QStringLiteral("当前与基线须确认为机械振动；合成标记或无效来源说明会阻断。");
    if(gate.id=="calibration")return QStringLiteral("当前与基线均须明确确认实际标定，幅值单位为 g。");
    if(gate.id=="analysis_configuration")return QStringLiteral("灵敏度、有符号增益、采样率、ADC 量程、通道映射、FFT 长度及频率范围须一致。");
    if(gate.id=="ppr")return QStringLiteral("当前与基线须具有相同且已知的有效 PPR。");
    if(gate.id=="record_count")return QStringLiteral("当前采样点数须达到配置的最低要求。");
    if(gate.id=="adc_endpoints")return QStringLiteral("三通道均不得包含 ADC 端点码。");
    if(gate.id=="finite_features")return QStringLiteral("记录时长须为正，核心时域特征须为有效数值。");
    if(gate.id=="required_features")return QStringLiteral("基线与规则引用的特征均须有效；所需转频带缺失时暂停判定。");
    if(gate.id=="comparison_domain")return QStringLiteral("规则所需的差值和比值均须可计算；比值的基线不能为零。");
    if(gate.id=="speed")return QStringLiteral("RPM 范围与波动须符合配置；有效测速至少 90%，转速变异系数不超过 5%，且存在可观测边沿。");
    return text(gate.detail);
}
QString conditionState(const std::string& state) {
    if(state=="not_evaluated_quality_gate")return QStringLiteral("条件未通过，未执行规则");
    if(state=="feature_unavailable")return QStringLiteral("特征不可用");
    if(state=="comparison_unavailable")return QStringLiteral("无法计算对比值");
    if(state=="within_interval")return QStringLiteral("满足区间");
    if(state=="outside_interval")return QStringLiteral("不满足区间");
    return text(state);
}
QString translatedEvidence(const std::string& input) {
    const QString s = text(input);
    if (s == "CAPDDR2 integrity, index continuity and sample timestamps passed validation.")
        return QStringLiteral("文件 CRC、记录序号与采样时间连续性校验通过。");
    if (s == "Time-domain features and window-averaged spectra are measured from this record.")
        return QStringLiteral("已计算本次记录的时域统计与分段平均频谱。");
    if (s == "1X/2X/3X band energy is referenced to measured SPEED and the stated PPR.")
        return QStringLiteral("1X / 2X / 3X 频带以有效测速和指定 PPR 为参考。");
    if (s == "One or more channels contain ADC endpoint codes; inspect saturation before interpretation.")
        return QStringLiteral("发现 ADC 满量程端点码，需先排查削顶或输入饱和。");
    if (s == "Acceleration uses configured nominal sensitivity/gain; calibration is not confirmed.")
        return QStringLiteral("加速度按名义灵敏度与前端增益换算，尚未完成标定。");
    if (s == "V1/V3/V5 are storage channels; physical X/Y/Z direction requires a verified wiring record.")
        return QStringLiteral("CH1 / CH2 / CH3 对应 V1 / V3 / V5，物理轴向尚需接线确认。");
    if (s == "The file proves digital continuity, not sensor validity or analog anti-alias performance.")
        return QStringLiteral("数字校验不验证传感器状态与模拟抗混叠性能。");
    if (s == "PPR is unknown; only pulse Hz is available and RPM/order features are disabled.")
        return QStringLiteral("每转脉冲数未知：显示脉冲频率，暂停 RPM 与转频分析。");
    if (s == "Less than 90 percent of SPEED snapshots are valid; order features are disabled.")
        return QStringLiteral("有效测速记录不足 90%，暂不解释转频能量。");
    if (s == "Speed variation exceeds 5 percent; fixed-frequency order bands are disabled. Order tracking is not implemented.")
        return QStringLiteral("转速波动较大，暂停固定转频带分析；当前未实现阶次跟踪。");
    if (s == "No trained classifier or validated machine-specific thresholds are installed; no fault category or accuracy is claimed.")
        return QStringLiteral("尚未装载经验证的分类器或台架阈值，故障类别待确认。");
    if (s == "A 1X or 2X peak alone cannot uniquely identify imbalance, misalignment or looseness.")
        return QStringLiteral("单个 1X / 2X 谱峰不能唯一确定不平衡、不对中或松动。");
    if (s == "Synthetic source sidecar: development signal only; this record is not a physical machine measurement.")
        return QStringLiteral("来源说明标记为合成数据：仅供软件验证，不是实际机械测量。");
    if (s == "Source sidecar is invalid or unreadable; source confirmation is disabled until it is resolved.")
        return QStringLiteral("来源说明无效或无法读取：修复前不能确认数据来源。");
    if (s == "Speed display merges multiple missing runs within a display bucket; intervening valid fragments are hidden. Full-record statistics are unchanged.")
        return QStringLiteral("测速概览合并了桶内多段缺测，隐藏中间有效碎片；全记录统计不受影响。");
    return s;
}

class AnalysisThread final : public QThread {
public:
    AnalysisThread(QString path, AnalysisConfig config, QObject* parent)
        : QThread(parent), path_(std::move(path)), config_(std::move(config)) {}
    std::unique_ptr<AnalysisResult> result;
    std::shared_ptr<Capture> capture;
    QString error;
protected:
    void run() override {
        try {
            capture = std::make_shared<Capture>(load_capture(path_.toUtf8().toStdString()));
            result = std::make_unique<AnalysisResult>(analyze(*capture, config_));
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        } catch (...) {
            error = QStringLiteral("分析发生未知错误。");
        }
    }
private:
    QString path_;
    AnalysisConfig config_;
};

// Pointer entry opens a small native keypad; setValue and keyboard focus never
// open dialogs. The real spin box remains the sole owner of its numeric value.
class TouchDoubleSpinBox : public QDoubleSpinBox {
public:
    explicit TouchDoubleSpinBox(QWidget* parent=nullptr) : QDoubleSpinBox(parent) {
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        lineEdit()->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject* watched,QEvent* event) override {
        if(watched==lineEdit()&&event->type()==QEvent::MouseButtonPress&&isEnabled()){
            if(static_cast<QMouseEvent*>(event)->button()==Qt::LeftButton){openKeypad();event->accept();return true;}
        }
        if(watched==lineEdit()&&event->type()==QEvent::MouseButtonRelease)return true;
        return QDoubleSpinBox::eventFilter(watched,event);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if(event->button()==Qt::LeftButton&&isEnabled()){openKeypad();event->accept();return;}
        QDoubleSpinBox::mousePressEvent(event);
    }
private:
    void openKeypad() {
        QDialog pad(window());pad.setObjectName("numericKeypadDialog");pad.setWindowTitle(accessibleName().isEmpty()?QStringLiteral("输入数值"):accessibleName());pad.resize(444,480);
        auto* layout=new QVBoxLayout(&pad);layout->setContentsMargins(20,18,20,18);layout->setSpacing(10);
        auto* title=new QLabel(pad.windowTitle());title->setStyleSheet("font-size:20px;font-weight:600;color:#1d2b3b;");layout->addWidget(title);
        auto* limit=new QLabel(QStringLiteral("%1 — %2%3").arg(QString::number(minimum(),'g',10),QString::number(maximum(),'g',10),suffix()));limit->setStyleSheet("font-size:12px;color:#78899c;");layout->addWidget(limit);
        auto* display=new QLineEdit;display->setObjectName("numericValueDisplay");display->setMaxLength(32);display->setMinimumHeight(56);display->setAlignment(Qt::AlignRight);display->setStyleSheet("font-size:28px;padding:0 12px;background:white;border:1px solid #adc5e9;border-radius:6px;");
        QString initial=QString::number(value(),'f',decimals());if(initial.contains('.')){while(initial.endsWith('0'))initial.chop(1);if(initial.endsWith('.'))initial.chop(1);}display->setText(initial);display->selectAll();layout->addWidget(display);
        auto* error=new QLabel;error->setObjectName("numericValidationError");error->setMinimumHeight(18);error->setStyleSheet("color:#ae5149;font-size:12px;");error->setWordWrap(true);layout->addWidget(error);
        auto* keys=new QGridLayout;keys->setSpacing(8);bool replace=true;
        connect(display,&QLineEdit::textEdited,&pad,[&replace]{replace=false;});
        const auto key=[&](const QString& label,const QString& id,int row,int col){auto* button=new QPushButton(label);button->setObjectName(id);button->setMinimumSize(80,56);keys->addWidget(button,row,col);return button;};
        const std::array<int,9> digits{7,8,9,4,5,6,1,2,3};
        const auto append=[&](const QString& digit){error->clear();if(replace){display->clear();replace=false;}if(display->text().size()<32)display->setText(display->text()+digit);};
        for(int i=0;i<9;++i){const auto digit=QString::number(digits[i]);auto* button=key(digit,"numericKey"+digit,i/3,i%3);connect(button,&QPushButton::clicked,&pad,[append,digit]{append(digit);});}
        auto* zero=key("0","numericKey0",3,1);connect(zero,&QPushButton::clicked,&pad,[append]{append("0");});
        auto* decimal=key(".","numericKeyDecimal",3,2);decimal->setEnabled(decimals()>0);connect(decimal,&QPushButton::clicked,&pad,[&]{error->clear();if(replace){display->setText("0.");replace=false;}else if(!display->text().contains('.'))display->setText(display->text().isEmpty()?QStringLiteral("0."):display->text()+'.');});
        auto* sign=key(QStringLiteral("正负"),"numericKeySign",3,0);sign->setEnabled(minimum()<0);connect(sign,&QPushButton::clicked,&pad,[&]{error->clear();QString current=display->text();if(current.startsWith('-'))current.remove(0,1);else current.prepend('-');display->setText(current);replace=false;});
        auto* back=key(QStringLiteral("退格"),"numericKeyBackspace",0,3);connect(back,&QPushButton::clicked,&pad,[&]{error->clear();QString current=display->text();if(replace)current.clear();else current.chop(1);display->setText(current);replace=false;});
        auto* clear=key(QStringLiteral("清空"),"numericKeyClear",1,3);connect(clear,&QPushButton::clicked,&pad,[&]{display->clear();error->clear();replace=false;});
        auto* cancel=key(QStringLiteral("取消"),"numericKeyCancel",2,3);connect(cancel,&QPushButton::clicked,&pad,&QDialog::reject);
        auto* confirm=key(QStringLiteral("确认"),"numericKeyConfirm",3,3);confirm->setProperty("primary",true);double accepted=value();
        const auto accept=[&]{bool ok=false;const double entered=display->text().toDouble(&ok);if(!ok||!std::isfinite(entered)||entered<minimum()||entered>maximum()){error->setText(QStringLiteral("请输入允许范围内的有限数值"));return;}accepted=entered;pad.accept();};
        connect(confirm,&QPushButton::clicked,&pad,accept);connect(display,&QLineEdit::returnPressed,&pad,accept);layout->addLayout(keys);
        if(pad.exec()==QDialog::Accepted)setValue(accepted);
    }
};
class PprSpinBox final : public TouchDoubleSpinBox {
protected:
    QString textFromValue(double value) const override {
        QString result = QString::number(value, 'f', 6);
        while (result.endsWith('0')) result.chop(1);
        if (result.endsWith('.')) result.chop(1);
        return result;
    }
};
// Preserve the full user-supplied profile identity while sizing its single-line
// summary to the actual label width, including after a screen resize.
class ElidedProfileLabel final : public QLabel {
public:
    ElidedProfileLabel() {
        setTextFormat(Qt::PlainText);setWordWrap(false);setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    }
    void setFullText(const QString& value,const QString& detail) {
        fullText_=value;setToolTip(detail);updateElision();
    }
protected:
    void resizeEvent(QResizeEvent* event) override {QLabel::resizeEvent(event);updateElision();}
    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if(event->type()==QEvent::FontChange||event->type()==QEvent::StyleChange)updateElision();
    }
private:
    QString fullText_;
    void updateElision() {
        const auto visible=fontMetrics().elidedText(fullText_,Qt::ElideMiddle,std::max(0,contentsRect().width()-2*margin()));
        if(visible!=text())QLabel::setText(visible);
    }
};
class TouchCheckBox final : public QCheckBox {
public:
    explicit TouchCheckBox(const QString& label,const QColor& color) : QCheckBox(label), color_(color) { setFixedSize(56, 44); }
protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
        const QColor border=hasFocus()?QColor("#2c67c5"):isChecked()?QColor("#b7cbe6"):QColor("#d8e0ea");
        p.setPen(QPen(border,1));p.setBrush(isChecked()?QColor("#f3f7fd"):QColor("#ffffff"));
        p.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),5,5);
        QFont f=font();f.setPixelSize(12);f.setWeight(QFont::DemiBold);p.setFont(f);
        p.setPen(isEnabled()?(isChecked()?color_:QColor("#78899c")):QColor("#9aa7b7"));
        p.drawText(rect(),Qt::AlignCenter,text());
    }
private:
    QColor color_;
};
}

MainWindow::MainWindow(const AnalysisConfig& initialConfig, QWidget* parent)
    : QMainWindow(parent), config_(initialConfig) {
    setWindowTitle(QStringLiteral("振动工作台"));
    resize(1024, 600); setMinimumSize(1024, 600);
    config_.spectrum_max_hz = 400.0;
    setStyleSheet(QStringLiteral(
        "QMainWindow,QDialog{background:#f6f7f8;color:#25282c;font-size:14px;}"
        "QLabel{color:#25282c;background:transparent;}"
        "QFrame#navigationRail{background:#f6f7f8;border:0;}"
        "QFrame#workspaceSurface{background:#ffffff;border:0;}"
        "QFrame#globalRail{background:#f7f8fa;border:0;border-right:1px solid #eceef1;}"
        "QFrame#contextSidebar{background:#fafbfc;border:0;border-right:1px solid #eceef1;}"
        "QPushButton[rail=\"true\"]{min-height:44px;padding:0;border:0;background:transparent;border-radius:8px;}"
        "QPushButton[rail=\"true\"]:checked{background:#e8ebef;}"
        "QFrame#recordHeader{background:transparent;border:0;border-bottom:1px solid #f0f1f3;}"
        "QFrame#workbenchCard{background:#ffffff;border:0;border-radius:10px;}"
        "QFrame#plotSurface{background:#ffffff;border:1px solid #eceef1;border-radius:9px;}"
        "QPushButton{min-height:42px;padding:0 12px;border:1px solid #e5e7eb;border-radius:8px;background:#ffffff;color:#34383d;font-size:14px;}"
        "QPushButton:hover{background:#f6f7f9;border-color:#cfd4db;}"
        "QPushButton:focus{border-color:#a3aab5;}"
        "QPushButton:pressed{background:#eceff3;border-color:#a9b1bc;}"
        "QPushButton:disabled{color:#aeb3bc;background:#fafbfc;border-color:#eff1f4;}"
        "QPushButton[primary=\"true\"]{background:#262a30;border-color:#262a30;color:white;}"
        "QPushButton[primary=\"true\"]:hover{background:#3b414a;}"
        "QPushButton[primary=\"true\"]:pressed{background:#171b20;}"
        "QPushButton[primary=\"true\"]:disabled{background:#e9ecf0;color:#a5acb6;border-color:#e9ecf0;}"
        "QPushButton[segment=\"true\"]{border-color:transparent;background:transparent;}"
        "QPushButton[segment=\"true\"]:checked{background:#eef0f3;color:#262a30;border-color:transparent;font-weight:600;}"
        "QPushButton[sidebar=\"true\"]{min-height:44px;text-align:left;background:transparent;border:0;padding:0 12px;}"
        "QPushButton[sidebar=\"true\"]:checked{background:#e8ebef;}"
        "QTextBrowser{background:#ffffff;color:#526479;border:0;padding:10px;}"
        "QListWidget{background:transparent;border:0;padding:0;outline:0;}"
        "QListWidget::item{min-height:56px;padding:0 12px;border:0;border-bottom:1px solid #f1f3f6;color:#3c424a;}"
        "QListWidget::item:selected{background:#edf0f4;color:#303843;}"
        "QListWidget#sidebarRecords::item{min-height:44px;padding:0 10px;border:0;border-radius:7px;font-size:12px;}"
        "QListWidget::item:disabled{color:#78899c;}"
        "QComboBox,QDoubleSpinBox,QLineEdit{min-height:44px;padding:0 12px;background:#fbfcfd;border:1px solid #e5e9ef;border-radius:8px;color:#30363e;}"
        "QComboBox::drop-down{border:0;width:26px;}"
        "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:24px;}"
        "QCheckBox{min-height:42px;spacing:8px;}QCheckBox::indicator{width:20px;height:20px;}"
        "QTabWidget::pane{border:0;}QScrollArea{background:transparent;border:0;}"
        "QTabBar::tab{min-height:44px;padding:0 18px;background:white;border:0;color:#747d89;}"
        "QTabBar::tab:selected{background:#eff2f6;color:#303845;}"
        "QScrollBar:vertical{background:transparent;width:14px;margin:0;}"
        "QScrollBar::handle:vertical{background:#dce1e8;min-height:44px;border-radius:6px;margin:3px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:none;}"));
    auto makeButton=[](const QString& label,NativeIcon icon,int width=0) {
        auto* button=new QPushButton(label); button->setIcon(nativeIcon(icon));button->setIconSize(QSize(22,22));
        button->setMinimumHeight(44);if(width)button->setFixedWidth(width);return button;
    };
    auto makeCard=[]() {auto* frame=new QFrame;frame->setObjectName("workbenchCard");frame->setAttribute(Qt::WA_StyledBackground);return frame;};
    auto makeLabel=[](const QString& text,int pixels,const QString& color="#526479",int weight=400) {
        auto* label=new QLabel(text);label->setStyleSheet(QString("font-size:%1px;color:%2;font-weight:%3;").arg(pixels).arg(color).arg(weight));return label;
    };
    auto* central=new QWidget(this);auto* root=new QHBoxLayout(central);root->setContentsMargins(0,0,0,0);root->setSpacing(0);
    auto* rail=new QFrame;rail->setObjectName("globalRail");rail->setFixedWidth(56);
    auto* global=new QVBoxLayout(rail);global->setContentsMargins(6,10,6,10);global->setSpacing(10);
    auto* brand=new QLabel;brand->setFixedSize(44,42);brand->setAlignment(Qt::AlignCenter);brand->setPixmap(nativeIcon(NativeIcon::Wave,QColor("#383d44"),24).pixmap(24,24));global->addWidget(brand);global->addSpacing(10);
    auto* captureMode=makeButton({},NativeIcon::Plus,44);captureMode->setObjectName("deviceCaptureMode");captureMode->setCheckable(true);captureMode->setProperty("rail",true);captureMode->setAccessibleName(QStringLiteral("新建采集"));captureMode->setToolTip(QStringLiteral("新建采集"));global->addWidget(captureMode);
    auto* navGroup=new QButtonGroup(this);navGroup->setExclusive(true);navGroup->addButton(captureMode);
    navigation_[0]=makeButton({},NativeIcon::Folder,44);navigation_[0]->setObjectName("navFiles");navigation_[0]->setProperty("rail",true);navigation_[0]->setCheckable(true);navigation_[0]->setAccessibleName(QStringLiteral("采集记录"));navigation_[0]->setToolTip(QStringLiteral("采集记录"));navigation_[0]->setProperty("pageIndex",0);navGroup->addButton(navigation_[0]);global->addWidget(navigation_[0]);
    connect(navigation_[0],&QPushButton::clicked,this,[this]{findChild<QPushButton*>("storedRecordMode")->click();});
    global->addStretch();parametersButton_=makeButton({},NativeIcon::Settings,44);parametersButton_->setProperty("rail",true);parametersButton_->setObjectName("analysisParameters");parametersButton_->setToolTip(QStringLiteral("分析设置"));parametersButton_->setAccessibleName(QStringLiteral("分析参数"));global->addWidget(parametersButton_);root->addWidget(rail);
    auto* sidebar=new QFrame;contextSidebar_=sidebar;sidebar->setObjectName("contextSidebar");sidebar->setFixedWidth(192);
    auto* nav=new QVBoxLayout(sidebar);nav->setContentsMargins(10,14,10,12);nav->setSpacing(4);
    auto* brandTitle=makeLabel(QStringLiteral("振动分析"),17,"#30343a",600);brandTitle->setContentsMargins(10,0,0,0);brandTitle->setFixedHeight(34);nav->addWidget(brandTitle);nav->addSpacing(18);
    auto* currentHeading=makeLabel(QStringLiteral("当前记录"),11,"#969ba3");currentHeading->setContentsMargins(10,0,0,0);nav->addWidget(currentHeading);
    sidebarRecord_=new ElidedProfileLabel;sidebarRecord_->setObjectName("sidebarCurrentFile");sidebarRecord_->setStyleSheet("font-size:13px;color:#515862;");sidebarRecord_->setContentsMargins(10,0,8,0);sidebarRecord_->setFixedHeight(24);static_cast<ElidedProfileLabel*>(sidebarRecord_)->setFullText(QStringLiteral("尚未打开"),{});nav->addWidget(sidebarRecord_);
    sampleInfo_=makeLabel({},11,"#9098a3");sampleInfo_->setObjectName("sampleInfo");sampleInfo_->setContentsMargins(10,0,0,0);sampleInfo_->setMinimumWidth(0);sampleInfo_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);nav->addWidget(sampleInfo_);nav->addSpacing(14);
    const QStringList names{QStringLiteral("时域波形"),QStringLiteral("频谱分析"),QStringLiteral("转速趋势"),QStringLiteral("故障检测")};
    const QStringList navIds{"navWaveform","navSpectrum","navSpeed","navDetection"};
    const std::array<NativeIcon,4> navIcons{NativeIcon::Wave,NativeIcon::Spectrum,NativeIcon::Speed,NativeIcon::Detection};
    for(int i=1;i<5;++i){navigation_[i]=new NavigationButton(names[i-1],navIcons[i-1]);navigation_[i]->setObjectName(navIds[i-1]);navigation_[i]->setFixedHeight(44);navigation_[i]->setProperty("pageIndex",i);navGroup->addButton(navigation_[i]);nav->addWidget(navigation_[i]);connect(navigation_[i],&QPushButton::clicked,this,[this,i]{pages_->setCurrentIndex(i);});}
    nav->addSpacing(18);auto* recentHeading=makeLabel(QStringLiteral("记录"),11,"#969ba3");recentHeading->setContentsMargins(10,0,0,0);nav->addWidget(recentHeading);
    sidebarFiles_=new QListWidget;sidebarFiles_->setObjectName("sidebarRecords");sidebarFiles_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);sidebarFiles_->setTextElideMode(Qt::ElideMiddle);sidebarFiles_->setMinimumHeight(44);sidebarFiles_->setIconSize(QSize(16,16));nav->addWidget(sidebarFiles_,1);
    connect(sidebarFiles_,&QListWidget::itemClicked,this,[this](QListWidgetItem* item){const QString path=item->data(Qt::UserRole).toString();if(!path.isEmpty()&&!isAnalyzing())openCapture(path);});root->addWidget(sidebar);
    auto* surface=new QFrame;surface->setObjectName("workspaceSurface");auto* workspace=new QVBoxLayout(surface);workspace->setContentsMargins(0,0,0,0);workspace->setSpacing(0);
    auto* header=new QFrame;header->setObjectName("recordHeader");header->setFixedHeight(56);
    auto* headerRow=new QHBoxLayout(header);headerRow->setContentsMargins(10,6,14,6);headerRow->setSpacing(8);
    auto* toggleSidebar=makeButton({},NativeIcon::Sidebar,44);toggleSidebar->setObjectName("toggleSidebar");toggleSidebar->setProperty("rail",true);toggleSidebar->setAccessibleName(QStringLiteral("收起或展开侧栏"));toggleSidebar->setToolTip(QStringLiteral("收起或展开侧栏"));headerRow->addWidget(toggleSidebar);
    connect(toggleSidebar,&QPushButton::clicked,this,[this]{contextSidebar_->setVisible(!contextSidebar_->isVisible());});
    fileLabel_=new ElidedProfileLabel;fileLabel_->setStyleSheet("font-size:14px;color:#333840;font-weight:600;");fileLabel_->setObjectName("currentFile");static_cast<ElidedProfileLabel*>(fileLabel_)->setFullText(QStringLiteral("三轴振动工作台"),{});headerRow->addWidget(fileLabel_,1);
    banner_=new QLabel;banner_->setObjectName("analysisBanner");banner_->setFixedHeight(28);headerRow->addWidget(banner_);
    calibration_=new QLabel(QStringLiteral("未标定"));calibration_->setObjectName("calibrationState");calibration_->setFixedHeight(28);headerRow->addWidget(calibration_);
    exportMenuButton_=makeButton(QStringLiteral("导出"),NativeIcon::Export,88);exportMenuButton_->setObjectName("exportOptions");headerRow->addSpacing(4);headerRow->addWidget(exportMenuButton_);workspace->addWidget(header);
    auto* content=new QWidget;auto* contentLayout=new QVBoxLayout(content);contentLayout->setContentsMargins(16,12,16,10);contentLayout->setSpacing(0);
    pages_=new QStackedWidget;pages_->setObjectName("workspacePages");contentLayout->addWidget(pages_);workspace->addWidget(content,1);root->addWidget(surface,1);setCentralWidget(central);
    struct Page {QVBoxLayout* layout;QHBoxLayout* toolbar;};
    const auto page=[&](const QString& title) {auto* widget=new QWidget;auto* layout=new QVBoxLayout(widget);layout->setContentsMargins(0,0,0,0);layout->setSpacing(12);auto* tools=new QHBoxLayout;tools->setSpacing(6);auto* heading=makeLabel(title,18,"#30363e",600);heading->setFixedWidth(48);tools->addWidget(heading);layout->addLayout(tools);pages_->addWidget(widget);return Page{layout,tools};};

    auto filesPage=page(QStringLiteral("记录"));
    auto* storedMode=makeButton(QStringLiteral("已存记录"),NativeIcon::Record,120);storedMode->setParent(this);storedMode->hide();storedMode->setObjectName("storedRecordMode");storedMode->setCheckable(true);storedMode->setChecked(true);
    directoryButton_=makeButton(QStringLiteral("目录"),NativeIcon::Folder,96);directoryButton_->setObjectName("chooseDirectory");
    refreshButton_=makeButton({},NativeIcon::Refresh,44);refreshButton_->setObjectName("refreshFiles");refreshButton_->setToolTip(QStringLiteral("刷新目录"));
    openButton_=makeButton(QStringLiteral("打开文件"),NativeIcon::Record,128);openButton_->setObjectName("primary");
    demoButton_=makeButton(QStringLiteral("演示记录"),NativeIcon::Wave,128);demoButton_->setObjectName("openDemo");
    filesPage.toolbar->addWidget(directoryButton_);filesPage.toolbar->addWidget(refreshButton_);filesPage.toolbar->addStretch();filesPage.toolbar->addWidget(openButton_);filesPage.toolbar->addWidget(demoButton_);
    auto* fileColumns=new QHBoxLayout;fileColumns->setSpacing(12);
    auto* listCard=makeCard();auto* listLayout=new QVBoxLayout(listCard);listLayout->setContentsMargins(12,12,12,12);listLayout->setSpacing(8);
    directoryLabel_=makeLabel({},12,"#78899c");directoryLabel_->setObjectName("directoryLabel");directoryLabel_->setTextFormat(Qt::PlainText);directoryLabel_->setMinimumHeight(28);directoryLabel_->setWordWrap(true);listLayout->addWidget(directoryLabel_);
    files_=new QListWidget;files_->setObjectName("captureFiles");files_->setIconSize(QSize(24,24));files_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);files_->setTextElideMode(Qt::ElideMiddle);listLayout->addWidget(files_,1);fileColumns->addWidget(listCard,1);
    auto* metadataCard=makeCard();metadataCard->setFixedWidth(280);auto* metadata=new QVBoxLayout(metadataCard);metadata->setContentsMargins(20,20,20,16);metadata->setSpacing(14);
    fileMetadata_=new QLabel;fileMetadata_->setObjectName("fileMetadata");fileMetadata_->setWordWrap(true);fileMetadata_->setAlignment(Qt::AlignTop);metadata->addWidget(fileMetadata_,1);
    recent_=makeLabel({},12,"#78899c");recent_->setObjectName("recentFiles");recent_->setTextFormat(Qt::RichText);recent_->setWordWrap(true);metadata->addWidget(recent_);fileColumns->addWidget(metadataCard);
    auto* recordsBody=new QWidget;recordsBody->setLayout(fileColumns);fileColumns->setContentsMargins(0,0,0,0);
    recordModes_=new QStackedWidget;recordModes_->setObjectName("recordModes");recordModes_->addWidget(recordsBody);
    acquisition_=new AcquisitionPanel([]{return new TouchDoubleSpinBox;});recordModes_->addWidget(acquisition_);filesPage.layout->addWidget(recordModes_,1);
    auto* recordHeading=qobject_cast<QLabel*>(filesPage.toolbar->itemAt(0)->widget());
    const auto selectMode=[this,storedMode,captureMode,recordHeading](int mode){pages_->setCurrentIndex(0);recordModes_->setCurrentIndex(mode);storedMode->setChecked(mode==0);captureMode->setChecked(mode==1);navigation_[0]->setChecked(mode==0);recordHeading->setText(mode==0?QStringLiteral("记录"):QStringLiteral("采集"));for(auto* b:{directoryButton_,refreshButton_,openButton_,demoButton_})b->setVisible(mode==0);};
    connect(storedMode,&QPushButton::clicked,this,[selectMode]{selectMode(0);});connect(captureMode,&QPushButton::clicked,this,[selectMode]{selectMode(1);});
    connect(acquisition_,&AcquisitionPanel::captureReady,this,[this](const QString& path){if(!isAnalyzing()){openCapture(path);setPage("waveform");}});
    connect(files_,&QListWidget::itemActivated,this,[this](QListWidgetItem* item){if(!item->data(Qt::UserRole).toString().isEmpty())openCapture(item->data(Qt::UserRole).toString());});
    connect(files_,&QListWidget::itemClicked,this,[this](QListWidgetItem* item){if(!item->data(Qt::UserRole).toString().isEmpty())openCapture(item->data(Qt::UserRole).toString());});
    connect(recent_,&QLabel::linkActivated,this,[this](const QString& link){bool ok=false;const int i=link.toInt(&ok);if(ok&&i>=0&&i<recentPaths_.size())openCapture(recentPaths_[i]);});

    auto wavePage=page(QStringLiteral("时域"));
    waveRangePicker_=makeButton(QStringLiteral("时间区间"),NativeIcon::Clock,208);waveRangePicker_->setObjectName("waveRangePicker");waveRangePicker_->setToolTip(QStringLiteral("预览区间不改变全记录分析与导出；点击编辑起止时间。"));wavePage.toolbar->addWidget(waveRangePicker_);wavePage.toolbar->addSpacing(8);
    const std::array<NativeIcon,4> exploreIcons{NativeIcon::Plus,NativeIcon::Minus,NativeIcon::Left,NativeIcon::Right};
    const QStringList exploreIds{"waveZoomIn","waveZoomOut","wavePanLeft","wavePanRight"};const QStringList exploreNames{QStringLiteral("放大"),QStringLiteral("缩小"),QStringLiteral("左移"),QStringLiteral("右移")};
    for(int i=0;i<4;++i){waveNavigation_[i]=makeButton({},exploreIcons[i],44);waveNavigation_[i]->setObjectName(exploreIds[i]);waveNavigation_[i]->setToolTip(exploreNames[i]);waveNavigation_[i]->setAccessibleName(exploreNames[i]);wavePage.toolbar->addWidget(waveNavigation_[i]);}
    fullButton_=makeButton(QStringLiteral("全长"),NativeIcon::Fit,76);fullButton_->setObjectName("fullRange");wavePage.toolbar->addWidget(fullButton_);wavePage.toolbar->addStretch();
    auto* rangeDialog=new QDialog(this);rangeDialog->setObjectName("waveRangeDialog");rangeDialog->setWindowTitle(QStringLiteral("时间区间"));rangeDialog->resize(500,250);
    auto* rangeLayout=new QVBoxLayout(rangeDialog);rangeLayout->setContentsMargins(20,18,20,18);rangeLayout->setSpacing(14);rangeLayout->addWidget(makeLabel(QStringLiteral("时间区间"),22,"#1d2b3b",600));
    auto* rangeForm=new QFormLayout;rangeForm->setSpacing(12);rangeStart_=new TouchDoubleSpinBox;rangeEnd_=new TouchDoubleSpinBox;rangeStart_->setAccessibleName(QStringLiteral("开始时间"));rangeEnd_->setAccessibleName(QStringLiteral("结束时间"));rangeStart_->setObjectName("rangeStart");rangeEnd_->setObjectName("rangeEnd");
    for(auto* spin:{rangeStart_,rangeEnd_}){spin->setDecimals(5);spin->setSuffix(" s");spin->setRange(0,3600);spin->setSingleStep(.1);spin->setButtonSymbols(QAbstractSpinBox::NoButtons);spin->setMinimumHeight(44);}
    rangeForm->addRow(QStringLiteral("开始"),rangeStart_);rangeForm->addRow(QStringLiteral("结束"),rangeEnd_);rangeLayout->addLayout(rangeForm);
    auto* rangeError=makeLabel({},12,"#ae5149");rangeError->setObjectName("rangeValidationError");rangeLayout->addWidget(rangeError);
    auto* rangeActions=new QHBoxLayout;rangeActions->addStretch();auto* cancelRange=new QPushButton(QStringLiteral("取消"));rangeButton_=new QPushButton(QStringLiteral("应用"));rangeButton_->setObjectName("applyRange");rangeButton_->setProperty("primary",true);rangeActions->addWidget(cancelRange);rangeActions->addWidget(rangeButton_);rangeLayout->addLayout(rangeActions);
    connect(cancelRange,&QPushButton::clicked,rangeDialog,&QDialog::reject);connect(waveRangePicker_,&QPushButton::clicked,this,[this,rangeDialog,rangeError]{const double start=rangeStart_->value(),end=rangeEnd_->value();rangeError->clear();if(rangeDialog->exec()!=QDialog::Accepted){rangeStart_->setValue(start);rangeEnd_->setValue(end);}});
    connect(rangeButton_,&QPushButton::clicked,this,[this,rangeDialog,rangeError]{if(rangeEnd_->value()<=rangeStart_->value()){const auto message=QStringLiteral("结束时间须大于开始时间");rangeError->setText(message);waveCursor_->setText(message);return;}updateWaveform();rangeDialog->accept();});
    const auto channels=[&](QHBoxLayout* row,std::array<QCheckBox*,3>& boxes,const QString& prefix){for(int i=0;i<3;++i){boxes[i]=new TouchCheckBox(channelNames[i],colors[i]);boxes[i]->setObjectName(prefix+QString::number(i+1));boxes[i]->setChecked(true);row->addWidget(boxes[i]);connect(boxes[i],&QCheckBox::toggled,this,[this]{updateChannels();});}};
    auto* waveGrid=new QGridLayout;waveGrid->setContentsMargins(0,0,0,0);waveGrid->setSpacing(12);
    const auto waveCard=[&](PlotWidget* plot,const QString& title,int channel,bool overlay){auto* card=makeCard();auto* layout=new QVBoxLayout(card);layout->setContentsMargins(1,1,1,1);layout->setSpacing(0);auto* titleBar=new QWidget;titleBar->setFixedHeight(44);auto* titleRow=new QHBoxLayout(titleBar);titleRow->setContentsMargins(14,0,10,0);titleRow->setSpacing(6);if(channel>=0){auto* colorBar=new QFrame;colorBar->setFixedSize(4,14);colorBar->setStyleSheet(QString("background:%1;border:0;").arg(colors[channel].name()));titleRow->addWidget(colorBar);}titleRow->addWidget(makeLabel(title,14,"#1d2b3b",600));titleRow->addStretch();if(overlay)channels(titleRow,waveChannels_,"waveChannel");else titleRow->addWidget(makeLabel(QStringLiteral("g"),12,"#78899c"));layout->addWidget(titleBar);plot->setCompact(true);plot->setEmbedded(true);plot->setLegendVisible(false);plot->setMinimumHeight(90);layout->addWidget(plot,1);return card;};
    for(int axis=0;axis<3;++axis){auto* plot=new PlotWidget;waveSingle_[axis]=plot;plot->setObjectName(QString("waveCH%1Plot").arg(axis+1));plot->setXAxisVisible(axis==2);plot->setToolTip(QStringLiteral("%1 独立幅值刻度 · 拖动同步四图游标").arg(channelNames[axis]));plot->cursorSelected=[this](double t){selectWaveCursor(t);};waveGrid->addWidget(waveCard(plot,channelNames[axis],axis,false),axis/2,axis%2);}
    waveform_=new PlotWidget;waveform_->setObjectName("waveformPlot");waveform_->setToolTip(QStringLiteral("三通道共用幅值刻度；开关仅影响叠加图"));waveform_->cursorSelected=[this](double t){selectWaveCursor(t);};waveGrid->addWidget(waveCard(waveform_,QStringLiteral("叠加 / g"),-1,true),1,1);
    waveGrid->setRowStretch(0,1);waveGrid->setRowStretch(1,1);waveGrid->setColumnStretch(0,1);waveGrid->setColumnStretch(1,1);wavePage.layout->addLayout(waveGrid,1);
    waveCursor_=new QLabel;waveCursor_->setObjectName("waveCursor");waveCursor_->setFixedHeight(36);waveCursor_->setStyleSheet("background:white;border-radius:5px;padding:0 12px;color:#526479;font-size:13px;");wavePage.layout->addWidget(waveCursor_);
    connect(fullButton_,&QPushButton::clicked,this,[this]{if(!result_)return;rangeStart_->setValue(0);rangeEnd_->setValue(result_->duration_s);updateWaveform();});
    connect(waveNavigation_[0],&QPushButton::clicked,this,[this]{adjustWaveRange(.5,0);});connect(waveNavigation_[1],&QPushButton::clicked,this,[this]{adjustWaveRange(2,0);});connect(waveNavigation_[2],&QPushButton::clicked,this,[this]{adjustWaveRange(1,-.5);});connect(waveNavigation_[3],&QPushButton::clicked,this,[this]{adjustWaveRange(1,.5);});

    auto spectrumPage=page(QStringLiteral("频域"));channels(spectrumPage.toolbar,spectrumChannels_,"spectrumChannel");spectrumPage.toolbar->addStretch();spectrumPage.toolbar->addWidget(makeLabel(QStringLiteral("分段平均幅值谱"),12,"#78899c"));
    auto* frequencySummary=new QHBoxLayout;frequencySummary->setSpacing(14);
    frequencyReference_=makeLabel({},12);frequencyReference_->setObjectName("frequencyReference");frequencyReference_->setWordWrap(true);frequencySummary->addWidget(frequencyReference_,1);
    frequencyPeaks_=makeLabel({},12);frequencyPeaks_->setObjectName("frequencyPeaks");frequencyPeaks_->setWordWrap(true);frequencyPeaks_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);frequencySummary->addWidget(frequencyPeaks_,1);spectrumPage.layout->addLayout(frequencySummary);
    auto* spectrumCard=makeCard();auto* spectrumLayout=new QVBoxLayout(spectrumCard);spectrumLayout->setContentsMargins(0,0,0,0);spectrum_=new PlotWidget;spectrum_->setObjectName("spectrumPlot");spectrum_->setLegendVisible(false);spectrum_->setZeroBasedY(true);spectrumLayout->addWidget(spectrum_);spectrumPage.layout->addWidget(spectrumCard,1);
    footer_=makeLabel({},11,"#8a929d");footer_->setWordWrap(true);spectrumPage.layout->addWidget(footer_);
    frequencyCursor_=new QLabel;frequencyCursor_->setObjectName("frequencyCursor");frequencyCursor_->setFixedHeight(36);frequencyCursor_->setStyleSheet("background:white;border-radius:5px;padding:0 12px;color:#526479;font-size:13px;");spectrumPage.layout->addWidget(frequencyCursor_);
    spectrum_->cursorSelected=[this](double hz){if(!result_)return;const auto k=nearest_frequency_bin(result_->spectra[0],hz);const auto f=result_->spectra[0].frequency_hz[k];spectrum_->setCursor(f);frequencyCursor_->setText(QStringLiteral("%1 Hz　 CH1 %2　 CH2 %3　 CH3 %4 g").arg(format(f,3),format(result_->spectra[0].peak_amplitude[k],5),format(result_->spectra[1].peak_amplitude[k],5),format(result_->spectra[2].peak_amplitude[k],5)));};

    auto speedPage=page(QStringLiteral("转速"));speedHeading_=makeLabel({},13);speedPage.toolbar->addWidget(speedHeading_);speedPage.toolbar->addStretch();auto* pprButton=makeButton(QStringLiteral("PPR 参数"),NativeIcon::Settings,128);pprButton->setObjectName("speedPprParameters");speedPage.toolbar->addWidget(pprButton);connect(pprButton,&QPushButton::clicked,this,[this]{editParameters();});
    auto* speedSummary=new QHBoxLayout;speedSummary->setContentsMargins(12,8,12,8);speedSummary->setSpacing(24);
    metrics_=new QLabel;metrics_->setObjectName("analysisMetrics");metrics_->setWordWrap(true);speedSummary->addWidget(metrics_,1);
    speedStats_=makeLabel({},13);speedStats_->setObjectName("speedStats");speedStats_->setWordWrap(true);speedSummary->addWidget(speedStats_,2);speedPage.layout->addLayout(speedSummary);
    speed_=new PlotWidget;speed_->setObjectName("speedPlot");speedPage.layout->addWidget(speed_,1);
    auto* speedNote=makeLabel(QStringLiteral("缺测保留为空，不解释为停转。"),11,"#929aa6");speedPage.layout->addWidget(speedNote);


    auto detectionPage=page(QStringLiteral("检测"));
    auto* detectionGroup=new QButtonGroup(this);detectionGroup->setExclusive(true);
    const QStringList detectionLabels{QStringLiteral("总览"),QStringLiteral("特征"),QStringLiteral("条件记录")};const QStringList detectionIds{"detectionOverviewTab","detectionFeaturesTab","detectionConditionsTab"};std::array<QPushButton*,3> detectionNav{};
    for(int i=0;i<3;++i){auto* button=new QPushButton(detectionLabels[i]);button->setObjectName(detectionIds[i]);button->setProperty("segment",true);button->setCheckable(true);button->setFixedWidth(100);detectionGroup->addButton(button);detectionPage.toolbar->addWidget(button);detectionNav[i]=button;connect(button,&QPushButton::clicked,this,[this,i]{detectionTabs_->setCurrentIndex(i);});}detectionNav[0]->setChecked(true);detectionPage.toolbar->addStretch();
    detectionConfigButton_=makeButton(QStringLiteral("配置"),NativeIcon::Settings,116);detectionConfigButton_->setObjectName("manageDetectionConfig");detectionPage.toolbar->addWidget(detectionConfigButton_);
    detectionTabs_=new QTabWidget;detectionTabs_->setObjectName("detectionTabs");detectionTabs_->tabBar()->hide();detectionPage.layout->addWidget(detectionTabs_,1);
    auto* overview=new QWidget;auto* overviewLayout=new QVBoxLayout(overview);overviewLayout->setContentsMargins(0,0,0,0);overviewLayout->setSpacing(12);
    auto* hero=new QHBoxLayout;hero->setSpacing(12);auto* stateCard=makeCard();stateCard->setMinimumWidth(320);stateCard->setMaximumWidth(410);auto* stateLayout=new QVBoxLayout(stateCard);stateLayout->setContentsMargins(20,18,20,18);stateLayout->setSpacing(12);auto* stateHeading=new QHBoxLayout;auto* stateIcon=new QLabel;stateIcon->setPixmap(nativeIcon(NativeIcon::Detection,QColor("#956320"),24).pixmap(24,24));stateHeading->addWidget(stateIcon);stateHeading->addWidget(makeLabel(QStringLiteral("判定状态"),13));stateHeading->addStretch();stateLayout->addLayout(stateHeading);detectionState_=makeLabel({},26,"#1d2b3b",600);detectionState_->setObjectName("detectionState");detectionState_->setTextFormat(Qt::PlainText);detectionState_->setWordWrap(true);stateLayout->addWidget(detectionState_);detectionQuality_=makeLabel({},13);detectionQuality_->setObjectName("detectionQuality");detectionQuality_->setWordWrap(true);detectionQuality_->setTextFormat(Qt::PlainText);stateLayout->addWidget(detectionQuality_);stateLayout->addStretch();auto* experimental=makeLabel(QStringLiteral("实验规则框架 · 尚无独立验证结果"),12,"#956320");experimental->setWordWrap(true);stateLayout->addWidget(experimental);hero->addWidget(stateCard,2);
    auto* evidenceCard=makeCard();auto* evidenceLayout=new QVBoxLayout(evidenceCard);evidenceLayout->setContentsMargins(18,16,12,12);evidenceLayout->setSpacing(10);detectionSummaryHeading_=makeLabel(QStringLiteral("需要处理"),16,"#1d2b3b",600);detectionSummaryHeading_->setObjectName("detectionSummaryHeading");evidenceLayout->addWidget(detectionSummaryHeading_);auto* summaryScroll=new QScrollArea;summaryScroll->setWidgetResizable(true);summaryScroll->setFrameShape(QFrame::NoFrame);auto* summaryBody=new QWidget;summaryBody->setObjectName("detectionSummaryCards");summaryBody->setStyleSheet("background:#ffffff;");detectionCards_=new QVBoxLayout(summaryBody);detectionCards_->setContentsMargins(0,0,4,0);detectionCards_->setSpacing(10);summaryScroll->setWidget(summaryBody);QScroller::grabGesture(summaryScroll->viewport(),QScroller::TouchGesture);evidenceLayout->addWidget(summaryScroll,1);hero->addWidget(evidenceCard,3);overviewLayout->addLayout(hero,1);
    auto* profileCard=makeCard();auto* profileLayout=new QVBoxLayout(profileCard);profileLayout->setContentsMargins(20,14,20,14);profileLayout->setSpacing(8);profileLayout->addWidget(makeLabel(QStringLiteral("检测配置"),13));detectionConfigSummary_=new ElidedProfileLabel;detectionConfigSummary_->setStyleSheet("font-size:17px;color:#1d2b3b;font-weight:600;");detectionConfigSummary_->setObjectName("detectionConfigSummary");profileLayout->addWidget(detectionConfigSummary_);detectionConfigMeta_=makeLabel({},12,"#78899c");detectionConfigMeta_->setObjectName("detectionConfigMeta");detectionConfigMeta_->setWordWrap(true);profileLayout->addWidget(detectionConfigMeta_);overviewLayout->addWidget(profileCard);
    auto* overviewActions=new QHBoxLayout;detailsButton_=makeButton(QStringLiteral("分析详情"),NativeIcon::Info,132);detailsButton_->setObjectName("analysisDetails");overviewActions->addWidget(detailsButton_);overviewActions->addStretch();overviewLayout->addLayout(overviewActions);detectionTabs_->addTab(overview,QStringLiteral("总览"));
    auto* featureScroll=new QScrollArea;featureScroll->setWidgetResizable(true);featureScroll->setFrameShape(QFrame::NoFrame);auto* featureBody=new QWidget;featureBody->setStyleSheet("background:#f4f6f9;");auto* featureLayout=new QVBoxLayout(featureBody);featureLayout->setContentsMargins(0,0,0,0);featureLayout->setSpacing(12);auto* featureCard=makeCard();auto* featureCardLayout=new QVBoxLayout(featureCard);featureCardLayout->setContentsMargins(20,20,20,20);featureCardLayout->setSpacing(16);diagnosis_=makeLabel({},20,"#1d2b3b",600);diagnosis_->setObjectName("analysisDiagnosis");featureCardLayout->addWidget(diagnosis_);auto* featureNote=makeLabel(QStringLiteral("数值特征用于比较；尚无经验证的故障分类器。"),12,"#78899c");featureNote->setWordWrap(true);featureCardLayout->addWidget(featureNote);features_=new QLabel;features_->setObjectName("analysisFeatures");features_->setTextFormat(Qt::RichText);features_->setMinimumHeight(128);featureCardLayout->addWidget(features_);bandTable_=new QLabel;bandTable_->setObjectName("orderBandTable");bandTable_->setTextFormat(Qt::RichText);bandTable_->setMinimumHeight(115);featureCardLayout->addWidget(bandTable_);featureLayout->addWidget(featureCard);featureLayout->addStretch();featureScroll->setWidget(featureBody);detectionTabs_->addTab(featureScroll,QStringLiteral("特征"));
    auto* conditionsCard=makeCard();auto* conditionsLayout=new QVBoxLayout(conditionsCard);conditionsLayout->setContentsMargins(8,8,8,8);detectionEvidence_=new QTextBrowser;detectionEvidence_->setObjectName("detectionEvidence");detectionEvidence_->setOpenExternalLinks(false);conditionsLayout->addWidget(detectionEvidence_);detectionTabs_->addTab(conditionsCard,QStringLiteral("条件记录"));connect(detectionTabs_,&QTabWidget::currentChanged,this,[detectionNav](int index){for(int i=0;i<3;++i)detectionNav[i]->setChecked(index==i);});detectionTabs_->tabBar()->hide();
    evidence_=new QTextBrowser(this);evidence_->setObjectName("analysisEvidence");evidence_->hide();

    auto* configDialog=new QDialog(this);configDialog->setObjectName("detectionConfigDialog");configDialog->setWindowTitle(QStringLiteral("检测配置"));configDialog->resize(620,360);auto* configLayout=new QVBoxLayout(configDialog);configLayout->setContentsMargins(20,20,20,20);configLayout->setSpacing(16);configLayout->addWidget(makeLabel(QStringLiteral("检测配置"),22,"#1d2b3b",600));detectionProfileLabel_=makeLabel({},13);detectionProfileLabel_->setObjectName("detectionProfile");detectionProfileLabel_->setWordWrap(true);detectionProfileLabel_->setTextFormat(Qt::PlainText);configLayout->addWidget(detectionProfileLabel_);auto* profileNote=makeLabel(QStringLiteral("单记录基线草稿仅保存观察值，不代表正常工况，也不包含识别规则。"),13,"#78899c");profileNote->setWordWrap(true);configLayout->addWidget(profileNote);configLayout->addStretch();auto* configActions=new QHBoxLayout;loadProfileButton_=makeButton(QStringLiteral("加载配置"),NativeIcon::Folder);loadProfileButton_->setObjectName("loadDetectionProfile");clearProfileButton_=new QPushButton(QStringLiteral("清除配置"));clearProfileButton_->setObjectName("clearDetectionProfile");baselineButton_=new QPushButton(QStringLiteral("生成基线草稿"));baselineButton_->setObjectName("saveBaselineDraft");configActions->addWidget(loadProfileButton_);configActions->addWidget(clearProfileButton_);configActions->addWidget(baselineButton_);configLayout->addLayout(configActions);auto* configClose=new QDialogButtonBox(QDialogButtonBox::Close);configClose->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));configLayout->addWidget(configClose);connect(configClose,&QDialogButtonBox::rejected,configDialog,&QDialog::reject);connect(detectionConfigButton_,&QPushButton::clicked,this,[configDialog]{configDialog->exec();});
    auto* exportDialog=new QDialog(this);exportDialog->setObjectName("exportOptionsDialog");exportDialog->setWindowTitle(QStringLiteral("导出分析结果"));exportDialog->resize(450,220);auto* exportLayout=new QVBoxLayout(exportDialog);exportLayout->setContentsMargins(20,20,20,20);exportLayout->setSpacing(16);exportLayout->addWidget(makeLabel(QStringLiteral("导出分析结果"),22,"#1d2b3b",600));auto* exportNote=makeLabel(QStringLiteral("包含来源、参数、数值特征与实验检测证据。"),13,"#78899c");exportNote->setWordWrap(true);exportLayout->addWidget(exportNote);auto* exportFormats=new QHBoxLayout;csvButton_=new QPushButton(QStringLiteral("CSV 表格"));csvButton_->setObjectName("exportCsv");exportButton_=new QPushButton(QStringLiteral("JSON 完整结果"));exportButton_->setObjectName("exportResult");exportFormats->addWidget(csvButton_);exportFormats->addWidget(exportButton_);exportLayout->addLayout(exportFormats);auto* exportCancel=new QDialogButtonBox(QDialogButtonBox::Cancel);exportCancel->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));exportLayout->addWidget(exportCancel);connect(exportCancel,&QDialogButtonBox::rejected,exportDialog,&QDialog::reject);connect(exportButton_,&QPushButton::clicked,exportDialog,[exportDialog]{exportDialog->done(1);});connect(csvButton_,&QPushButton::clicked,exportDialog,[exportDialog]{exportDialog->done(2);});connect(exportMenuButton_,&QPushButton::clicked,this,[this,exportDialog]{const int format=exportDialog->exec();if(format==1||format==2)exportResult(format==2);});

    navigation_[0]->setChecked(true);connect(pages_,&QStackedWidget::currentChanged,this,[this](int index){for(int i=0;i<5;++i)navigation_[i]->setChecked(i==index);});
    connect(openButton_,&QPushButton::clicked,this,[this]{chooseFile();});connect(directoryButton_,&QPushButton::clicked,this,[this]{chooseDirectory();});connect(refreshButton_,&QPushButton::clicked,this,[this]{refreshFiles();});connect(demoButton_,&QPushButton::clicked,this,[this]{openDemo();});connect(parametersButton_,&QPushButton::clicked,this,[this]{editParameters();});connect(detailsButton_,&QPushButton::clicked,this,[this]{showDetails();});connect(loadProfileButton_,&QPushButton::clicked,this,[this]{chooseDetectionProfile();});connect(clearProfileButton_,&QPushButton::clicked,this,[this]{clearDetectionProfile();});connect(baselineButton_,&QPushButton::clicked,this,[this]{createBaselineDraft();});
    clearResults();setBanner(QStringLiteral("未打开文件"),"idle");refreshFiles();
}

bool MainWindow::setPage(const QString& name) {
    if(name=="capture"){pages_->setCurrentIndex(0);findChild<QPushButton*>("deviceCaptureMode")->click();return true;}
    if(name=="features"){pages_->setCurrentIndex(4);navigation_[4]->setChecked(true);detectionTabs_->setCurrentIndex(1);return true;}
    const QStringList ids{"files","waveform","spectrum","speed","diagnosis"};
    int i=ids.indexOf(name);if(i<0){bool valid=false;i=name.toInt(&valid);if(!valid||i<0||i>4)return false;}
    pages_->setCurrentIndex(i);navigation_[i]->setChecked(true);if(i==4)detectionTabs_->setCurrentIndex(0);return true;
}

bool MainWindow::loadDetectionProfile(const QString& path, QString* error) {
    auto fail=[&](const QString& message){if(error)*error=message;return false;};
    if(isAnalyzing())return fail(QStringLiteral("正在分析，请等待完成后加载检测配置。"));
    if(path.isEmpty())return fail(QStringLiteral("未选择检测配置。"));
    detectionProfilePath_=QFileInfo(path).absoluteFilePath();
    if(!detectionInputPaths_.contains(detectionProfilePath_))detectionInputPaths_.append(detectionProfilePath_);
    // A failed replacement never leaves an earlier profile or candidate active.
    detectionProfile_.reset();detectionProfileError_.clear();
    try {
        detectionProfile_=load_detection_profile(detectionProfilePath_.toUtf8().toStdString());
    }catch(const std::exception& e){
        detectionProfileError_=QString::fromUtf8(e.what());updateDetection();return fail(detectionProfileError_);
    }
    updateDetection();return true;
}

void MainWindow::clearDetectionProfile() {
    if(isAnalyzing())return;
    detectionProfile_.reset();detectionProfilePath_.clear();detectionProfileError_.clear();updateDetection();
}

bool MainWindow::setDetectionSetupId(const QString& setupId, QString* error) {
    auto fail=[&](const QString& message){if(error)*error=message;return false;};
    if(isAnalyzing())return fail(QStringLiteral("正在分析，请等待完成后修改台架标识。"));
    if(setupId.size()>128)return fail(QStringLiteral("台架标识最长为 128 个 ASCII 字符。"));
    for(const QChar c:setupId)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='.'||c=='-'))
        return fail(QStringLiteral("台架标识只能使用字母、数字、下划线、点或连字符。"));
    detectionSetupId_=setupId;updateDetection();return true;
}

bool MainWindow::setDetectionMeasurementConditions(SignalKind source, bool calibrationConfirmed, QString* error) {
    auto fail=[&](const QString& message){if(error)*error=message;return false;};
    if(isAnalyzing())return fail(QStringLiteral("正在分析，请等待完成后确认当前记录条件。"));
    if(!result_)return fail(QStringLiteral("请先打开有效的采集记录。"));
    if(source!=SignalKind::Unknown&&source!=SignalKind::TestSignal&&source!=SignalKind::VibrationConfirmed)
        return fail(QStringLiteral("记录来源无效。"));
    if(source==SignalKind::VibrationConfirmed&&(synthetic_||!metadataNotice_.isEmpty()))
        return fail(QStringLiteral("合成数据或无效来源说明不能确认为机械振动记录。"));
    config_.signal_kind=synthetic_?SignalKind::TestSignal:source;
    config_.calibration_confirmed=calibrationConfirmed;
    beginAnalysis();return true;
}

std::vector<std::string> MainWindow::protectedOutputPaths() const {
    std::vector<std::string> paths;
    if(!currentPath_.isEmpty()){
        const QFileInfo source(currentPath_);
        paths.push_back(currentPath_.toUtf8().toStdString());
        paths.push_back((source.absolutePath()+'/'+source.completeBaseName()+".meta.json").toUtf8().toStdString());
    }
    for(const auto& path:detectionInputPaths_)paths.push_back(path.toUtf8().toStdString());
    return paths;
}

bool MainWindow::saveDetectionBaselineDraft(const QString& path, const QString& profileId, QString* error) const {
    auto fail=[&](const QString& message){if(error)*error=message;return false;};
    if(isAnalyzing())return fail(QStringLiteral("正在分析，请等待完成后生成基线草稿。"));
    if(!result_)return fail(QStringLiteral("请先打开有效的采集记录。"));
    if(result_->source_metadata.synthetic||result_->source_metadata.status=="invalid_or_unreadable_sidecar")
        return fail(QStringLiteral("合成数据或无效来源说明不能生成机械基线草稿。"));
    if(result_->config.signal_kind!=SignalKind::VibrationConfirmed||!result_->config.calibration_confirmed||detectionSetupId_.isEmpty())
        return fail(QStringLiteral("先在参数中明确确认当前机械记录来源、实际标定和台架标识，再生成草稿。"));
    try {
        const auto draft=make_baseline_draft(*result_,detectionSetupId_.toUtf8().toStdString(),profileId.toUtf8().toStdString());
        validate_detection_outputs({path.toUtf8().toStdString()},protectedOutputPaths());
        const auto data=QByteArray::fromStdString(detection_profile_text(draft));
        QSaveFile output(path);
        if(!output.open(QIODevice::WriteOnly)||output.write(data)!=data.size()||!output.commit())
            return fail(output.errorString());
    }catch(const std::exception& e){return fail(QString::fromUtf8(e.what()));}
    return true;
}

void MainWindow::chooseDetectionProfile() {
    const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("加载实验检测配置"),detectionProfilePath_,QStringLiteral("检测配置 (*.profile *.txt);;所有文件 (*)"));
    if(path.isEmpty())return;
    QString error;if(!loadDetectionProfile(path,&error))QMessageBox::warning(this,QStringLiteral("配置无效，检测已阻断"),error);
}

void MainWindow::createBaselineDraft() {
    if(!result_)return;
    if(result_->source_metadata.synthetic||result_->config.signal_kind!=SignalKind::VibrationConfirmed||!result_->config.calibration_confirmed||detectionSetupId_.isEmpty()||!metadataNotice_.isEmpty()){
        QMessageBox::information(this,QStringLiteral("基线条件未确认"),QStringLiteral("请先在“参数”中明确确认当前机械记录来源、实际标定与台架标识。\n\n草稿只保存本条记录的观察值，不代表正常工况，也不包含识别规则。合成数据不能作为机械基线。"));return;
    }
    const QFileInfo source(currentPath_);
    const auto path=QFileDialog::getSaveFileName(this,QStringLiteral("保存单记录基线草稿（不代表正常）"),source.absolutePath()+'/'+source.completeBaseName()+"_baseline_draft.profile",QStringLiteral("检测配置 (*.profile)"));
    if(path.isEmpty())return;
    QString error;if(!saveDetectionBaselineDraft(path,QStringLiteral("single_record_draft"),&error))QMessageBox::warning(this,QStringLiteral("草稿保存失败"),error);
    else QMessageBox::information(this,QStringLiteral("草稿已保存"),QStringLiteral("已保存单记录基线草稿，状态为 draft，不含规则。需独立工况数据与验证后再配置实验规则；草稿不会自动加载。"));
}

void MainWindow::updateDetection() {
    detectionResult_=DetectionResult{};
    detectionResult_.setup_id=detectionSetupId_.toUtf8().toStdString();
    detectionResult_.source_path=currentPath_.toUtf8().toStdString();
    detectionResult_.limitations={"Experimental rule framework only; no validated classifier, diagnostic probability or accuracy claim."};
    QString quality;
    if(!detectionProfileError_.isEmpty()){
        detectionResult_.status="invalid_profile";detectionState_->setText(QStringLiteral("配置无效 · 不可判定"));
        quality=QStringLiteral("配置未通过严格校验，旧配置和候选已清除。\n%1").arg(detectionProfileError_);
    }else if(!detectionProfile_){
        detectionResult_.status="not_configured";detectionState_->setText(QStringLiteral("待配置"));
        quality=QStringLiteral("未加载台架基线与实验规则。请确认来源、标定与台架条件后加载配置。当前不输出故障类别。");
    }else if(!result_){
        detectionResult_.status="no_record";detectionState_->setText(QStringLiteral("等待有效记录"));
        quality=QStringLiteral("尚无可用于检测的完整分析结果。打开记录后检查数据质量与台架适用条件。");
    }else{
        try {detectionResult_=evaluate_detection(*result_,*detectionProfile_,detectionSetupId_.toUtf8().toStdString());}
        catch(const std::exception& e){detectionResult_.status="evaluation_error";quality=QStringLiteral("检测评估失败，未保留旧候选：%1").arg(QString::fromUtf8(e.what()));}
        if(detectionResult_.status=="candidate"&&detectionResult_.candidate)detectionState_->setText(candidateName(*detectionResult_.candidate)+QStringLiteral(" · 未验证"));
        else if(detectionResult_.status=="conflict")detectionState_->setText(QStringLiteral("规则冲突 · 不可判定"));
        else if(detectionResult_.status=="inconclusive")detectionState_->setText(QStringLiteral("无规则命中 · 不可判定"));
        else if(detectionResult_.status=="baseline_only")detectionState_->setText(QStringLiteral("仅有基线 · 未配置规则"));
        else if(detectionResult_.status=="evaluation_error")detectionState_->setText(QStringLiteral("评估失败 · 不可判定"));
        else detectionState_->setText(QStringLiteral("条件未满足 · 不可判定"));
        if(quality.isEmpty()){
            QStringList failed;for(const auto& gate:detectionResult_.gates)if(!gate.passed)failed<<gateName(gate.id);
            quality=!failed.empty()?QStringLiteral("数据质量 / 适用条件未通过：%1。候选已阻断。").arg(failed.join(QStringLiteral("、"))):
                detectionResult_.status=="candidate"?QStringLiteral("数据质量 / 适用条件通过；仅输出实验规则候选，尚无独立验证结果。"):
                detectionResult_.status=="conflict"?QStringLiteral("数据质量 / 适用条件通过；多个类别的规则同时命中，未选择候选。"):
                QStringLiteral("数据质量 / 适用条件通过；当前规则未提供唯一候选，不能据此判断正常。");
        }
    }
    if(detectionProfile_){
        detectionResult_.profile_id=detectionProfile_->id;detectionResult_.profile_status=detectionProfile_->status;
        detectionProfileLabel_->setText(QStringLiteral("配置 %1 [%2]　 ·　 当前台架 %3　 ·　 配置台架 %4")
            .arg(text(detectionProfile_->id),text(detectionProfile_->status),detectionSetupId_.isEmpty()?QStringLiteral("待确认"):detectionSetupId_,text(detectionProfile_->setup_id)));
    }else detectionProfileLabel_->setText(QStringLiteral("配置 未加载　 ·　 当前台架 %1（在参数中设置）").arg(detectionSetupId_.isEmpty()?QStringLiteral("待确认"):detectionSetupId_));
    detectionProfileLabel_->setToolTip(detectionProfilePath_);detectionQuality_->setText(quality);
    QStringList html;
    for(const auto& rule:detectionResult_.rules)if(rule.matched){
        html<<QStringLiteral("<b>命中规则 %1 · %2（未验证）</b>").arg(text(rule.id).toHtmlEscaped(),candidateName(rule.label).toHtmlEscaped());
        for(const auto& condition:rule.conditions)html<<QStringLiteral("<p>CH%1 %2：当前 %3；基线 %4；规则计算值 %5，满足区间 [%6, %7]。</p>")
            .arg(condition.condition.channel).arg(featureName(condition.condition.feature).toHtmlEscaped(),optionalNumber(condition.current,5),optionalNumber(condition.baseline,5),optionalNumber(condition.evaluated,5),format(condition.condition.minimum,5),format(condition.condition.maximum,5));
    }
    if(detectionResult_.gates.empty())html<<QStringLiteral("<p>检测顺序：数据质量 → 来源与标定 → 台架与工况 → 基线对比 → 实验规则。</p>");
    else{
        html<<QStringLiteral("<b>条件记录</b>");
        for(const auto& gate:detectionResult_.gates)html<<QStringLiteral("<p style='margin:5px 0'><span style='color:%1'>%2</span> %3：%4</p>")
            .arg(gate.passed?"#668672":"#96534c",gate.passed?QStringLiteral("通过"):QStringLiteral("未通过"),gateName(gate.id).toHtmlEscaped(),gateDetail(gate).toHtmlEscaped());
    }
    if(!detectionResult_.rules.empty()){
        html<<QStringLiteral("<br><b>规则证据</b>");
        for(const auto& rule:detectionResult_.rules){
            html<<QStringLiteral("<p><b>%1</b> · %2 · %3</p>").arg(text(rule.id).toHtmlEscaped(),candidateName(rule.label).toHtmlEscaped(),rule.matched?QStringLiteral("命中"):QStringLiteral("未命中"));
            for(const auto& condition:rule.conditions)html<<QStringLiteral("<p style='margin:3px 0 3px 12px'>CH%1 %2 · %3 · 计算值 %4 · 区间 [%5, %6] · %7</p>")
                .arg(condition.condition.channel).arg(featureName(condition.condition.feature).toHtmlEscaped(),condition.condition.comparison=="absolute"?QStringLiteral("绝对值"):condition.condition.comparison=="delta"?QStringLiteral("与基线差值"):QStringLiteral("与基线比值"),optionalNumber(condition.evaluated,5),format(condition.condition.minimum,5),format(condition.condition.maximum,5),conditionState(condition.status).toHtmlEscaped());
        }
    }
    if(!detectionResult_.baseline_comparisons.empty()){
        html<<QStringLiteral("<br><b>基线对比</b>");
        for(const auto& comparison:detectionResult_.baseline_comparisons)html<<QStringLiteral("<p style='margin:4px 0'>CH%1 %2 · 当前 %3 · 基线 %4 · 差值 %5 · 比值 %6</p>")
            .arg(comparison.channel).arg(featureName(comparison.feature).toHtmlEscaped(),optionalNumber(comparison.current,5),format(comparison.baseline,5),optionalNumber(comparison.delta,5),optionalNumber(comparison.ratio,3));
    }
    html<<QStringLiteral("<p style='color:#7f8f99'>实验规则不等于已验证的故障识别。这里不显示诊断概率或准确率；单条记录草稿不代表正常工况。</p>");
    detectionEvidence_->setHtml(QStringLiteral("<html><body style='font-size:13px;color:#43545f'>%1</body></html>").arg(html.join(QString())));
    clearProfileButton_->setEnabled(!isAnalyzing()&&(bool(detectionProfile_)||!detectionProfileError_.isEmpty()));
    baselineButton_->setEnabled(!isAnalyzing()&&bool(result_));
    updateDetectionSummary();
}

void MainWindow::updateDetectionSummary() {
    while(auto* item=detectionCards_->takeAt(0)){delete item->widget();delete item;}
    const QString fullQuality=detectionQuality_->text();detectionQuality_->setToolTip(fullQuality);
    const auto addCard=[this](const QString& title,const QString& detail,NativeIcon icon,const QString& gateId,const QString& action,std::function<void()> callback) {
        auto* card=new QWidget;card->setObjectName(gateId.isEmpty()?QStringLiteral("detectionSummaryItem"):QStringLiteral("gate_")+gateId);
        if(!gateId.isEmpty()){card->setProperty("gateId",gateId);card->setProperty("passed",false);}
        auto* row=new QHBoxLayout(card);row->setContentsMargins(0,8,0,12);row->setSpacing(12);
        auto* symbol=new QLabel;symbol->setFixedSize(24,28);symbol->setPixmap(nativeIcon(icon,QColor(icon==NativeIcon::Check?"#238575":"#956320"),24).pixmap(24,24));row->addWidget(symbol,0,Qt::AlignTop);
        auto* words=new QVBoxLayout;words->setSpacing(6);auto* heading=new QLabel(title);heading->setTextFormat(Qt::PlainText);heading->setWordWrap(true);heading->setStyleSheet("font-size:15px;font-weight:600;color:#1d2b3b;");words->addWidget(heading);
        auto* description=new QLabel(detail);description->setTextFormat(Qt::PlainText);description->setWordWrap(true);description->setStyleSheet("font-size:13px;color:#526479;");words->addWidget(description);row->addLayout(words,1);
        if(!action.isEmpty()){auto* button=new QPushButton(action);button->setFixedSize(76,44);row->addWidget(button,0,Qt::AlignTop);connect(button,&QPushButton::clicked,this,[callback]{callback();});}
        detectionCards_->addWidget(card);
    };
    const QString status=text(detectionResult_.status);
    int failed=0,passed=0,matched=0;for(const auto& gate:detectionResult_.gates)(gate.passed?passed:failed)++;for(const auto& rule:detectionResult_.rules)if(rule.matched)++matched;
    if(failed){
        detectionSummaryHeading_->setText(QStringLiteral("需要处理 · %1 项").arg(failed));
        detectionQuality_->setText(QStringLiteral("先处理未通过的条件。当前不输出故障类别。"));
        for(const auto& gate:detectionResult_.gates)if(!gate.passed){
            const auto id=text(gate.id);QString detail=gateDetail(gate);
            if(gate.id=="source"&&synthetic_)detail=QStringLiteral("当前记录为合成演示，不能用于机械故障判定。");
            else if(gate.id=="source"&&!metadataNotice_.isEmpty())detail=metadataNotice_;
            else if(gate.id=="calibration"&&result_&&!result_->config.calibration_confirmed)detail=QStringLiteral("当前记录尚未确认传感器与前端的实际标定。");
            const bool editable=gate.id=="setup"||gate.id=="calibration"||gate.id=="ppr"||gate.id=="analysis_configuration"||(gate.id=="source"&&!synthetic_&&metadataNotice_.isEmpty());
            addCard(gateName(gate.id),detail,gate.id=="source"?NativeIcon::Source:gate.id=="calibration"?NativeIcon::Calibration:NativeIcon::Alert,id,editable?QStringLiteral("参数"):QStringLiteral("详情"),[this,editable]{if(editable)editParameters();else showDetails();});
        }
    }else if(matched){
        detectionSummaryHeading_->setText(QStringLiteral("命中规则 · %1 项").arg(matched));
        detectionQuality_->setText(status=="conflict"?QStringLiteral("多个类别同时命中，未选择候选。"):QStringLiteral("实验规则命中，候选尚无独立验证结果。"));
        for(const auto& rule:detectionResult_.rules)if(rule.matched){
            QStringList conditions;for(const auto& condition:rule.conditions)conditions<<QStringLiteral("CH%1 %2：当前 %3；基线 %4；计算值 %5；区间 [%6, %7]")
                .arg(condition.condition.channel).arg(featureName(condition.condition.feature),optionalNumber(condition.current,5),optionalNumber(condition.baseline,5),optionalNumber(condition.evaluated,5),format(condition.condition.minimum,5),format(condition.condition.maximum,5));
            addCard(text(rule.id)+QStringLiteral(" · ")+candidateName(rule.label),conditions.join('\n'),status=="conflict"?NativeIcon::Alert:NativeIcon::Check,{},QStringLiteral("证据"),[this]{detectionTabs_->setCurrentIndex(2);});
        }
    }else if(status=="not_configured"){
        detectionSummaryHeading_->setText(QStringLiteral("开始检测"));detectionQuality_->setText(QStringLiteral("加载台架基线与实验规则后开始评估。"));
        addCard(QStringLiteral("尚未加载检测配置"),QStringLiteral("先确认当前记录来源、实际标定与台架标识，再加载适用的配置。"),NativeIcon::Folder,{},QStringLiteral("配置"),[this]{detectionConfigButton_->click();});
    }else if(status=="no_record"){
        detectionSummaryHeading_->setText(QStringLiteral("等待记录"));detectionQuality_->setText(QStringLiteral("打开有效采集记录后检查质量与适用条件。"));
        addCard(QStringLiteral("没有完整分析结果"),QStringLiteral("离线分析不会把设备连接状态作为故障结论。"),NativeIcon::Record,{},QStringLiteral("打开"),[this]{chooseFile();});
    }else if(status=="invalid_profile"||status=="evaluation_error"){
        detectionSummaryHeading_->setText(QStringLiteral("错误详情"));detectionQuality_->setText(QStringLiteral("评估已停止，旧配置或旧候选已清除。"));
        addCard(status=="invalid_profile"?QStringLiteral("配置未通过校验"):QStringLiteral("检测评估失败"),status=="invalid_profile"?detectionProfileError_:fullQuality,NativeIcon::Alert,{},QStringLiteral("配置"),[this]{detectionConfigButton_->click();});
    }else if(status=="baseline_only"){
        detectionSummaryHeading_->setText(QStringLiteral("观察基线"));detectionQuality_->setText(QStringLiteral("此配置仅含基线观察值，尚未提供实验规则。"));
        addCard(QStringLiteral("未配置检测规则"),QStringLiteral("单记录草稿不代表正常工况；当前不输出正常或故障候选。"),NativeIcon::Info,{},QStringLiteral("证据"),[this]{detectionTabs_->setCurrentIndex(2);});
    }else{
        detectionSummaryHeading_->setText(QStringLiteral("规则评估"));detectionQuality_->setText(QStringLiteral("当前规则未提供唯一候选，不能据此判断正常。"));
        addCard(QStringLiteral("没有唯一候选"),QStringLiteral("完整条件记录、规则计算值与基线比较保留在证据页。"),NativeIcon::Info,{},QStringLiteral("证据"),[this]{detectionTabs_->setCurrentIndex(2);});
    }
    detectionCards_->addStretch();
    detectionConfigSummary_->setTextFormat(Qt::PlainText);detectionConfigMeta_->setTextFormat(Qt::PlainText);
    const QString profileSummary=detectionProfile_?text(detectionProfile_->id)+QStringLiteral("  [")+text(detectionProfile_->status)+']':QStringLiteral("未加载");
    static_cast<ElidedProfileLabel*>(detectionConfigSummary_)->setFullText(profileSummary,detectionProfilePath_.isEmpty()?profileSummary:profileSummary+'\n'+detectionProfilePath_);
    const QString gateSummary=detectionResult_.gates.empty()?QStringLiteral("尚未评估质量与适用条件"):QStringLiteral("质量与适用条件：%1 项通过 · %2 项未通过").arg(passed).arg(failed);
    detectionConfigMeta_->setText(QStringLiteral("当前台架 %1  ·  %2").arg(detectionSetupId_.isEmpty()?QStringLiteral("待确认"):detectionSetupId_,gateSummary));
}
void MainWindow::chooseDirectory() {
    const auto path=QFileDialog::getExistingDirectory(this,QStringLiteral("选择数据目录"),directory_);
    if(!path.isEmpty())setDataDirectory(path);
}
void MainWindow::setDataDirectory(const QString& path) {
    directory_=path.isEmpty()?QString():QDir(path).absolutePath();
    refreshFiles();
}
void MainWindow::refreshFiles() {
    files_->clear();
    sidebarFiles_->clear();
    if(directory_.isEmpty()){auto* hint=new QListWidgetItem(QStringLiteral("暂无记录"),sidebarFiles_);hint->setFlags(Qt::NoItemFlags);}
    if(directory_.isEmpty()) {directoryLabel_->setText(QStringLiteral("选择 SD 卡中的目录，或打开已保存的 BIN 文件"));directoryLabel_->setToolTip(QString());auto* item=new QListWidgetItem(QStringLiteral("尚未选择数据目录"),files_);item->setFlags(Qt::NoItemFlags);return;}
    QDir dir(directory_);directoryLabel_->setText(fontMetrics().elidedText(QDir::toNativeSeparators(directory_),Qt::ElideMiddle,650));directoryLabel_->setToolTip(directory_);
    const QFileInfo directoryInfo(directory_);
    QString directoryError;
    if(!directoryInfo.exists())directoryError=QStringLiteral("数据目录不存在，请检查 SD 卡或重新选择目录");
    else if(!directoryInfo.isDir())directoryError=QStringLiteral("所选路径不是目录，请重新选择目录");
    else if(!directoryInfo.isReadable()||!dir.isReadable())directoryError=QStringLiteral("数据目录不可读，请检查权限或重新选择目录");
    if(!directoryError.isEmpty()){auto* item=new QListWidgetItem(directoryError,files_);item->setFlags(Qt::NoItemFlags);return;}
    const auto entries=dir.entryInfoList(QDir::Files|QDir::Readable,QDir::Name|QDir::IgnoreCase);
    int count=0;
    for(const auto& entry:entries){
        if(entry.suffix().compare("bin",Qt::CaseInsensitive)!=0)continue;
        QFile file(entry.absoluteFilePath());
        // Identify only the eight-byte core magic; opening still checks the full capture and CRC.
        if(!file.open(QIODevice::ReadOnly|QIODevice::Unbuffered)||file.read(8)!=QByteArray("CAPDDR2\0",8))continue;
        if(count++>=500)break;
        auto* item=new QListWidgetItem(nativeIcon(NativeIcon::Record),QStringLiteral("%1    ·    %2 MiB").arg(entry.fileName(),format(entry.size()/1048576.0,1)),files_);item->setData(Qt::UserRole,entry.absoluteFilePath());item->setToolTip(entry.absoluteFilePath());if(entry.absoluteFilePath()==currentPath_)files_->setCurrentItem(item);
        if(count<=40){auto* quick=new QListWidgetItem(nativeIcon(NativeIcon::Record,QColor("#737d89")),entry.fileName(),sidebarFiles_);quick->setData(Qt::UserRole,entry.absoluteFilePath());quick->setToolTip(entry.absoluteFilePath());if(entry.absoluteFilePath()==currentPath_)sidebarFiles_->setCurrentItem(quick);}
    }
    if(!count){auto* item=new QListWidgetItem(QStringLiteral("此目录没有采集记录"),files_);item->setFlags(Qt::NoItemFlags);}
    if(count>500)directoryLabel_->setText(directoryLabel_->text()+QStringLiteral(" · 仅显示前 500 个文件"));
}
void MainWindow::openDemo() {
    const QStringList paths{QDir(QApplication::applicationDirPath()).filePath("samples/synthetic_10s.BIN"),QDir(QApplication::applicationDirPath()).filePath("../../samples/synthetic_10s.BIN"),QDir::current().filePath("artifacts/samples/synthetic_10s.BIN")};
    for(const auto& path:paths)if(QFileInfo::exists(path)){openCapture(path);return;}
    const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("打开合成演示数据"),QString(),QStringLiteral("BIN (*.BIN *.bin)"));
    if(!path.isEmpty())openCapture(path);
}
void MainWindow::updateChannels() {for(int i=0;i<3;++i){waveform_->setChannelVisible(i,waveChannels_[i]->isChecked());spectrum_->setChannelVisible(i,spectrumChannels_[i]->isChecked());}}
void MainWindow::selectWaveCursor(double time) {
    if(!capture_||!result_)return;
    // Snap once to the validated raw record. setCursor only repaints; it never
    // invokes cursorSelected, so every plot shares this lookup without a loop.
    const auto point=waveform_cursor(*capture_,result_->config,time);
    waveCursorTime_=point.time_s;
    waveform_->setCursor(point.time_s);
    for(auto* plot:waveSingle_)plot->setCursor(point.time_s);
    waveCursor_->setText(QStringLiteral("%1 s　 CH1 %2　 CH2 %3　 CH3 %4 g")
        .arg(format(point.time_s,5),format(point.values[0],5),format(point.values[1],5),format(point.values[2],5)));
}
void MainWindow::updateWaveform() {
    if(!capture_||!result_)return;
    const double start=rangeStart_->value(),end=rangeEnd_->value();
    waveRangePicker_->setText(QStringLiteral("%1 — %2 s").arg(format(start,5),format(end,5)));
    if(appliedWaveStart_==start&&appliedWaveEnd_==end){++previewCacheHits_;setProperty("wavePreviewCacheHits",qulonglong(previewCacheHits_));return;}
    QElapsedTimer elapsed;elapsed.start();
    QVector<PlotSeries> series;
    auto cached=std::find_if(wavePreviews_.begin(),wavePreviews_.end(),[=](const WavePreview& preview){return preview.start==start&&preview.end==end;});
    if(cached!=wavePreviews_.end()){
        // QVector shares immutable point buffers; a range revisit never copies
        // the capture or repeats the selected-record extrema scan.
        const auto entry=*cached;series=entry.series;wavePreviews_.erase(cached);wavePreviews_.push_back(entry);++previewCacheHits_;
    }else{
        const auto preview=waveform_preview(*capture_,result_->config,start,end,1800);
        for(int axis=0;axis<3;++axis){PlotSeries s{channelNames[axis],colors[axis],{}};s.points.reserve(int(preview.size()));for(const auto& p:preview)s.points.push_back({p.time_s,p.values[axis]});series.push_back(std::move(s));}
        if(wavePreviews_.size()==8)wavePreviews_.removeFirst();wavePreviews_.push_back({start,end,series});++previewBuilds_;
    }
    setProperty("wavePreviewBuilds",qulonglong(previewBuilds_));setProperty("wavePreviewCacheHits",qulonglong(previewCacheHits_));setProperty("wavePreviewLastNs",elapsed.nsecsElapsed());
    appliedWaveStart_=start;appliedWaveEnd_=end;
    // Each single-channel chart shares the same immutable point buffer as the
    // overlay/cache. Do not rescan the capture for individual plots.
    for(int axis=0;axis<3;++axis){
        waveSingle_[axis]->setChart(QStringLiteral("%1 · 独立幅值").arg(channelNames[axis]),
            QStringLiteral("时间 / s"),QStringLiteral("g"),{series[axis]});
        waveSingle_[axis]->setXRange(start,end);
    }
    waveform_->setChart(QStringLiteral("三通道叠加 · 共用幅值"),QStringLiteral("时间 / s"),QStringLiteral("g"),std::move(series),
        QStringLiteral("选择上方通道以显示叠加"));
    waveform_->setXRange(start,end);
    waveCursor_->setText(QStringLiteral("点选或拖动波形查看数值"));updateChannels();
    if(waveCursorTime_&&*waveCursorTime_>=start&&*waveCursorTime_<=end)selectWaveCursor(*waveCursorTime_);
}
void MainWindow::adjustWaveRange(double scale,double shift) {
    if(!capture_||!result_||result_->duration_s<=0)return;
    const double duration=result_->duration_s;
    const double minimum=std::max(1e-5,1.0/capture_->header.sample_rate_hz);
    const double span=std::clamp(rangeEnd_->value()-rangeStart_->value(),minimum,duration);
    const double width=std::clamp(span*scale,minimum,duration);
    double center=(rangeStart_->value()+rangeEnd_->value())*.5;
    if(scale!=1&&waveCursorTime_&&*waveCursorTime_>=rangeStart_->value()&&*waveCursorTime_<=rangeEnd_->value())center=*waveCursorTime_;
    const double start=std::clamp(center+span*shift-width*.5,0.0,std::max(0.0,duration-width));
    rangeStart_->setValue(start);rangeEnd_->setValue(start+width);updateWaveform();
}

MainWindow::~MainWindow() {
    // Parsing owns no device handles. Let an in-flight finite file analysis finish
    // before Qt destroys its thread object; no background callback outlives us.
    if (worker_) worker_->wait();
}

bool MainWindow::isAnalyzing() const { return worker_ != nullptr; }

void MainWindow::setBanner(const QString& message, const QString& kind) {
    QString background = "#edf1f6", color = "#526479";
    if (kind == "error") { background = "#fbece9"; color = "#ae5149"; }
    if (kind == "test" || kind == "warning") { background = "#fff0d9"; color = "#956320"; }
    if (kind == "ready") { background = "#e6eefb"; color = "#2c67c5"; }
    banner_->setText(message);
    banner_->setStyleSheet(QStringLiteral("background:%1;color:%2;padding:0 9px;"
                                         "border-radius:5px;font-size:12px;").arg(background, color));
}

void MainWindow::clearResults() {
    result_.reset();
    capture_.reset();
    wavePreviews_.clear();appliedWaveStart_.reset();appliedWaveEnd_.reset();waveCursorTime_.reset();previewBuilds_=previewCacheHits_=0;
    setProperty("wavePreviewBuilds",qulonglong(0));setProperty("wavePreviewCacheHits",qulonglong(0));
    exportButton_->setEnabled(false);
    csvButton_->setEnabled(false);
    exportMenuButton_->setEnabled(false);waveRangePicker_->setEnabled(false);waveRangePicker_->setText(QStringLiteral("时间区间"));
    rangeButton_->setEnabled(false);fullButton_->setEnabled(false);rangeStart_->setEnabled(false);rangeEnd_->setEnabled(false);
    for(auto* button:waveNavigation_)button->setEnabled(false);
    fileMetadata_->setText(QStringLiteral("<span style='font-size:17px;color:#55707f'>文件详情</span><br><br><span style='color:#8b99a2'>选择记录后查看校验结果与文件信息。</span>"));
    waveCursor_->setText(QStringLiteral("打开文件后查看波形"));frequencyCursor_->setText(QStringLiteral("点击频谱读取最近频点"));frequencyPeaks_->clear();speedStats_->clear();bandTable_->clear();speedHeading_->setText(QStringLiteral("来自文件记录"));
    frequencyReference_->setText(QStringLiteral("打开记录后查看有效转频参考。"));
    spectrum_->setReferenceLines({});
    for(int axis=0;axis<3;++axis){waveSingle_[axis]->clearXRange();waveSingle_[axis]->setChart(QStringLiteral("%1 · 独立幅值").arg(channelNames[axis]),QStringLiteral("时间 / s"),QStringLiteral("g"),{});}
    waveform_->clearXRange();
    waveform_->setChart(QStringLiteral("三通道叠加 · 共用幅值"), QStringLiteral("时间 / s"), QStringLiteral("g"), {});
    spectrum_->setChart({}, QStringLiteral("频率 / Hz"), {}, {});
    speed_->setChart({}, QStringLiteral("s"), {}, {}, QStringLiteral("暂无测速"));
    metrics_->setText(QStringLiteral("<span style='font-size:32px;color:#324d5c'>—</span> "
                                    "<span style='font-size:12px;color:#839098'>RPM</span>"));
    diagnosis_->setText(QStringLiteral("未分类"));
    features_->setText(QStringLiteral("<span style='color:#97a1a7'>暂无特征</span>"));
    evidence_->setPlainText(QStringLiteral("选择采集文件后显示完整分析信息。\n\n当前版本不预设故障类别或准确率。"));
    calibration_->hide();
    sampleInfo_->clear();
    footer_->clear();
    detailsButton_->setEnabled(false);
    updateDetection();
}

void MainWindow::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开 SD 采集文件"),
        currentPath_.isEmpty() ? QString() : QFileInfo(currentPath_).absolutePath(),
        QStringLiteral("采集文件 (*.BIN *.bin);;所有文件 (*)"));
    if (!path.isEmpty()) openCapture(path);
}

void MainWindow::openCapture(const QString& path) {
    if (isAnalyzing()) return;
    currentPath_ = QFileInfo(path).absoluteFilePath();
    directory_=QFileInfo(currentPath_).absolutePath();refreshFiles();
    // Source confirmation describes this recording, not every later file.
    config_.signal_kind = SignalKind::Unknown;
    synthetic_ = false;
    metadataNotice_.clear();
    beginAnalysis();
}

void MainWindow::setBusy(bool busy) {
    acquisition_->setAnalysisBusy(busy);
    sidebarFiles_->setEnabled(!busy);
    openButton_->setEnabled(!busy);
    parametersButton_->setEnabled(!busy);
    exportButton_->setEnabled(!busy && bool(result_));
    csvButton_->setEnabled(!busy && bool(result_));
    exportMenuButton_->setEnabled(!busy&&bool(result_));waveRangePicker_->setEnabled(!busy&&bool(result_));detectionConfigButton_->setEnabled(!busy);
    directoryButton_->setEnabled(!busy);refreshButton_->setEnabled(!busy);demoButton_->setEnabled(!busy);files_->setEnabled(!busy);
    rangeButton_->setEnabled(!busy && bool(result_));fullButton_->setEnabled(!busy && bool(result_));rangeStart_->setEnabled(!busy && bool(result_));rangeEnd_->setEnabled(!busy && bool(result_));
    for(auto* button:waveNavigation_)button->setEnabled(!busy&&bool(result_));
    loadProfileButton_->setEnabled(!busy);
    clearProfileButton_->setEnabled(!busy&&(bool(detectionProfile_)||!detectionProfileError_.isEmpty()));
    baselineButton_->setEnabled(!busy&&bool(result_));
    if(auto* ppr=findChild<QPushButton*>("speedPprParameters"))ppr->setEnabled(!busy);
    for(auto* action:detectionCards_->parentWidget()->findChildren<QPushButton*>())action->setEnabled(!busy);
}

void MainWindow::beginAnalysis() {
    if (currentPath_.isEmpty() || isAnalyzing()) return;
    clearResults();
    static_cast<ElidedProfileLabel*>(fileLabel_)->setFullText(QFileInfo(currentPath_).fileName(),currentPath_);
    static_cast<ElidedProfileLabel*>(sidebarRecord_)->setFullText(QFileInfo(currentPath_).fileName(),currentPath_);
    fileLabel_->setToolTip(currentPath_);
    setBanner(QStringLiteral("分析中"), "idle");
    setBusy(true);
    const auto config = config_;
    auto* thread = new AnalysisThread(currentPath_, config, this);
    worker_ = thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        worker_ = nullptr;
        if (thread->result) {
            result_ = std::move(thread->result);
            capture_ = std::move(thread->capture);
            showResult();
        } else {
            clearResults();
            setBanner(QStringLiteral("文件无效"), "error");
            banner_->setToolTip(thread->error);
            sampleInfo_->setText(QStringLiteral("检查文件与参数"));
            fileMetadata_->setText(QStringLiteral("<span style='font-size:19px;color:#96534c'>文件无效</span><br><br>")+thread->error.toHtmlEscaped());
            diagnosis_->setText(QStringLiteral("不可判定"));
            features_->setText(QStringLiteral("<span style='color:#89969e'>无法分析此文件</span>"));
            evidence_->setPlainText(QStringLiteral("错误详情\n") + thread->error +
                QStringLiteral("\n\n请核对原始文件与参数。程序未补零、截断或修改文件。"));
            detailsButton_->setEnabled(true);
        }
        setBusy(false);
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::showResult() {
    const auto& r = *result_;
    // The core reads source metadata together with this capture. A separate UI
    // pre-read could disagree after an edit, or parse invalid JSON differently.
    synthetic_ = r.source_metadata.synthetic;
    config_ = r.config;
    metadataNotice_ = r.source_metadata.status == "invalid_or_unreadable_sidecar"
        ? QStringLiteral("来源说明无效或无法读取，修复前不能确认数据来源。") : QString();
    const QString unit = QStringLiteral("g");
    calibration_->setVisible(true);
    calibration_->setText(r.config.calibration_confirmed?QStringLiteral("已标定"):QStringLiteral("未标定"));
    calibration_->setStyleSheet(r.config.calibration_confirmed?
        QStringLiteral("background:#e6eefb;color:#2c67c5;border-radius:5px;padding:0 10px;font-size:12px;"):
        QStringLiteral("background:#fff0d9;color:#956320;border-radius:5px;padding:0 10px;font-size:12px;"));
    calibration_->setToolTip(r.config.calibration_confirmed?QStringLiteral("用户已确认实际标定；这不是独立诊断验证。"):QStringLiteral("加速度按名义灵敏度与前端增益换算，尚未确认实际标定。"));
    sampleInfo_->setText((metadataNotice_.isEmpty() ? QString() : QStringLiteral("来源说明需修复 · ")) +
        QStringLiteral("%1 kSPS / 通道 · %2 s")
            .arg(QString::number(r.header.sample_rate_hz / 1000.0, 'g', 4), format(r.duration_s, 2)));
    QVector<PlotSeries> frequencySeries;
    for (int axis = 0; axis < 3; ++axis) {
        PlotSeries freq{channelNames[axis], colors[axis], {}};
        const auto& sp = r.spectra[axis];
        freq.points.reserve(int(sp.frequency_hz.size()));
        for (std::size_t k = 0; k < sp.frequency_hz.size() && k < sp.peak_amplitude.size(); ++k)
            freq.points.push_back({sp.frequency_hz[k], sp.peak_amplitude[k]});
        frequencySeries.push_back(std::move(freq));
    }
    spectrum_->setChart(QStringLiteral("幅值谱"), QStringLiteral("Hz"), QStringLiteral("幅值 / g"), std::move(frequencySeries));
    spectrum_->setXRange(0, std::min(r.config.spectrum_max_hz, double(r.header.sample_rate_hz) / 2));
    const bool havePpr = bool(r.speed.pulses_per_rev);
    PlotSeries rpm{{}, colors[0], {}};
    for (const auto& point : r.speed.trace) {
        const auto value = havePpr ? point.rpm : point.pulse_frequency_hz;
        rpm.points.push_back({point.time_s, value.value_or(std::numeric_limits<double>::quiet_NaN())});
    }
    speed_->setChart({}, QStringLiteral("s"), havePpr ? QStringLiteral("RPM") : QStringLiteral("Hz"), {rpm},
                     QStringLiteral("无有效测速"));
    speed_->setToolTip(havePpr ? QStringLiteral("测速缺测保留为空，不将缺测解释为停转。") :
        QStringLiteral("PPR 未设置，曲线显示有效脉冲频率（Hz），不计算 RPM。"));
    QString speedCaption = QStringLiteral("PPR %1 · 有效 %2%")
        .arg(r.speed.pulses_per_rev ? QString::number(*r.speed.pulses_per_rev, 'g', 4) : QStringLiteral("未知"),
             format(r.speed.valid_fraction * 100.0, 1));
    if (!havePpr && r.speed.held_mean_pulse_hz)
        speedCaption = QStringLiteral("脉冲 %1 Hz · PPR 未知 · 有效 %2%")
            .arg(format(*r.speed.held_mean_pulse_hz, 1),format(r.speed.valid_fraction*100.0,1));
    metrics_->setText(QStringLiteral("<span style='font-size:13px;color:#526479'>平均转速</span><br><span style='font-size:38px;font-weight:500;color:#1d2b3b'>%1</span> "
        "<span style='font-size:13px;color:#78899c'>RPM</span><br><br>"
        "<span style='font-size:13px;color:#526479'>%2</span>")
        .arg(r.speed.held_mean_rpm ? format(*r.speed.held_mean_rpm, 1) : QStringLiteral("—"), speedCaption));

    QString featureTable=QStringLiteral("<table width='100%' cellspacing='0' cellpadding='5'><tr style='color:#8596a0;font-size:12px'><td>时域特征</td><td align='right'>均值 / g</td><td align='right'>交流 RMS / g</td><td align='right'>峰值 / g</td><td align='right'>峰值因子</td><td align='right'>峭度</td></tr>");
    for(int axis=0;axis<3;++axis){const auto& t=r.time_stats[axis];featureTable+=QStringLiteral("<tr><td style='color:%1'>%2</td><td align='right'>%3</td><td align='right'>%4</td><td align='right'>%5</td><td align='right'>%6</td><td align='right'>%7</td></tr>").arg(colors[axis].name(),channelNames[axis],format(t.mean,4),format(t.ac_rms,4),format(t.abs_peak,4),optionalNumber(t.crest_factor,2),optionalNumber(t.pearson_kurtosis,2));}
    features_->setText(featureTable+"</table>");
    QString bands=QStringLiteral("<table width='100%' cellspacing='0' cellpadding='5'><tr style='color:#8596a0;font-size:12px'><td>转频频带 RMS / g</td><td align='right'>1X</td><td align='right'>2X</td><td align='right'>3X</td></tr>");
    for(int axis=0;axis<3;++axis){bands+=QStringLiteral("<tr><td style='color:%1'>%2</td>").arg(colors[axis].name(),channelNames[axis]);for(const auto& band:r.order_bands[axis])bands+=QStringLiteral("<td align='right'>%1</td>").arg(band.rms?format(*band.rms,5):QStringLiteral("—"));bands+="</tr>";}
    bandTable_->setText(bands+"</table>");
    speedHeading_->setText(QStringLiteral("文件记录"));
    speedStats_->setText(QStringLiteral("最小 %1 RPM\n\n最大 %2 RPM").arg(r.speed.min_rpm?format(*r.speed.min_rpm,1):QStringLiteral("—"),r.speed.max_rpm?format(*r.speed.max_rpm,1):QStringLiteral("—")));
    speed_->setChart(QStringLiteral("转速记录"),QStringLiteral("时间 / s"),havePpr?QStringLiteral("RPM"):QStringLiteral("脉冲 / Hz"),{rpm},QStringLiteral("无有效测速"));
    speed_->setXRange(0,r.duration_s);
    QVector<QPointF> references;
    for(const auto& band:r.order_bands[0])if(band.rms&&band.center_hz)references.push_back({*band.center_hz,double(band.order)});
    spectrum_->setReferenceLines(references);
    QStringList referenceTexts;for(const auto& band:r.order_bands[0])if(band.rms&&band.center_hz)referenceTexts<<QStringLiteral("%1X     %2 Hz").arg(band.order).arg(format(*band.center_hz,2));
    frequencyReference_->setText(referenceTexts.isEmpty()?QStringLiteral("测速或工况条件未满足，暂无转频参考。"):referenceTexts.join(QStringLiteral("\n\n")));
    QStringList peakTexts;
    for(int axis=0;axis<3;++axis){const auto& sp=r.spectra[axis];if(sp.peak_amplitude.empty())continue;const auto begin=sp.peak_amplitude.size()>1?sp.peak_amplitude.begin()+1:sp.peak_amplitude.begin();const auto k=std::size_t(std::max_element(begin,sp.peak_amplitude.end())-sp.peak_amplitude.begin());peakTexts<<QStringLiteral("%1 %2 Hz / %3 g").arg(channelNames[axis],format(sp.frequency_hz[k],2),format(sp.peak_amplitude[k],4));}
    frequencyPeaks_->setText(peakTexts.join(QStringLiteral("   ·   ")));
    rangeStart_->setRange(0,r.duration_s);rangeEnd_->setRange(0,r.duration_s);rangeStart_->setSingleStep(1.0/r.header.sample_rate_hz);rangeEnd_->setSingleStep(1.0/r.header.sample_rate_hz);rangeStart_->setValue(0);rangeEnd_->setValue(r.duration_s);updateWaveform();
    fileMetadata_->setText(QStringLiteral("<span style='font-size:18px;font-weight:600'>校验通过</span><br><br><span style='color:#81939e'>记录时长</span>　%1 s<br><br><span style='color:#81939e'>采样率</span>　%2 kSPS / 通道<br><br><span style='color:#81939e'>记录数</span>　%3<br><br><span style='color:#81939e'>存储通道</span>　ADC1 / ADC3 / ADC5<br><br><span style='color:#81939e'>完整性</span>　CRC / 序号 / 时间通过").arg(format(r.duration_s,2),QString::number(r.header.sample_rate_hz/1000.0,'g',4),QString::number(r.header.completed_records)));
    recentPaths_.removeAll(currentPath_);recentPaths_.prepend(currentPath_);while(recentPaths_.size()>3)recentPaths_.removeLast();
    QStringList recentLines;for(int i=0;i<recentPaths_.size();++i)recentLines<<QStringLiteral("<a style='color:#597686;text-decoration:none' href='%1'>%2</a>").arg(i).arg(fontMetrics().elidedText(QFileInfo(recentPaths_[i]).fileName(),Qt::ElideMiddle,275).toHtmlEscaped());
    recent_->setText(QStringLiteral("本次会话最近打开<br>")+recentLines.join("<br>"));

    QStringList lines;
    lines << QStringLiteral("<b>文件与来源</b>");
    lines << QFileInfo(currentPath_).fileName().toHtmlEscaped();
    lines << (synthetic_ ? QStringLiteral("合成演示：仅供软件验证，不是实际机械测量。") :
        r.config.signal_kind == SignalKind::Unknown ? QStringLiteral("数据来源尚未确认。") :
        r.config.signal_kind == SignalKind::TestSignal ? QStringLiteral("测试信号，不作机械故障结论。") :
        QStringLiteral("用户确认的机械振动记录。"));
    lines << QStringLiteral("数据来自打开的文件，界面不代表设备实时状态。");
    lines << QStringLiteral("<br><b>校验与分析</b>");
    lines << QStringLiteral("PPR：%1 · %2")
        .arg(r.speed.pulses_per_rev ? QString::number(*r.speed.pulses_per_rev, 'g', 10) : QStringLiteral("未知"),
             r.speed.ppr_source == "explicit_user_override" ? QStringLiteral("用户显式指定") :
             r.speed.ppr_source == "file_header" ? QStringLiteral("文件头") : QStringLiteral("未提供"));
    for (const auto& e : r.diagnosis.evidence) lines << translatedEvidence(e).toHtmlEscaped();
    lines << QStringLiteral("<br><b>三通道交流 RMS</b>");
    for (int axis = 0; axis < 3; ++axis)
        lines << QStringLiteral("%1：%2 %3").arg(channelNames[axis], format(r.time_stats[axis].ac_rms, 5), unit).toHtmlEscaped();
    lines << QStringLiteral("<br><b>1X / 2X / 3X 频带 RMS</b>");
    for (int axis = 0; axis < 3; ++axis) {
        QStringList values;
        for (const auto& band : r.order_bands[axis]) values << (band.rms ? format(*band.rms, 5) : QStringLiteral("—"));
        lines << QStringLiteral("%1：%2").arg(channelNames[axis], values.join(" / ")).toHtmlEscaped();
    }
    lines << QStringLiteral("<br><b>解释条件</b>");
    for (const auto& warning : r.warnings) lines << translatedEvidence(warning).toHtmlEscaped();
    for (const auto& limitation : r.diagnosis.limitations) lines << translatedEvidence(limitation).toHtmlEscaped();
    if (!metadataNotice_.isEmpty()) lines << metadataNotice_.toHtmlEscaped();
    evidence_->setHtml(QStringLiteral("<html><body style='font-size:14px;color:#43545f'>%1</body></html>").arg(lines.join("<br>")));
    diagnosis_->setText(r.diagnosis.status == "adc_saturation_requires_review" ? QStringLiteral("检查饱和") : QStringLiteral("未分类"));
    if (synthetic_) setBanner(QStringLiteral("合成演示"), "test");
    else if (r.config.signal_kind == SignalKind::Unknown)
        setBanner(QStringLiteral("来源待确认"), "warning");
    else if (r.config.signal_kind == SignalKind::TestSignal)
        setBanner(QStringLiteral("测试信号"), "test");
    else setBanner(QStringLiteral("机械振动"), "ready");
    banner_->setToolTip(synthetic_ ? QStringLiteral("此文件标记为合成数据，仅供软件功能验证，不用于证明真实故障识别效果。") :
        !metadataNotice_.isEmpty() ? metadataNotice_ : QStringLiteral("来源是对当前文件的声明；故障类别仍需台架与分类器验证。"));
    const auto& sp = r.spectra[0];
    footer_->setText(QStringLiteral("全记录分析　 ·　 FFT %1　 ·　 %2 段　 ·　 Δf %3 Hz")
        .arg(qulonglong(sp.fft_size)).arg(qulonglong(sp.segment_count)).arg(format(sp.bin_width_hz, 3)));
    detailsButton_->setEnabled(true);
    updateDetection();
}

void MainWindow::showDetails() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("analysisDetailsDialog"));
    dialog.setWindowTitle(QStringLiteral("分析详情"));
    dialog.resize(720, 480);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    auto* title = new QLabel(QStringLiteral("分析详情"));
    title->setStyleSheet(QStringLiteral("font-size:18px;font-weight:600;"));
    layout->addWidget(title);
    auto* browser = new QTextBrowser;
    browser->setObjectName(QStringLiteral("detailsContent"));
    browser->setOpenExternalLinks(false);
    browser->setHtml(QStringLiteral("<h3>实验检测框架</h3>")+detectionState_->text().toHtmlEscaped()+
        QStringLiteral("<p>")+detectionProfileLabel_->text().toHtmlEscaped()+QStringLiteral("</p><p>")+(detectionQuality_->toolTip().isEmpty()?detectionQuality_->text():detectionQuality_->toolTip()).toHtmlEscaped()+
        QStringLiteral("</p>")+detectionEvidence_->toHtml()+QStringLiteral("<hr><h3>数值分析与来源</h3>")+evidence_->toHtml());
    layout->addWidget(browser, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::editParameters() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("analysisParametersDialog"));
    dialog.setWindowTitle(QStringLiteral("分析参数"));
    dialog.resize(640, 540);
    auto* dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setContentsMargins(20,18,20,18);dialogLayout->setSpacing(14);
    auto* title=new QLabel(QStringLiteral("分析参数"));title->setStyleSheet("font-size:22px;font-weight:600;color:#1d2b3b;");dialogLayout->addWidget(title);
    auto* tabs=new QTabWidget;tabs->setObjectName("parameterTabs");dialogLayout->addWidget(tabs,1);
    const auto parameterPage=[&](const QString& name){auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);auto* body=new QWidget;body->setStyleSheet("background:#ffffff;");auto* layout=new QVBoxLayout(body);layout->setContentsMargins(18,16,18,16);layout->setSpacing(14);scroll->setWidget(body);QScroller::grabGesture(scroll->viewport(),QScroller::TouchGesture);tabs->addTab(scroll,name);return layout;};
    auto* analysisPage=parameterPage(QStringLiteral("分析"));auto* sourcePage=parameterPage(QStringLiteral("来源与标定"));
    auto* note = new QLabel(QStringLiteral("PPR 为 0 时使用文件值；已知的文件 PPR 不接受冲突覆盖。数值字段可点按输入。"));
    note->setWordWrap(true);note->setStyleSheet("font-size:13px;color:#78899c;");analysisPage->addWidget(note);
    auto* form = new QFormLayout;form->setSpacing(14);analysisPage->addLayout(form);analysisPage->addStretch();
    auto* sourceNote=new QLabel(QStringLiteral("仅在完成实际确认后，更改当前记录的来源和标定声明。"));sourceNote->setWordWrap(true);sourceNote->setStyleSheet("font-size:13px;color:#78899c;");sourcePage->addWidget(sourceNote);
    auto* sourceForm=new QFormLayout;sourceForm->setSpacing(14);sourcePage->addLayout(sourceForm);
    auto* ppr = new PprSpinBox;
    ppr->setAccessibleName(QStringLiteral("每转脉冲数 PPR"));
    ppr->setDecimals(6); ppr->setRange(0, 1e9); ppr->setSpecialValueText(QStringLiteral("未知 / 使用文件值"));
    ppr->setValue(config_.ppr_override.value_or(0)); form->addRow(QStringLiteral("每转脉冲数 PPR"), ppr);
    auto* sensitivity = new TouchDoubleSpinBox;sensitivity->setAccessibleName(QStringLiteral("传感器灵敏度"));
    sensitivity->setRange(0.001, 100000); sensitivity->setDecimals(3); sensitivity->setSuffix(" mV/g");
    sensitivity->setValue(config_.sensor_sensitivity_v_per_g * 1000); sourceForm->addRow(QStringLiteral("传感器灵敏度"), sensitivity);
    auto* gain = new TouchDoubleSpinBox;gain->setAccessibleName(QStringLiteral("前端电压增益"));
    gain->setDecimals(4); gain->setRange(-10000, 10000); gain->setValue(config_.analog_gain);
    sourceForm->addRow(QStringLiteral("前端电压增益（非零）"), gain);
    auto* kind = new QComboBox;
    kind->addItem(QStringLiteral("来源未确认"), int(SignalKind::Unknown));
    kind->addItem(QStringLiteral("电气测试 / 合成信号"), int(SignalKind::TestSignal));
    kind->addItem(QStringLiteral("已确认的机械振动记录"), int(SignalKind::VibrationConfirmed));
    kind->setCurrentIndex(kind->findData(int(config_.signal_kind)));
    const bool sourceLocked = synthetic_ || !metadataNotice_.isEmpty();
    kind->setEnabled(!sourceLocked); sourceForm->addRow(QStringLiteral("当前文件来源"), kind);
    auto* setupId = new QLineEdit(detectionSetupId_);setupId->setObjectName("detectionSetupIdInput");
    setupId->setMaxLength(128);setupId->setPlaceholderText(QStringLiteral("例如 rig01_mountA（变更接线/安装后需区分）"));
    sourceForm->addRow(QStringLiteral("当前台架 / 安装标识"),setupId);
    auto* fft = new QComboBox;
    for (int n : {8192, 16384, 32768, 65536, 131072, 262144}) fft->addItem(QString::number(n), n);
    fft->setCurrentIndex(fft->findData(int(config_.fft_size))); form->addRow(QStringLiteral("FFT 分段长度"), fft);
    auto* maxHz = new TouchDoubleSpinBox;maxHz->setAccessibleName(QStringLiteral("显示频率上限"));
    maxHz->setRange(10, 4000); maxHz->setDecimals(0); maxHz->setSuffix(" Hz");
    maxHz->setValue(config_.spectrum_max_hz); form->addRow(QStringLiteral("显示频率上限"), maxHz);
    auto* calibrated = new QCheckBox(QStringLiteral("已通过实际标定确认灵敏度与前端增益"));
    calibrated->setChecked(config_.calibration_confirmed);
    sourcePage->addWidget(calibrated);
    if (synthetic_) {auto* warning=new QLabel(QStringLiteral("此记录标记为合成数据，不能确认为机械振动。"));warning->setWordWrap(true);warning->setStyleSheet("color:#956320;font-size:13px;");sourcePage->addWidget(warning);}
    else if (!metadataNotice_.isEmpty()) {auto* warning=new QLabel(metadataNotice_);warning->setTextFormat(Qt::PlainText);warning->setWordWrap(true);warning->setStyleSheet("color:#956320;font-size:13px;");sourcePage->addWidget(warning);}
    sourcePage->addStretch();
    if(config_.signal_kind==SignalKind::Unknown||!config_.calibration_confirmed)tabs->setCurrentIndex(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用并分析"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    dialogLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (std::abs(gain->value()) < 1e-12) {
            QMessageBox::warning(&dialog, QStringLiteral("参数无效"), QStringLiteral("前端电压增益不能为零。"));
            return;
        }
        QString setupError;if(!setDetectionSetupId(setupId->text(),&setupError)){
            QMessageBox::warning(&dialog,QStringLiteral("台架标识无效"),setupError);return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    config_.ppr_override = ppr->value() > 0 ? std::optional<double>(ppr->value()) : std::nullopt;
    config_.sensor_sensitivity_v_per_g = sensitivity->value() / 1000.0;
    config_.analog_gain = gain->value();
    config_.signal_kind = synthetic_ ? SignalKind::TestSignal : SignalKind(kind->currentData().toInt());
    config_.fft_size = std::size_t(fft->currentData().toInt());
    config_.spectrum_max_hz = maxHz->value();
    config_.calibration_confirmed = calibrated->isChecked();
    if (!currentPath_.isEmpty()) beginAnalysis();
}

namespace {
QString csvCell(QString value) {
    // User-controlled cells cannot become spreadsheet formulae.
    bool numeric=false;const double parsed=value.toDouble(&numeric);
    if(!value.isEmpty() && !(numeric&&std::isfinite(parsed)) && QString("=+-@\t\r").contains(value.front()))value.prepend(QChar(39));
    value.replace('"',"\"\"");return '"'+value+'"';
}
}
bool MainWindow::saveResult(const QString& target, bool csv, QString* error) const {
    auto fail=[&](const QString& value){if(error)*error=value;return false;};
    if(isAnalyzing())return fail(QStringLiteral("正在分析，请等待完成后导出。"));
    if(!result_)return fail(QStringLiteral("没有可导出的分析结果"));
    const QFileInfo source(currentPath_),targetInfo(target),sidecar(source.absolutePath()+'/'+source.completeBaseName()+".meta.json");
    for(const auto& protectedPath:protectedOutputPaths()) {
        const QFileInfo protectedFile(QString::fromUtf8(protectedPath.data(),int(protectedPath.size())));
        std::error_code ec;
        const bool equivalent=std::filesystem::equivalent(std::filesystem::u8path(target.toUtf8().toStdString()),
            std::filesystem::u8path(protectedFile.absoluteFilePath().toUtf8().toStdString()),ec);
        if(equivalent || QDir::cleanPath(targetInfo.absoluteFilePath())==QDir::cleanPath(protectedFile.absoluteFilePath()) ||
           (!targetInfo.canonicalFilePath().isEmpty()&&targetInfo.canonicalFilePath()==protectedFile.canonicalFilePath()))
            return fail(QStringLiteral("不能覆盖原始 BIN、来源说明或输入检测配置，请选择新路径。"));
    }
    QByteArray data;
    QJsonObject detection=QJsonDocument::fromJson(QByteArray::fromStdString(detection_to_json(detectionResult_))).object();
    detection.insert("profile_path",detectionProfilePath_);
    detection.insert("profile_error",detectionProfileError_);
    if(csv) {
        const auto& r=*result_;QString output=QStringLiteral("field,channel,value,unit\r\n");
        auto row=[&](const QString& field,const QString& channel,const QString& value,const QString& unit){output+=csvCell(field)+','+csvCell(channel)+','+csvCell(value)+','+csvCell(unit)+"\r\n";};
        row("source_path",{},currentPath_,{});row("sidecar_path",{},sidecar.absoluteFilePath(),{});
        row("source_kind",{},r.source_metadata.synthetic?"synthetic":r.config.signal_kind==SignalKind::VibrationConfirmed?"user_confirmed_vibration":r.config.signal_kind==SignalKind::TestSignal?"test_signal":"unconfirmed",{});
        row("sidecar_status",{},text(r.source_metadata.status),{});row("classification",{},text(r.diagnosis.classification),{});
        row("calibration_confirmed",{},r.config.calibration_confirmed?"true":"false",{});
        row("duration",{},QString::number(r.duration_s,'g',17),"s");row("sample_rate",{},QString::number(r.header.sample_rate_hz),"Hz");
        row("ppr",{},r.speed.pulses_per_rev?QString::number(*r.speed.pulses_per_rev,'g',17):QString(),"pulse/rev");row("ppr_source",{},text(r.speed.ppr_source),{});
        row("speed_valid_fraction",{},QString::number(r.speed.valid_fraction,'g',17),{});
        row("held_mean_rpm",{},r.speed.held_mean_rpm?QString::number(*r.speed.held_mean_rpm,'g',17):QString(),"RPM");
        row("detection.status",{},text(detectionResult_.status),{});
        row("detection.classification",{},text(detectionResult_.classification),{});
        row("detection.candidate",{},detectionResult_.candidate?text(*detectionResult_.candidate):QString(),{});
        row("detection.validated",{},"false",{});row("detection.profile_id",{},text(detectionResult_.profile_id),{});
        row("detection.profile_status",{},text(detectionResult_.profile_status),{});row("detection.profile_path",{},detectionProfilePath_,{});
        row("detection.setup_id",{},detectionSetupId_,{});row("detection.profile_error",{},detectionProfileError_,{});
        for(const auto& gate:detectionResult_.gates){row("detection.gate."+text(gate.id)+".passed",{},gate.passed?"true":"false",{});row("detection.gate."+text(gate.id)+".detail",{},text(gate.detail),{});}
        for(const auto& rule:detectionResult_.rules){row("detection.rule."+text(rule.id)+".label",{},text(rule.label),{});row("detection.rule."+text(rule.id)+".matched",{},rule.matched?"true":"false",{});}
        // Keep the entire structured object, including condition and baseline
        // evidence, in addition to the scan-friendly summary rows.
        row("detection.json",{},QString::fromUtf8(QJsonDocument(detection).toJson(QJsonDocument::Compact)),{});
        for(int axis=0;axis<3;++axis){const auto& t=r.time_stats[axis];const auto name=channelNames[axis];
            row("mean",name,QString::number(t.mean,'g',17),text(r.amplitude_unit));row("ac_rms",name,QString::number(t.ac_rms,'g',17),text(r.amplitude_unit));row("abs_peak",name,QString::number(t.abs_peak,'g',17),text(r.amplitude_unit));row("peak_to_peak",name,QString::number(t.peak_to_peak,'g',17),text(r.amplitude_unit));
            row("crest_factor",name,t.crest_factor?QString::number(*t.crest_factor,'g',17):QString(),{});row("pearson_kurtosis",name,t.pearson_kurtosis?QString::number(*t.pearson_kurtosis,'g',17):QString(),{});
            for(const auto& band:r.order_bands[axis]){row(QString("%1X_band_rms").arg(band.order),name,band.rms?QString::number(*band.rms,'g',17):QString(),text(r.amplitude_unit));row(QString("%1X_band_status").arg(band.order),name,text(band.status),{});}}
        data=output.toUtf8();
    }else{
        QJsonObject provenance;provenance.insert("synthetic",result_->source_metadata.synthetic);provenance.insert("sidecar_status",text(result_->source_metadata.status));
        provenance.insert("source_confirmation",result_->source_metadata.synthetic?"synthetic_sidecar":result_->config.signal_kind==SignalKind::VibrationConfirmed?"user_confirmed_vibration":result_->config.signal_kind==SignalKind::TestSignal?"user_declared_test_signal":"unconfirmed");
        auto json=QJsonDocument::fromJson(QByteArray::fromStdString(to_json(*result_))).object();
        if(json.isEmpty()||detection.isEmpty())return fail(QStringLiteral("结果序列化失败"));
        json.insert("gui_source_metadata",provenance);json.insert("detection",detection);
        data=QJsonDocument(json).toJson(QJsonDocument::Indented);
    }
    try {validate_detection_outputs({target.toUtf8().toStdString()},protectedOutputPaths());}
    catch(const std::exception& e){return fail(QString::fromUtf8(e.what()));}
    QSaveFile file(target);
    if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()||!file.commit())return fail(file.errorString());
    return true;
}
void MainWindow::exportResult(bool csv) {
    if(!result_)return;const QFileInfo source(currentPath_);
    const auto target=QFileDialog::getSaveFileName(this,QStringLiteral("保存分析结果"),source.absolutePath()+'/'+source.completeBaseName()+(csv?"_summary.csv":"_analysis.json"),csv?QStringLiteral("CSV (*.csv)"):QStringLiteral("JSON (*.json)"));
    if(target.isEmpty())return;QString error;
    if(!saveResult(target,csv,&error))QMessageBox::warning(this,QStringLiteral("保存失败"),error);
    else QMessageBox::information(this,QStringLiteral("已保存"),QStringLiteral("分析结果已保存。"));
}
} // namespace vibration::gui
