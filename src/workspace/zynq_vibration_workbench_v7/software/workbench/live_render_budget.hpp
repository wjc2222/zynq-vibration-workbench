#pragma once
#include <algorithm>
#include <array>
#include <QtGlobal>
namespace vibration::workbench {
// Presentation pacing only. Acquisition and accepted preview cadence are not
// throttled. Retain a short peak history so an unusually cheap cursor paint
// cannot immediately erase the cost of the preceding full waveform frame.
class LiveRenderBudget {
public:
    void observe(qint64 guiWorkNs,qint64 paintNs) {
        const qint64 frame=std::max(qint64(0),guiWorkNs)+std::max(qint64(0),paintNs);
        recent_[next_++%recent_.size()]=frame;
    }
    int intervalMs() const {
        const auto peak=*std::max_element(recent_.begin(),recent_.end());
        // Allow at least as much main-loop time between renders as the last
        // measured frame consumed. The 2 s ceiling also bounds UI staleness.
        return int(std::clamp<qint64>((peak+499999)/500000,40,2000));
    }
    void reset(){recent_.fill(0);next_=0;}
private:
    std::array<qint64,8> recent_{};quint64 next_=0;
};
}
