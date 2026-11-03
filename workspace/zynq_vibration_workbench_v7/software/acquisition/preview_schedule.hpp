#pragma once
#include <chrono>

namespace vibration::transport {
// Advance along the original monotonic 40ms grid. Never wait another full
// period after work, and never enqueue a catch-up burst for expired slots.
inline std::chrono::steady_clock::time_point nextPreviewSlot(
        std::chrono::steady_clock::time_point scheduled,
        std::chrono::steady_clock::time_point workFinished){
    constexpr auto period=std::chrono::milliseconds(40);
    if(workFinished<scheduled)return scheduled;
    return scheduled+period*((workFinished-scheduled)/period+1);
}
}
