#pragma once
#include "vibration/core.hpp"
#include "vibration/accelerator.hpp"
#include <array>

namespace vibration {
constexpr std::size_t candidate_feature_count = 36;
// Fixed engineering resolution margin, not a measured instrument noise floor,
// SNR calibration, or a guaranteed bound on spectral-ratio error. Changing
// either value requires a new policy identity and a new compatible baseline.
inline constexpr const char* candidate_quality_policy = "reference_adc_rms64_v1";
inline constexpr double candidate_minimum_reference_rms_adc = 64.0;
struct CandidateWindow {
    std::string feature_schema = "lowband36.v1", configuration, source, setup_id;
    std::string input_kind = "committed_bin", source_path;
    std::string raw_window_sha256, capture_sha256, source_session_id;
    std::uint32_t sample_rate_hz = 0;
    std::uint64_t first_index = 0, last_index = 0;
    std::uint64_t analysis_first_index = 0, analysis_last_index = 0;
    double rpm = 0, rpm_min = 0, rpm_max = 0, speed_cv = 0, elapsed_ms = 0;
    unsigned fresh_periods = 0;
    bool valid = false, synthetic = false;
    std::array<double, candidate_feature_count> features{};
    std::vector<std::string> reasons;
    std::string quality_policy = candidate_quality_policy;
    std::string noise_floor_status = "not_characterized";
    std::array<double, 3> reference_rms_adc{};
    std::array<bool, 3> ratio_features_valid{};
};
inline constexpr std::size_t candidate_model_max_bytes=16*1024*1024;
inline constexpr std::size_t candidate_reference_max_exemplars=8192;
// V2 compatibility policy, not a calibrated classifier or noise estimate.
// Within-file groups span at most 4% above their lowest RPM and use coordinate
// medians. Query support must lie completely within each reference's +/-10%.
// One representative per file is chosen using RPM only. Each class has weight
// 1/4 and each eligible file has equal weight within its class for local IQR
// scaling. Quantiles select the smallest sorted value with cumulative weight
// at least the requested fraction (ties retain that value). Scale is max of
// IQR/1.349, 1% weighted median absolute feature, and the fixed unit floor.
// The class distance is RMS of the two closest distinct-file distances
// (36-feature RMS plus normalized RPM displacement). No query feature fits scale.
// Fixed two-file support suppresses singleton matches; a class with more files
// still has more opportunities for a close pair and is not bias-free.
inline constexpr const char* reference_ensemble_policy =
    "file_median_rpm_nearest_balanced_iqr_top2.v2";
// V3 engineering defaults are fixed policy choices, not fitted parameters,
// calibrated probabilities or evidence of cross-speed machine accuracy.
// A measured order-ratio/crest/kurtosis shape is primary; amplitude contributes
// a log-ratio distance (0.25 per doubling), with no assumed RPM power law.
// RPM selects the nearest representative per file and adds only a smooth,
// bounded penalty. All independent files remain eligible at every valid RPM.
inline constexpr const char* reference_speed_tolerant_policy =
    "file_median_rpm_nearest_shape_logamp_softspeed_modes.v3";
inline constexpr double reference_speed_amplitude_weight = .25;
inline constexpr double reference_speed_maximum_penalty = .35;
inline constexpr double reference_speed_transition_ratio = 1.25;
inline constexpr unsigned reference_ensemble_minimum_files_per_class = 2;
inline constexpr double reference_ensemble_speed_group_ratio = 1.04;
inline constexpr double reference_ensemble_speed_tolerance = .10;
inline constexpr double reference_ensemble_iqr_normalizer = 1.349;
inline constexpr double reference_ensemble_relative_scale_floor = .01;
inline constexpr double reference_ensemble_amplitude_scale_floor = 1e-9;
inline constexpr double reference_ensemble_shape_scale_floor = .01;
struct CandidateReferenceExemplar {
    unsigned class_index=0;
    double rpm_min=0,rpm_max=0;
    std::string capture_sha256,session_id;
    std::array<double,candidate_feature_count> features{};
};
struct CandidateModel {
    std::string schema = "zynq_vibration.candidate_model.v1";
    std::string id = "normal_baseline", setup_id, status = "provisional";
    std::string kind = "engineering_prior", configuration;
    std::uint32_t fs = 0;
    unsigned baseline_count = 0;
    double rpm_min = 0, rpm_max = 0;
    double minimum_score = 55, minimum_margin = 12, maximum_distance = 25;
    std::array<double, candidate_feature_count> mean{}, scale{};
    // Row-major normal, imbalance, misalignment, looseness. Linear scores
    // are discriminants; no calibrated-probability or accuracy claim is made.
    std::array<double, 4> bias{};
    std::array<double, 4 * candidate_feature_count> weights{};
    // Reference models count source files, never windows as independent trials.
    std::array<double, 4> reference_file_counts{};
    std::string reference_provenance_sha256;
    // Mandatory for V2/V3 ensembles; absent for older kinds.
    std::string reference_policy;
    // Empty for a new V3 fit. An explicit V2->V3 upgrade records the SHA256 of
    // the canonical V2 model bytes and binds it into the new provenance.
    std::string reference_parent_model_sha256;
    std::vector<CandidateReferenceExemplar> references;
};
struct CandidateInference {
    std::string schema = "zynq_vibration.candidate_result.v1";
    std::string model_id, model_kind, status = "no_model", classification = "unknown";
    std::string source, configuration;
    bool validated = false;
    bool reference_replay = false;
    std::array<unsigned,4> reference_eligible_files{};
    // V2/V3 reference-only scale audit. V3 amplitude entries are the positive
    // reference floor for its log ratios, not a linear standard deviation.
    std::array<double, candidate_feature_count> local_feature_scale{};
    // V3 audit only. Factor is exp(-absolute log RPM displacement of the
    // winning nearest file); engineering speed similarity, never a probability.
    std::string speed_support_status;
    double reference_speed_distance = 0, speed_support_factor = 0;
    // Classes may contain different damage modes/severities without a severity
    // label. The second nearest file corroborates a mode but cannot veto it.
    // A single-mode match is explicitly provisional and may be an outlier.
    std::string shape_support_status;
    double reference_second_distance = 0;
    double distance = 0, margin = 0;
    std::array<double, 4> scores{};
    std::vector<std::string> evidence, reasons;
};
// Exactly one 8192-point / 5 kHz window, including the complete FIR history.
// start is a raw-record offset; neither display thinning nor sparse resampling
// is used. Hardware errors propagate; choosing Reference is explicit.
CandidateWindow extract_candidate_window(const Capture&, std::size_t start,
    const AnalysisConfig&, const std::string& setup_id,
    accelerator::Backend = accelerator::Backend::Reference);
CandidateModel fit_normal_baseline(const std::vector<CandidateWindow>&,
    const std::string& id = "normal_baseline");
CandidateInference infer_candidate(const CandidateWindow&, const CandidateModel&);
// V3 alone ignores the nine unused acceleration conversion values in a
// strictly recognized current Voltage configuration. All other identity bytes
// remain exact; unknown/invalid encodings and acceleration units stay exact.
std::string canonical_candidate_configuration_for_v3(const std::string&);
bool candidate_configurations_compatible_for_v3(const std::string&, const std::string&);
// Reuses existing V2 aggregated features exactly. No raw extraction, training,
// new acquisition or independent validation is implied by this policy upgrade.
CandidateModel upgrade_reference_model_to_speed_tolerant(const CandidateModel&);
CandidateModel parse_candidate_model(const std::string&);
CandidateModel load_candidate_model(const std::string&);
std::string candidate_model_text(const CandidateModel&);
std::string candidate_window_json(const CandidateWindow&);
std::string candidate_inference_json(const CandidateInference&);
} // namespace vibration
