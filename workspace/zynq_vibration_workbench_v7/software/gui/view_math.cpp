#include "view_math.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace vibration::gui {
namespace {
void validate(const Capture& c, const AnalysisConfig& cfg) {
    validate_calibration_config(cfg);
    if (!c.integrity_verified || c.records.empty() || !c.header.sample_rate_hz ||
        c.header.adc_range_uv <= 0)
        throw AnalysisError("Invalid capture or conversion for waveform preview");
}
WaveformPoint point(const Capture& c, std::size_t i, const AnalysisConfig& cfg) {
    return {double(i) / c.header.sample_rate_hz,
            {adc_to_amplitude(c.records[i].raw[0], c.header.adc_range_uv, 0, cfg),
             adc_to_amplitude(c.records[i].raw[1], c.header.adc_range_uv, 1, cfg),
             adc_to_amplitude(c.records[i].raw[2], c.header.adc_range_uv, 2, cfg)}};
}
}
std::vector<WaveformPoint> waveform_preview(const Capture& c, const AnalysisConfig& cfg,
                                           double start, double end, std::size_t budget) {
    validate(c, cfg);
    if (!std::isfinite(start) || !std::isfinite(end) || end < start || budget < 12)
        throw AnalysisError("Invalid waveform range or point budget");
    const double fs = c.header.sample_rate_hz;
    const double last = double(c.records.size() - 1) / fs;
    start = std::clamp(start, 0.0, last); end = std::clamp(end, 0.0, last);
    const auto first = std::min(c.records.size() - 1, std::size_t(std::ceil(start * fs - 1e-8)));
    const auto final = std::min(c.records.size() - 1, std::size_t(std::floor(end * fs + 1e-8)));
    if (first > final) return {};
    const auto count = final - first + 1;
    std::set<std::size_t> indices{first, final};
    if (count <= budget) {
        for (auto i = first; i <= final; ++i) indices.insert(i);
    } else {
        const auto buckets = (budget - 2) / 6;
        for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
            const auto begin = first + count * bucket / buckets;
            const auto finish = first + count * (bucket + 1) / buckets;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                auto lo = begin, hi = begin;
                for (auto i = begin + 1; i < finish; ++i) {
                    if (c.records[i].raw[axis] < c.records[lo].raw[axis]) lo = i;
                    if (c.records[i].raw[axis] > c.records[hi].raw[axis]) hi = i;
                }
                indices.insert(lo); indices.insert(hi);
            }
        }
    }
    std::vector<WaveformPoint> output;
    output.reserve(indices.size());
    for (auto i : indices) output.push_back(point(c, i, cfg));
    return output;
}
WaveformPoint waveform_cursor(const Capture& c, const AnalysisConfig& cfg, double t) {
    validate(c, cfg);
    if (!std::isfinite(t)) throw AnalysisError("Invalid cursor time");
    t = std::clamp(t, 0.0, double(c.records.size() - 1) / c.header.sample_rate_hz);
    return point(c, std::min(c.records.size() - 1, std::size_t(std::llround(t * c.header.sample_rate_hz))), cfg);
}
std::size_t nearest_frequency_bin(const Spectrum& s, double hz) {
    if (s.frequency_hz.empty() || !std::isfinite(hz)) throw AnalysisError("Spectrum cursor is unavailable");
    auto it = std::lower_bound(s.frequency_hz.begin(), s.frequency_hz.end(), hz);
    if (it == s.frequency_hz.begin()) return 0;
    if (it == s.frequency_hz.end()) return s.frequency_hz.size() - 1;
    const auto i = std::size_t(it - s.frequency_hz.begin());
    return hz - s.frequency_hz[i - 1] <= s.frequency_hz[i] - hz ? i - 1 : i;
}
} // namespace vibration::gui
