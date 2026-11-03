#pragma once
#include "vibration/core.hpp"
#include <cstddef>
#include <vector>

namespace vibration::gui {
// Display selection uses validated raw records, never the previous overview.
std::vector<WaveformPoint> waveform_preview(const Capture& capture, const AnalysisConfig& config,
                                           double start_s, double end_s, std::size_t budget = 1800);
WaveformPoint waveform_cursor(const Capture& capture, const AnalysisConfig& config, double time_s);
std::size_t nearest_frequency_bin(const Spectrum& spectrum, double frequency_hz);
} // namespace vibration::gui
