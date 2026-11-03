#pragma once

#include "vibration/core.hpp"
#include "vibration/detection.hpp"
#include <QWidget>
#include <memory>

namespace vibration::workbench {

// Read-only presentation of core results. It never evaluates or promotes rules.
class DetectionPane final : public QWidget {
public:
    explicit DetectionPane(QWidget* parent = nullptr);
    ~DetectionPane() override;
    void setResult(const AnalysisResult* result, const DetectionResult* detection);
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibration::workbench
