#include "calibration_settings.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <cmath>
#include <iostream>

namespace {
unsigned checks = 0;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); ++checks; }
template<class T> T* find(QWidget& parent, const char* name) {
    auto* value = parent.findChild<T*>(QString::fromUtf8(name)); check(value != nullptr, "Missing calibration control"); return value;
}
void touchNumber(QDoubleSpinBox* field, const QString& value, const QString& invalid = {}) {
    bool visited = false, valid = true;
    QTimer::singleShot(0, [&, value, invalid] {
        auto* dialog = QApplication::activeModalWidget();
        if (!dialog || dialog->objectName() != "workbenchNumberPad") { valid = false; return; }
        visited = true;
        auto* clear = dialog->findChild<QPushButton*>("touchKeyboardClear");
        auto* apply = dialog->findChild<QPushButton*>("touchKeyboardApply");
        auto* error = dialog->findChild<QLabel*>("touchKeyboardError");
        if (!clear || !apply || !error) { valid = false; qobject_cast<QDialog*>(dialog)->reject(); return; }
        const auto type = [&](const QString& text) {
            clear->click();
            for (const auto character : text) {
                auto* key = dialog->findChild<QPushButton*>("touchKey_" + QString(character));
                if (!key) { valid = false; return; } key->click();
            }
        };
        if (!invalid.isEmpty()) {
            type(invalid); apply->click();
            if (error->text().isEmpty() || QApplication::activeModalWidget() != dialog) valid = false;
        }
        type(value); apply->click();
    });
    auto* editor = field->findChild<QLineEdit*>(); check(editor != nullptr, "Missing calibration number editor");
    QMouseEvent tap(QEvent::MouseButtonRelease, QPointF(10, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor, &tap);
    check(visited && valid && std::abs(field->value() - value.toDouble()) < 1e-9, "Touch keypad did not validate/commit calibration number");
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace vibration; using namespace vibration::workbench;
    try {
        AnalysisConfig original; original.calibration_confirmed = true;
        CalibrationSettings widget(original); widget.resize(760, 380); widget.show(); app.processEvents();
        auto result = original; QString error;
        check(widget.applyTo(result, &error) && calibration_config_equal(result, original), "Untouched legacy settings changed conversion");
        check(result.calibration_confirmed, "Untouched settings removed explicit calibration declaration");
        find<QComboBox>(widget, "calibrationAmplitudeUnit")->setCurrentIndex(1);
        touchNumber(find<QDoubleSpinBox>(widget, "calibrationSensitivity1"), "2.00", "0");
        touchNumber(find<QDoubleSpinBox>(widget, "calibrationSensitivity2"), "4");
        touchNumber(find<QDoubleSpinBox>(widget, "calibrationSensitivity3"), "8");
        touchNumber(find<QDoubleSpinBox>(widget, "calibrationGain1"), "1");
        touchNumber(find<QDoubleSpinBox>(widget, "calibrationGain2"), "2", "0");
        touchNumber(find<QDoubleSpinBox>(widget, "calibrationGain3"), "-1");
        check(widget.applyTo(result, &error), "Valid independent settings rejected");
        check(result.channel_calibration.has_value() && result.amplitude_unit == AmplitudeUnit::AccelerationSI, "Independent settings not applied");
        check(!result.calibration_confirmed, "Edited conversion asserted actual calibration");
        check(std::abs(voltage_to_amplitude(.012, 0, result) - 6) < 1e-12, "Dialog values convert 2mV/(m/s2) incorrectly");
        find<QDoubleSpinBox>(widget, "calibrationGain2")->setValue(0);
        const auto retained = result; check(!widget.applyTo(result, &error) && !error.isEmpty(), "Zero gain accepted in dialog");
        check(calibration_config_equal(retained, result), "Rejected dialog partially changed config");
        find<QDoubleSpinBox>(widget, "calibrationGain2")->setValue(2);
        const auto object = calibration_settings_json(result); auto restored = original;
        check(apply_calibration_settings_json(object, restored, &error), "Saved independent settings rejected");
        check(calibration_config_equal(result, restored) && !restored.calibration_confirmed, "Persistence changed values or asserted calibration");
        check(!object.contains("calibration_confirmed"), "Persistence stored actual-calibration claim");
        auto malformed = object; auto channels = malformed.value("channel_calibration").toArray();
        auto channel = channels[1].toObject(); channel["analog_gain"] = 0; channels[1] = channel; malformed["channel_calibration"] = channels;
        check(!apply_calibration_settings_json(malformed, restored, &error), "Persistence accepted zero channel gain");
        check(calibration_config_equal(restored, result), "Rejected persistence partially changed config");
        channels.removeLast(); malformed["channel_calibration"] = channels;
        check(!apply_calibration_settings_json(malformed, restored, &error), "Persistence accepted two-channel conversion");
        AnalysisConfig migrated;
        check(apply_calibration_settings_json(QJsonObject{{"sensor_sensitivity_v_per_g", .7}, {"analog_gain", -2}}, migrated, &error), "Legacy JSON not accepted");
        check(!migrated.channel_calibration && migrated.sensor_sensitivity_v_per_g == .7 && migrated.analog_gain == -2, "Legacy JSON migration changed values");
        if (argc > 1) check(widget.grab().save(QString::fromUtf8(argv[1])), "Calibration screenshot save failed");
        std::cout << "PASS " << checks << " calibration settings checks; parameters remain user declarations\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
