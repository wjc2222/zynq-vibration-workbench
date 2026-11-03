#pragma once
#include "vibration/core.hpp"
#include "vibration/accelerator.hpp"

namespace vibration {
inline constexpr std::size_t supervised_feature_count = 69;
inline constexpr const char* supervised_feature_schema = "rpmfree69.v1";
using SupervisedFeatureVector = std::array<double, supervised_feature_count>;
struct SupervisedWindow {
    std::string feature_schema = supervised_feature_schema;
    std::string configuration, setup_id, source, source_path;
    std::string capture_sha256, source_session_id, raw_window_sha256;
    std::string input_kind = "committed_bin";
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t first_index = 0, last_index = 0, analysis_first_index = 0, analysis_last_index = 0;
    bool valid = false, synthetic = false;
    double elapsed_ms = 0;
    SupervisedFeatureVector features{};
    std::array<double,3> ac_rms_adc{};
    std::vector<std::string> reasons;
    std::vector<std::string> warnings;
};
// Fixed schema order: 21 features per channel: 3 time, 9 spectrum, 9 wavelet;
// then 3 axis AC-energy fractions and correlations CH1/2, CH1/3, CH2/3.
// Wavelet packets: periodized orthogonal db4, depth 3, natural paths AAA..DDD.
// No speed, PPR, shaft order, operator label or class information is read.
const std::array<std::string,supervised_feature_count>& supervised_feature_names();
SupervisedFeatureVector supervised_features_from_accelerator(const accelerator::Result&,
    std::array<double,3>* ac_rms_adc = nullptr);
// Reuses the existing PL FIR/FFT interface. Hardware errors never fall back.
// Missing/unstable tachometer data cannot invalidate this RPM-free window.
// A quiet single axis is retained; an entirely flat three-axis window cannot
// establish usable vibration data. Synthetic/corrupt/incomplete inputs cannot
// become real-machine classification evidence.
SupervisedWindow extract_supervised_window(const Capture&, std::size_t start,
    const AnalysisConfig&, const std::string& setup_id,
    accelerator::Backend = accelerator::Backend::Reference);
std::string supervised_window_json(const SupervisedWindow&);
} // namespace vibration
