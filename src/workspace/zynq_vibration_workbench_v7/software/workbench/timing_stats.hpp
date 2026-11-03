#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QtGlobal>
#include <algorithm>
#include <array>

namespace vibration::workbench {
// Optional diagnostic counters: no allocation/sorting in the measured path.
// Percentiles describe the last 256 observations; totals cover the lifetime
// of this instance. JSON is produced only when explicitly requested by QA.
struct TimingSamples {
    quint64 count = 0, totalNs = 0;
    qint64 maximumNs = 0;
    std::array<qint64, 256> recent{};
    void add(qint64 ns) {
        ns = std::max(qint64(0), ns); recent[count % recent.size()] = ns;
        ++count; totalNs += quint64(ns); maximumNs = std::max(maximumNs, ns);
    }
    QJsonObject json() const {
        auto ordered = recent; const auto samples = std::min<quint64>(count, ordered.size());
        std::sort(ordered.begin(), ordered.begin() + samples);
        return {{"count", double(count)}, {"total_ms", totalNs / 1e6},
            {"mean_ms", count ? double(totalNs) / count / 1e6 : 0}, {"max_ms", maximumNs / 1e6},
            {"recent_samples", double(samples)}, {"recent_p50_ms", samples ? ordered[(samples - 1) / 2] / 1e6 : 0},
            {"recent_p95_ms", samples ? ordered[(samples - 1) * 95 / 100] / 1e6 : 0}};
    }
};
class ScopedTiming {
public:
    ScopedTiming(TimingSamples& samples, bool enabled) : samples_(enabled ? &samples : nullptr) { if (samples_) timer_.start(); }
    ~ScopedTiming() { if (samples_) samples_->add(timer_.nsecsElapsed()); }
private:
    TimingSamples* samples_; QElapsedTimer timer_;
};
class TimingLap {
public:
    explicit TimingLap(bool enabled) : enabled_(enabled) { if (enabled_) timer_.start(); }
    void mark(TimingSamples& samples) { if (enabled_) { samples.add(timer_.nsecsElapsed()); timer_.restart(); } }
private:
    bool enabled_; QElapsedTimer timer_;
};
} // namespace vibration::workbench
