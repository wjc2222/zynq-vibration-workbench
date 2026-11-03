#pragma once

#include "vibration/core.hpp"
#include <QDialog>
#include <QJsonObject>
#include <QWidget>
#include <array>
#include <functional>

class QComboBox;
class QDoubleSpinBox;

namespace vibration::workbench {

// Inline controls on the acceleration page. Front-end calibration remains
// part of the device configuration; users enter only sensor sensitivities.
class SensitivitySettings final : public QWidget {
public:
    explicit SensitivitySettings(const AnalysisConfig& config, QWidget* parent = nullptr);
    void setConfig(const AnalysisConfig& config);
    bool applyTo(AnalysisConfig& config, QString* error = nullptr) const;
    std::function<void()> applyRequested;
private:
    AnalysisConfig original_;
    std::array<QDoubleSpinBox*, 3> sensitivity_{};
    std::array<double, 3> displayed_{};
};

// Only conversion parameters and the displayed physical unit are edited here.
// Entering numbers never asserts that a physical calibration was performed.
class CalibrationSettings final : public QWidget {
public:
    explicit CalibrationSettings(const AnalysisConfig& config, QWidget* parent = nullptr);
    bool validate(QString* error = nullptr) const;
    bool applyTo(AnalysisConfig& config, QString* error = nullptr) const;
private:
    AnalysisConfig original_;
    QComboBox* unit_ = nullptr;
    std::array<QDoubleSpinBox*, 3> sensitivity_{}, gain_{}, bias_{};
    std::array<std::array<double, 3>, 3> displayed_{};
    AnalysisConfig edited() const;
};

class CalibrationSettingsDialog final : public QDialog {
public:
    explicit CalibrationSettingsDialog(const AnalysisConfig& config, QWidget* parent = nullptr);
    const AnalysisConfig& config() const { return result_; }
private:
    AnalysisConfig result_;
};

// Parameter persistence helpers. They do not serialize a claim of actual
// calibration. A successful read explicitly resets calibration_confirmed.
// Old objects containing only sensor_sensitivity_v_per_g/analog_gain are read.
QJsonObject calibration_settings_json(const AnalysisConfig& config);
bool apply_calibration_settings_json(const QJsonObject& object, AnalysisConfig& config,
                                      QString* error = nullptr);

} // namespace vibration::workbench
