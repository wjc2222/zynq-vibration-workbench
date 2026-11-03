#pragma once
#include "plot_canvas.hpp"
#include "vibration/core.hpp"
#include <QJsonObject>
#include <QString>
#include <array>
#include <memory>
#include <optional>

namespace vibration::workbench {
struct LivePreviewView {
    QVector<PlotSeries> voltage;
    PlotSeries speed;
    double startTime = 0, endTime = 0, validSpeedFraction = 0;
    std::optional<double> meanSpeed, minimumSpeed, maximumSpeed;
    bool rpm = false;
    quint64 revision = 0;
};
struct LivePreviewInfo {
    QString jobId, generation, source, error;
    bool active = false, quarantined = false, synthetic = false, hardwareConfirmed = false, pprConfirmed = false;
    quint32 sampleRate = 0, clockHz = 0, ppr = 0;
    qint32 adcRangeUv = 0;
    quint64 acceptedFrames = 0, rejectedFrames = 0, acceptedRecords = 0, previewDroppedRecords = 0;
    quint64 revision = 0, fftSubmitted = 0, fftCompleted = 0;
    std::size_t retainedRecords = 0, maximumRecords = 0;
};
struct LivePreviewSpectrum {
    std::array<Spectrum, 3> channels;
    double startTime = 0, endTime = 0;
    quint64 revision = 0, inputRevision = 0;
    bool available = false;
    QString error;
};

// GUI-thread model of the preview-contract-v1 committed raw stream. It makes
// no complete-CAPDDR2, calibration, physical-source or fault-classifier claim.
// A single bounded asynchronous worker computes voltage-only spectra at <=5Hz.
class LivePreviewModel final {
public:
    LivePreviewModel();
    ~LivePreviewModel();
    bool begin(const QJsonObject& firstFrame, bool pprConfirmed = false, QString* error = nullptr);
    bool accept(const QJsonObject& frame, QString* error = nullptr);
    void end(const QString& reason = {});
    void reset();
    bool isLive() const;
    LivePreviewInfo info() const;
    LivePreviewView view(double windowSeconds = .25, bool voltage = true, bool speed = true) const;
    // Exact opt-in AX7_LIVE_DEMAND_ANALYSIS=1 only: hidden/paused consumers
    // disable new FFT work. A running task is reaped without waiting and its
    // old demand generation cannot publish after demand resumes. begin/reset
    // close demand; the consumer re-enables it after accepting the first frame.
    void setSpectrumDemand(bool wanted);
    void pollSpectrum();
    LivePreviewSpectrum spectrum() const;
    QJsonObject performanceProfile() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace vibration::workbench
