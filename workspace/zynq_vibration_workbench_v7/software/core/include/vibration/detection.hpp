#pragma once

#include "vibration/core.hpp"

namespace vibration {

// This schema carries user-supplied experimental rules, never a trained or
// validated classifier. Channels are storage CH1/CH2/CH3 (ADC V1/V3/V5).
struct DetectionFeature {
    std::string feature;
    unsigned channel = 1;
    double value = 0;
};
struct DetectionCondition {
    std::string feature;
    unsigned channel = 1;
    // absolute, delta (current-baseline), or ratio (current/baseline).
    std::string comparison = "absolute";
    double minimum = 0, maximum = 0; // inclusive, finite interval
};
struct DetectionRule {
    std::string id, label; // normal, imbalance, misalignment, looseness
    std::vector<DetectionCondition> conditions; // nonempty all-of
};
struct DetectionProfile {
    std::string schema = "zynq_vibration.detection_profile.v1";
    std::string id, status = "draft", setup_id;
    std::string source_path, source_metadata_status;
    std::uint32_t source_header_crc32 = 0, source_payload_crc32 = 0;
    std::string source_kind = "unknown";
    bool source_synthetic = false, calibration_confirmed = false;
    double sensor_sensitivity_v_per_g = 0.5, analog_gain = -1;
    std::optional<std::array<ChannelCalibration, 3>> channel_calibration;
    // Rule intervals and baseline amplitudes are expressed in this unit.
    // Unit/configuration mismatches are gated before any rule can be selected.
    AmplitudeUnit amplitude_unit = AmplitudeUnit::Gravity;
    std::uint32_t sample_rate_hz = 0, channel_map = 0;
    std::int32_t adc_range_uv = 0;
    std::size_t fft_size = 65536, actual_fft_size = 0;
    double spectrum_max_hz = 2000;
    std::optional<double> pulses_per_rev, speed_rpm_min, speed_rpm_max;
    std::uint32_t min_records = 0;
    double min_speed_valid_fraction = 0, max_speed_timeout_fraction = 0;
    std::optional<double> max_speed_cv;
    std::vector<DetectionFeature> baseline;
    std::vector<DetectionRule> rules;
};
struct DetectionGate {
    std::string id;
    bool passed = false;
    std::string detail;
};
struct DetectionComparison {
    std::string feature;
    unsigned channel = 1;
    double baseline = 0;
    std::optional<double> current, delta, ratio;
    std::string status;
};
struct DetectionConditionEvidence {
    DetectionCondition condition;
    std::optional<double> current, baseline, evaluated;
    bool passed = false;
    std::string status;
};
struct DetectionRuleEvidence {
    std::string id, label;
    bool matched = false;
    std::vector<DetectionConditionEvidence> conditions;
};
struct DetectionResult {
    std::string schema = "zynq_vibration.detection.v1";
    std::string profile_id, profile_status, setup_id, source_path;
    std::string amplitude_unit = "g", baseline_amplitude_unit = "g";
    // Bounded canonical profile text captures all expected configuration,
    // quality limits, baseline observations and rules for audit/replay.
    std::string profile_snapshot;
    // quality_gated / baseline_only / candidate / conflict / inconclusive.
    // UI callers may also represent not_configured / invalid_profile / no_record.
    std::string status = "quality_gated";
    std::string classification = "not_classified";
    std::optional<std::string> candidate;
    bool validated = false; // fixed false, no profile key can promote this
    std::vector<DetectionGate> gates;
    std::vector<DetectionComparison> baseline_comparisons;
    std::vector<DetectionRuleEvidence> rules;
    std::vector<std::string> limitations;
};

// Strict bounded UTF-8 key=value format, no sections, no duplicate/unknown keys.
// File <=64KiB, <=32 rules, <=16 conditions/rule, <=30 baseline features.
// Profile objects supplied directly are validated by serialize/evaluate too.
DetectionProfile parse_detection_profile(const std::string& text);
DetectionProfile load_detection_profile(const std::string& path);
std::string detection_profile_text(const DetectionProfile& profile);
// Observed single-record snapshot only; no normal label, learned model, or rules.
DetectionProfile make_baseline_draft(const AnalysisResult& analysis,
                                     const std::string& setup_id,
                                     const std::string& profile_id = "single_record_draft");
DetectionResult evaluate_detection(const AnalysisResult& analysis,
                                   const DetectionProfile& profile,
                                   const std::string& setup_id);
std::string detection_to_json(const DetectionResult& result);
// Rejects writes through aliases/hardlinks to any protected file, including a
// missing .meta.json. Callers pass capture, its sidecar and any loaded profile.
void write_detection_text(const std::string& output_path, const std::string& text,
                          const std::vector<std::string>& protected_paths);
// Preflight the complete output set before writing any member (CLI batch).
void validate_detection_outputs(const std::vector<std::string>& output_paths,
                                const std::vector<std::string>& protected_paths);

} // namespace vibration
