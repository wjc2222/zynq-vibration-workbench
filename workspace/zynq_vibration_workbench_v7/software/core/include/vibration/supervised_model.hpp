#pragma once
#include "vibration/supervised_features.hpp"

namespace vibration {
struct SupervisedSample {
    std::string path, label, capture_sha256, session_id, operator_updated_at;
    std::vector<SupervisedWindow> windows;
    unsigned excluded_invalid_windows = 0;
    std::vector<std::string> excluded_reasons;
};
enum class SupervisedAlgorithm { LinearSoftmax, RandomForest };
enum class SupervisedFeatureSet { All69, TimeSpectrum42 };
struct SupervisedTrainOptions {
    SupervisedAlgorithm algorithm = SupervisedAlgorithm::RandomForest;
    SupervisedFeatureSet feature_set = SupervisedFeatureSet::TimeSpectrum42;
    double l2 = .1;
    unsigned iterations = 800;
    double learning_rate = .15;
    bool class_balance = false;
    unsigned seed = 20261102, trees = 32, maximum_depth = 3, minimum_leaf_windows = 2;
};
struct SupervisedTreeNode {
    int feature = -1, left = -1, right = -1;
    double threshold = 0;
    std::array<double,4> votes{};
};
struct SupervisedTree { std::vector<SupervisedTreeNode> nodes; };
struct SupervisedModel {
    std::string schema = "zynq_vibration.supervised_model.v1";
    std::string id = "four_class_supervised", kind = "supervised_random_forest_v1";
    std::string feature_schema = supervised_feature_schema, configuration, setup_id;
    std::string status = "trained_provisional", training_provenance_sha256;
    std::uint32_t sample_rate_hz = 0;
    unsigned training_windows = 0;
    unsigned training_warning_windows = 0;
    std::array<unsigned,4> training_file_counts{}, independent_session_counts{};
    SupervisedTrainOptions options;
    SupervisedFeatureVector mean{}, scale{};
    std::array<double,4> bias{};
    std::array<double,4*supervised_feature_count> weights{};
    std::vector<SupervisedTree> forest;
    std::vector<std::string> training_capture_sha256, training_session_ids;
};
struct SupervisedInference {
    std::string schema = "zynq_vibration.supervised_result.v1";
    std::string model_id, model_kind, status = "no_model", classification = "unknown";
    bool validated = false, training_replay = false;
    std::array<double,4> scores{};
    double margin = 0;
    std::vector<std::string> evidence, reasons;
};
struct SupervisedValidationReport {
    std::string schema = "zynq_vibration.supervised_validation.v1";
    std::string status = "insufficient_independent_sessions";
    bool metrics_available = false, validated = false;
    std::string scope = "whole_session_heldout_same_rig_internal_validation";
    std::string input_sha256, deployment_model_sha256;
    unsigned heldout_sessions = 0, windows = 0;
    std::array<std::array<double,4>,4> confusion_session_weights{};
    std::array<double,4> precision{}, recall{}, f1{};
    double accuracy = 0, macro_f1 = 0;
    std::vector<std::string> reasons;
    // Auditable per-fold JSON includes exact train/heldout source identities,
    // model hash and heldout predictions; no windows are randomly split.
    std::vector<std::string> fold_json;
};
std::vector<unsigned> supervised_active_feature_indices(const SupervisedTrainOptions&);
SupervisedSample extract_supervised_sample(const Capture&, const std::string& label,
    const AnalysisConfig&, const std::string& setup_id,
    accelerator::Backend = accelerator::Backend::Reference);
SupervisedModel fit_supervised_model(const std::vector<SupervisedSample>&,
    const std::string& id = "four_class_supervised", const SupervisedTrainOptions& = {});
SupervisedInference infer_supervised(const SupervisedWindow&, const SupervisedModel&);
SupervisedValidationReport evaluate_supervised_models(const std::vector<SupervisedSample>&,
    const SupervisedTrainOptions& = {});
std::string supervised_model_text(const SupervisedModel&);
SupervisedModel parse_supervised_model(const std::string&);
SupervisedModel load_supervised_model(const std::string&);
std::string supervised_inference_json(const SupervisedInference&);
std::string supervised_validation_json(const SupervisedValidationReport&);
} // namespace vibration
