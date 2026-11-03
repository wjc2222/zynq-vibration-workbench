#pragma once

#include "vibration/core.hpp"
#include <QWidget>
#include <QString>
#include <QJsonObject>
#include <functional>
#include <memory>

class QShowEvent;
class QHideEvent;

namespace vibration::workbench {

// Independent analysis surface. The owner keeps result alive until clear().
// Capture ownership is shared because range previews read the original record.
class AnalysisPane final : public QWidget {
public:
    explicit AnalysisPane(QWidget* parent = nullptr);
    ~AnalysisPane() override;
    void setResult(std::shared_ptr<const Capture> capture, const AnalysisResult* result);
    // Changes displayed amplitudes only; live transport/cache stay in ADC volts.
    void setAnalysisConfig(const AnalysisConfig& config);
    // Window's separate acceleration page converts waveform amplitudes only;
    // its spectrum stays in volts. Unmanaged panes keep configured SI/g FFTs.
    void setAccelerationMode(bool enabled);
    // Optional presentation overlay only; the measured FFT amplitudes/bins stay
    // unchanged. References require validated rotational-speed information.
    void setOrderMarkersVisible(bool visible);
    bool orderMarkersVisible() const;
    void clear();
    void setView(int index); // 0: waveform, 1: spectrum, 2: speed
    void setWindow(double start, double end);
    // beginLive validates and consumes the first frame. Later frames use
    // acceptLiveFrame; preview integrity never certifies a completed file.
    bool beginLive(const QJsonObject& firstFrame, bool pprConfirmed = false, QString* error = nullptr);
    bool acceptLiveFrame(const QJsonObject& frame, QString* error = nullptr);
    // A transport batch accepts every bounded pending raw chunk before one
    // presentation request. Nested scopes are balanced across first beginLive.
    void beginLiveBatch();
    void endLiveBatch();
    void endLive(const QString& reason = {});
    bool isLive() const;
    QJsonObject performanceProfile() const;
    std::function<void(QString)> statusChanged;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vibration::workbench
