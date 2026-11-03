#pragma once
#include "vibration/reference_library.hpp"
#include <map>

namespace vibration {
inline constexpr unsigned reference_validation_policy_count = 3;
// No windows are independent trials. Each held-out file contributes total
// weight 1 (1 / its window count per prediction), including unknown outcomes.
// RPM-bin metrics divide by the file's window count within that bin instead.
struct ReferenceValidationMetrics {
    bool available = false, accepted_accuracy_available = false;
    std::size_t files = 0, windows = 0;
    // Rows: normal, imbalance, misalignment, looseness. Columns: same + unknown.
    std::array<std::array<std::uint64_t, 5>, 4> confusion_window_counts{};
    std::array<std::array<double, 5>, 4> confusion_file_weights{};
    std::array<double, 4> precision{}, recall{}, f1{};
    double accuracy = 0, acceptance_coverage = 0, accepted_accuracy = 0, macro_f1 = 0;
};
struct ReferenceValidationRpmBin {
    std::string name;
    double lower_rpm = 0, upper_rpm = 0;
    bool upper_unbounded = false, spans_bins = false;
    ReferenceValidationMetrics metrics;
    std::map<std::string, std::size_t> status_counts, rejection_reason_counts;
};
struct ReferenceValidationInput {
    std::string path, label, capture_sha256, session_id, operator_updated_at;
    // Legacy capture formats may get the session from a saved annotation.
    // That is a held-out grouping assertion, never proof of a new remount.
    std::string session_identity_source;
    std::string sample_input_sha256;
    std::vector<std::string> raw_window_sha256;
};
struct ReferenceValidationPrediction {
    std::string raw_window_sha256, classification, status;
    std::uint64_t first_index = 0, last_index = 0;
    double rpm_min = 0, rpm_max = 0, file_weight = 0;
    std::array<unsigned, 4> reference_eligible_files{};
    std::string speed_support_status;
    double reference_speed_distance = 0, speed_support_factor = 0;
    std::string shape_support_status;
    double reference_second_distance = 0;
    std::vector<std::string> reasons;
};
struct ReferenceValidationFoldModel {
    std::string policy, model_kind, model_sha256, reference_provenance_sha256;
    std::vector<ReferenceValidationPrediction> predictions;
};
struct ReferenceValidationFold {
    std::string heldout_capture_sha256, heldout_session_id, heldout_input_sha256;
    std::vector<std::string> train_capture_sha256, train_session_id;
    std::string train_inputs_sha256;
    std::array<ReferenceValidationFoldModel, reference_validation_policy_count> models;
};
struct ReferenceValidationAlgorithm {
    std::string policy, model_kind, full_model_sha256, full_reference_provenance_sha256;
    ReferenceValidationMetrics metrics;
    std::vector<ReferenceValidationRpmBin> rpm_bins;
    std::map<std::string, std::size_t> status_counts, rejection_reason_counts;
    // Count actual inference reasons, preserving RPM-local failures as unknown.
    std::map<std::string, std::size_t> rpm_support_reason_counts;
    std::size_t rpm_support_missing_windows = 0;
};
struct ReferenceValidationReport {
    std::string schema = "zynq_vibration.reference_validation.v2";
    std::string status = "insufficient_independent_files";
    std::string scope = "capture_session_heldout_not_independent_remount";
    std::string input_sha256, setup_id, configuration;
    std::uint32_t sample_rate_hz = 0;
    bool validated = false, metrics_available = false;
    std::array<std::size_t, 4> independent_files_per_class{};
    std::vector<std::string> reasons;
    std::vector<ReferenceValidationInput> inputs;
    std::array<ReferenceValidationAlgorithm, reference_validation_policy_count> algorithms;
    std::vector<ReferenceValidationFold> folds;
};
// Fixed policies/default thresholds and fixed RPM bins; no threshold search,
// model selection or nested tuning. Invalid/synthetic/duplicate/config-mixed
// libraries throw AnalysisError before evaluation. Limits: <=256 files and
// <=32 windows/file. Missing session identity or <3 files in any class returns
// insufficient_independent_files with no metrics. Full-library fits validate
// the inputs and bind provenance only; they are never used for a fold query.
ReferenceValidationReport evaluate_reference_models(const std::vector<ReferenceSample>&);
std::string reference_validation_json(const ReferenceValidationReport&);
} // namespace vibration
