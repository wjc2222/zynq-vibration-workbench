#include "calibration_settings.hpp"
#include "touch_keyboard.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cmath>

namespace vibration::workbench {
namespace {
class CalibrationNumber : public QDoubleSpinBox {
public:
    CalibrationNumber(const QString& title, bool positive, bool nonzero, QWidget* parent = nullptr)
        : QDoubleSpinBox(parent), title_(title), positive_(positive), nonzero_(nonzero) {
        setDecimals(8); setRange(positive ? 0.00000001 : -1e9, 1e9);
        setMinimumHeight(44); setMinimumWidth(135); setKeyboardTracking(false);
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        lineEdit()->setReadOnly(true); lineEdit()->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == lineEdit() && event->type() == QEvent::MouseButtonRelease) {
            bool accepted = false;
            const auto text = TouchKeyboardDialog::getText(this, title_, QString::number(value(), 'g', 12),
                TouchKeyboardDialog::Mode::Decimal, &accepted, [this](const QString& entered) {
                    bool ok = false; const auto number = entered.toDouble(&ok);
                    if (!ok || !std::isfinite(number)) return QStringLiteral("请输入有限数值。");
                    if (positive_ && number <= 0) return QStringLiteral("灵敏度必须大于 0。");
                    if (nonzero_ && number == 0) return QStringLiteral("前端增益不能为 0，可用负号表示反相。");
                    if (number < minimum() || number > maximum()) return QStringLiteral("数值超出可输入范围。");
                    return QString();
                });
            if (accepted) setValue(text.toDouble());
            return true;
        }
        return QDoubleSpinBox::eventFilter(object, event);
    }
private:
    QString title_; bool positive_, nonzero_;
};
QString errorText(const std::exception& error) { return QString::fromUtf8(error.what()); }
class SensitivityNumber final : public CalibrationNumber {
public:
    explicit SensitivityNumber(const QString& name):CalibrationNumber(name,true,false){}
protected:
    QString textFromValue(double value) const override {return QString::number(value,'g',10);}
};
bool sameParameters(const AnalysisConfig& a, const AnalysisConfig& b) {
    auto x = a, y = b; x.amplitude_unit = y.amplitude_unit = AmplitudeUnit::Gravity;
    return calibration_config_equal(x, y);
}
double jsonNumber(const QJsonObject& object, const QString& key) {
    const auto value = object.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        throw AnalysisError("Missing or invalid calibration number: " + key.toStdString());
    return value.toDouble();
}
} // namespace

SensitivitySettings::SensitivitySettings(const AnalysisConfig& config, QWidget* parent)
    : QWidget(parent) {
    setObjectName("accelerationSensitivitySettings");
    auto* row = new QHBoxLayout(this); row->setContentsMargins(8,8,8,8); row->setSpacing(10);
    auto* title=new QLabel(QStringLiteral("灵敏度 · mV/(m/s²)"));title->setFixedWidth(190);row->addWidget(title);
    for (std::size_t channel=0;channel<3;++channel) {
        const auto name=QStringLiteral("CH%1").arg(channel+1);
        auto* label=new QLabel(name);label->setFixedWidth(36);row->addWidget(label);
        auto* field=new SensitivityNumber(name+QStringLiteral(" 灵敏度 · mV/(m/s²)"));
        field->setObjectName(QStringLiteral("accelerationSensitivity%1").arg(channel+1));
        field->setMinimumWidth(90);field->setMaximumWidth(180);
        sensitivity_[channel]=field;row->addWidget(field,1);
    }
    auto* apply=new QPushButton(QStringLiteral("应用"));apply->setObjectName("applyAccelerationSensitivity");
    apply->setFixedSize(80,44);row->addStretch();row->addWidget(apply);
    connect(apply,&QPushButton::clicked,this,[this]{if(applyRequested)applyRequested();});
    setConfig(config);
}
void SensitivitySettings::setConfig(const AnalysisConfig& config) {
    original_=config;
    for(std::size_t ch=0;ch<3;++ch) {
        sensitivity_[ch]->setValue(effective_channel_calibration(config,ch).sensitivity_mv_per_m_s2);
        displayed_[ch]=sensitivity_[ch]->value();
    }
}
bool SensitivitySettings::applyTo(AnalysisConfig& config, QString* error) const {
    try {
        auto candidate=config;std::array<ChannelCalibration,3> channels;
        bool changed=false;
        for(std::size_t ch=0;ch<3;++ch) {
            channels[ch]=effective_channel_calibration(config,ch);
            if(sensitivity_[ch]->value()!=displayed_[ch]) {
                changed=true;channels[ch].sensitivity_mv_per_m_s2=sensitivity_[ch]->value();
            }
        }
        if(changed) {candidate.channel_calibration=channels;candidate.calibration_confirmed=false;}
        validate_calibration_config(candidate);config=candidate;
        if(error)error->clear();return true;
    } catch(const std::exception& e) {if(error)*error=QString::fromUtf8(e.what());return false;}
}

CalibrationSettings::CalibrationSettings(const AnalysisConfig& config, QWidget* parent)
    : QWidget(parent), original_(config) {
    setObjectName("channelCalibrationSettings");
    auto* body = new QVBoxLayout(this); body->setContentsMargins(0, 0, 0, 0); body->setSpacing(10);
    auto* notice = new QLabel(QStringLiteral("三个通道分别使用自己的灵敏度与前端增益。输入的是换算参数，不代表已完成实测标定。"));
    notice->setObjectName("calibrationNominalNotice"); notice->setWordWrap(true); body->addWidget(notice);
    auto* form = new QFormLayout;
    unit_ = new QComboBox; unit_->setObjectName("calibrationAmplitudeUnit"); unit_->setMinimumHeight(44);
    unit_->addItem(QStringLiteral("电压 · V"), int(AmplitudeUnit::Voltage));
    unit_->addItem(QStringLiteral("加速度 · m/s²"), int(AmplitudeUnit::AccelerationSI));
    unit_->addItem(QStringLiteral("加速度 · g"), int(AmplitudeUnit::Gravity));
    unit_->setCurrentIndex(unit_->findData(int(config.amplitude_unit)));
    form->addRow(QStringLiteral("显示与分析单位"), unit_); body->addLayout(form);
    auto* grid = new QGridLayout; grid->setHorizontalSpacing(10); grid->setVerticalSpacing(8);
    grid->addWidget(new QLabel(QStringLiteral("通道")), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("灵敏度 · mV/(m/s²)")), 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("前端增益 · 倍")), 0, 2);
    grid->addWidget(new QLabel(QStringLiteral("显式零点 · V")), 0, 3);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto p = effective_channel_calibration(config, channel);
        const auto label = QStringLiteral("CH%1").arg(channel + 1);
        sensitivity_[channel] = new CalibrationNumber(label + QStringLiteral(" 灵敏度 · mV/(m/s²)"), true, false);
        gain_[channel] = new CalibrationNumber(label + QStringLiteral(" 前端增益"), false, true);
        bias_[channel] = new CalibrationNumber(label + QStringLiteral(" 显式电压零点 · V"), false, false);
        sensitivity_[channel]->setObjectName(QString("calibrationSensitivity%1").arg(channel + 1));
        gain_[channel]->setObjectName(QString("calibrationGain%1").arg(channel + 1));
        bias_[channel]->setObjectName(QString("calibrationBias%1").arg(channel + 1));
        sensitivity_[channel]->setValue(p.sensitivity_mv_per_m_s2); gain_[channel]->setValue(p.analog_gain); bias_[channel]->setValue(p.voltage_bias_v);
        displayed_[channel] = {sensitivity_[channel]->value(), gain_[channel]->value(), bias_[channel]->value()};
        sensitivity_[channel]->setToolTip(QStringLiteral("示例：2.00 mV/(m/s²)。填写本通道传感器参数；示例不等于实测标定结果。"));
        gain_[channel]->setToolTip(QStringLiteral("输入本通道前端的有符号电压增益，不能为 0；反相前端填负值。"));
        bias_[channel]->setToolTip(QStringLiteral("仅填写已知电压零点；默认为 0，不会自动减去记录均值。"));
        grid->addWidget(new QLabel(label), int(channel) + 1, 0);
        grid->addWidget(sensitivity_[channel], int(channel) + 1, 1);
        grid->addWidget(gain_[channel], int(channel) + 1, 2);
        grid->addWidget(bias_[channel], int(channel) + 1, 3);
    }
    body->addLayout(grid);
    auto* formula = new QLabel(QStringLiteral("a = 1000 × (V − 零点) / (增益 × 灵敏度)，单位 m/s²；g = a / 9.80665。\n电压模式保留原始 ADC 电压；时域不自动去直流，频谱预处理逐段去均值。"));
    formula->setObjectName("calibrationFormula"); formula->setWordWrap(true); body->addWidget(formula);
}

AnalysisConfig CalibrationSettings::edited() const {
    auto config = original_; config.amplitude_unit = AmplitudeUnit(unit_->currentData().toInt());
    bool changed = false;
    std::array<ChannelCalibration, 3> values;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const std::array<double, 3> current{sensitivity_[channel]->value(), gain_[channel]->value(), bias_[channel]->value()};
        const auto old = effective_channel_calibration(original_, channel);
        values[channel] = {current[0] == displayed_[channel][0] ? old.sensitivity_mv_per_m_s2 : current[0],
            current[1] == displayed_[channel][1] ? old.analog_gain : current[1],
            current[2] == displayed_[channel][2] ? old.voltage_bias_v : current[2]};
        changed = changed || current != displayed_[channel];
    }
    if (changed || original_.channel_calibration) config.channel_calibration = values;
    return config;
}
bool CalibrationSettings::validate(QString* error) const {
    try { validate_calibration_config(edited()); if (error) error->clear(); return true; }
    catch (const std::exception& e) { if (error) *error = errorText(e); return false; }
}
bool CalibrationSettings::applyTo(AnalysisConfig& config, QString* error) const {
    try {
        const auto candidate = edited(); validate_calibration_config(candidate);
        const bool changed = !sameParameters(config, candidate);
        config.amplitude_unit = candidate.amplitude_unit; config.channel_calibration = candidate.channel_calibration;
        config.sensor_sensitivity_v_per_g = candidate.sensor_sensitivity_v_per_g; config.analog_gain = candidate.analog_gain;
        if (changed) config.calibration_confirmed = false;
        if (error) error->clear(); return true;
    } catch (const std::exception& e) { if (error) *error = errorText(e); return false; }
}

CalibrationSettingsDialog::CalibrationSettingsDialog(const AnalysisConfig& config, QWidget* parent)
    : QDialog(parent), result_(config) {
    setObjectName("calibrationSettingsDialog"); setWindowTitle(QStringLiteral("三通道加速度换算")); resize(760, 520);
    auto* body = new QVBoxLayout(this); auto* scroll = new QScrollArea; scroll->setWidgetResizable(true);
    auto* settings = new CalibrationSettings(config); scroll->setWidget(settings); body->addWidget(scroll, 1);
    auto* error = new QLabel; error->setWordWrap(true); error->setStyleSheet("color:#b73e36"); body->addWidget(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用")); buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    for (auto* button : buttons->buttons()) button->setMinimumHeight(44); body->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, settings, error] {
        QString message; if (!settings->applyTo(result_, &message)) { error->setText(message); return; } accept();
    });
}

QJsonObject calibration_settings_json(const AnalysisConfig& config) {
    validate_calibration_config(config);
    QJsonObject object{{"schema", "zynq_vibration.calibration_parameters.v1"},
        {"amplitude_unit", QString::fromStdString(amplitude_unit_symbol(config))},
        {"sensor_sensitivity_v_per_g", config.sensor_sensitivity_v_per_g}, {"analog_gain", config.analog_gain},
        {"parameter_source", "user_configuration_not_measurement"}};
    if (!config.channel_calibration) object["channel_calibration"] = QJsonValue(QJsonValue::Null);
    else {
        QJsonArray channels;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto p = effective_channel_calibration(config, channel);
            channels.append(QJsonObject{{"channel", int(channel + 1)}, {"sensitivity_mv_per_m_s2", p.sensitivity_mv_per_m_s2},
                {"analog_gain", p.analog_gain}, {"voltage_bias_v", p.voltage_bias_v}});
        }
        object["channel_calibration"] = channels;
    }
    return object;
}
bool apply_calibration_settings_json(const QJsonObject& object, AnalysisConfig& config, QString* error) {
    try {
        auto candidate = config;
        if (object.contains("schema") && object.value("schema").toString() != "zynq_vibration.calibration_parameters.v1")
            throw AnalysisError("Unsupported calibration parameter schema");
        if (object.contains("amplitude_unit")) {
            const auto unit = object.value("amplitude_unit").toString();
            if (unit != "V" && unit != "m/s^2" && unit != "g") throw AnalysisError("Invalid saved amplitude unit");
            candidate.amplitude_unit = unit == "V" ? AmplitudeUnit::Voltage : unit == "g" ? AmplitudeUnit::Gravity : AmplitudeUnit::AccelerationSI;
        }
        if (object.contains("sensor_sensitivity_v_per_g")) candidate.sensor_sensitivity_v_per_g = jsonNumber(object, "sensor_sensitivity_v_per_g");
        if (object.contains("analog_gain")) candidate.analog_gain = jsonNumber(object, "analog_gain");
        const auto channels = object.value("channel_calibration");
        if (channels.isArray()) {
            const auto array = channels.toArray(); if (array.size() != 3) throw AnalysisError("Exactly three saved channel conversions are required");
            std::array<ChannelCalibration, 3> values;
            for (int channel = 0; channel < 3; ++channel) {
                if (!array[channel].isObject()) throw AnalysisError("Invalid saved channel conversion");
                const auto entry = array[channel].toObject();
                if (entry.contains("channel") && jsonNumber(entry, "channel") != channel + 1) throw AnalysisError("Saved channel order does not match CH1/CH2/CH3");
                values[std::size_t(channel)] = {jsonNumber(entry, "sensitivity_mv_per_m_s2"), jsonNumber(entry, "analog_gain"), jsonNumber(entry, "voltage_bias_v")};
            }
            candidate.channel_calibration = values;
        } else if (channels.isNull() || channels.isUndefined()) candidate.channel_calibration.reset();
        else throw AnalysisError("Invalid saved channel calibration field");
        validate_calibration_config(candidate); candidate.calibration_confirmed = false;
        config = candidate; if (error) error->clear(); return true;
    } catch (const std::exception& e) { if (error) *error = errorText(e); return false; }
}

} // namespace vibration::workbench
