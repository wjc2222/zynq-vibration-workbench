#include "vibration/reference_validation.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

namespace vibration {
namespace {
constexpr const char* labels[]{"normal", "imbalance", "misalignment", "looseness", "unknown"};
constexpr ReferenceFitPolicy policies[]{ReferenceFitPolicy::LegacyNearest, ReferenceFitPolicy::RobustEnsemble, ReferenceFitPolicy::SpeedTolerantEnsemble};
constexpr const char* policy_names[]{"LegacyNearest", "RobustEnsemble", "SpeedTolerantEnsemble"};
void require(bool value, const std::string& message) {
    if (!value) throw AnalysisError("Reference validation: " + message);
}
std::string hash(const std::string& value) {
    return sha256_hex(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}
// Length framing prevents delimiters in paths/session/operator strings from
// making different inputs share the same provenance material.
void frame(std::string& material, const std::string& value) {
    material += std::to_string(value.size()) + ":" + value;
}
std::string digest_inputs(const std::vector<ReferenceValidationInput>& inputs) {
    std::string material = "reference_validation_inputs_v1:";
    for (const auto& input : inputs) frame(material, input.sample_input_sha256);
    return hash(material);
}
std::string sample_digest(const ReferenceSample& sample) {
    std::string material = "reference_validation_sample_v1:";
    for (const auto* value : {&sample.path, &sample.label, &sample.capture_sha256,
                              &sample.session_id, &sample.operator_updated_at}) frame(material, *value);
    for (const auto& window : sample.windows) {
        frame(material, window.feature_schema);
        frame(material, candidate_window_json(window));
    }
    return hash(material);
}
unsigned prediction_index(const std::string& classification) {
    for (unsigned c = 0; c < 4; ++c) if (classification == labels[c]) return c;
    require(classification == "unknown", "inference returned an unrecognized class");
    return 4;
}
std::vector<ReferenceValidationRpmBin> rpm_bins() {
    std::vector<ReferenceValidationRpmBin> bins;
    const double edges[]{0, 600, 1200, 1800, 3000};
    const char* names[]{"[0,600)", "[600,1200)", "[1200,1800)", "[1800,3000)", "[3000,+inf)"};
    for (unsigned i = 0; i < 5; ++i) {
        ReferenceValidationRpmBin bin;
        bin.name = names[i]; bin.lower_rpm = edges[i]; bin.upper_unbounded = i == 4;
        if (i != 4) bin.upper_rpm = edges[i + 1];
        bins.push_back(bin);
    }
    ReferenceValidationRpmBin crossing; crossing.name = "spans_fixed_rpm_bins"; crossing.spans_bins = true;
    bins.push_back(crossing);
    return bins;
}
std::size_t rpm_bin_index(const CandidateWindow& window, const std::vector<ReferenceValidationRpmBin>& bins) {
    for (std::size_t i = 0; i + 1 < bins.size(); ++i)
        if (window.rpm_min >= bins[i].lower_rpm && (bins[i].upper_unbounded || window.rpm_max < bins[i].upper_rpm)) return i;
    return bins.size() - 1;
}
void record(ReferenceValidationMetrics& metrics, unsigned truth, unsigned prediction, double weight) {
    ++metrics.windows; ++metrics.confusion_window_counts[truth][prediction];
    metrics.confusion_file_weights[truth][prediction] += weight;
}
void finish(ReferenceValidationMetrics& metrics) {
    double total = 0, accepted = 0, correct = 0;
    for (unsigned c = 0; c < 4; ++c) {
        double truth = 0, predicted = 0;
        for (unsigned j = 0; j < 5; ++j) {
            const double n = metrics.confusion_file_weights[c][j];
            truth += n; total += n; if (j < 4) accepted += n;
        }
        for (unsigned j = 0; j < 4; ++j) predicted += metrics.confusion_file_weights[j][c];
        const double tp = metrics.confusion_file_weights[c][c]; correct += tp;
        metrics.precision[c] = predicted > 0 ? tp / predicted : 0;
        metrics.recall[c] = truth > 0 ? tp / truth : 0;
        metrics.f1[c] = truth + predicted > 0 ? 2 * tp / (truth + predicted) : 0;
    }
    metrics.available = total > 0;
    if (total > 0) { metrics.accuracy = correct / total; metrics.acceptance_coverage = accepted / total; }
    metrics.accepted_accuracy_available = accepted > 0;
    if (accepted > 0) metrics.accepted_accuracy = correct / accepted;
    metrics.macro_f1 = 0;
    for (double value : metrics.f1) metrics.macro_f1 += value / 4;
}
bool rpm_rejection(const CandidateInference& prediction) {
    return prediction.status == "coverage_insufficient" || prediction.status == "reference_support_insufficient" ||
           prediction.status == "outside_operating_range";
}
// JSON strings are byte-preserving for UTF-8 and escape every control byte.
std::string quote(const std::string& value) {
    std::ostringstream out; out << '"';
    constexpr const char* hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << char(c);
    }
    out << '"'; return out.str();
}
template<class Container> void numbers(std::ostream& out, const Container& values) {
    out << '['; bool first = true;
    for (const auto& value : values) { if (!first) out << ','; first = false; out << value; }
    out << ']';
}
void strings(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << quote(values[i]); }
    out << ']';
}
void counts(std::ostream& out, const std::map<std::string, std::size_t>& values) {
    out << '{'; bool first = true;
    for (const auto& pair : values) { if (!first) out << ','; first = false; out << quote(pair.first) << ':' << pair.second; }
    out << '}';
}
void write_metrics(std::ostream& out, const ReferenceValidationMetrics& m) {
    if (!m.available) { out << "null"; return; }
    out << "{\"files\":" << m.files << ",\"windows\":" << m.windows
        << ",\"weighting\":\"one_per_file_divided_across_its_windows_in_this_metric_including_unknown\""
        << ",\"confusion_window_counts\":[";
    for (unsigned c = 0; c < 4; ++c) { if (c) out << ','; numbers(out, m.confusion_window_counts[c]); }
    out << "],\"confusion_file_weights\":[";
    for (unsigned c = 0; c < 4; ++c) { if (c) out << ','; numbers(out, m.confusion_file_weights[c]); }
    out << "],\"accuracy_including_unknown\":" << m.accuracy << ",\"acceptance_coverage\":" << m.acceptance_coverage
        << ",\"accepted_accuracy\":";
    if (m.accepted_accuracy_available) out << m.accepted_accuracy; else out << "null";
    out << ",\"macro_f1\":" << m.macro_f1 << ",\"precision\":"; numbers(out, m.precision);
    out << ",\"recall_including_unknown\":"; numbers(out, m.recall);
    out << ",\"f1\":"; numbers(out, m.f1); out << '}';
}
} // namespace

ReferenceValidationReport evaluate_reference_models(const std::vector<ReferenceSample>& supplied) {
    require(supplied.size() <= reference_max_files, "maximum 256 files");
    for (const auto& sample : supplied)
        require(sample.windows.size() <= reference_max_windows_per_file, "maximum 32 windows per file");
    ReferenceValidationReport report;
    for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
        report.algorithms[a].policy = policy_names[a];
        report.algorithms[a].model_kind = a == 0 ? "reference_exemplars" : a == 1 ? "reference_ensemble_v2" : "reference_ensemble_v3";
        report.algorithms[a].rpm_bins = rpm_bins();
    }
    // Canonical ordering makes the report independent of directory enumeration.
    auto samples = supplied;
    std::sort(samples.begin(), samples.end(), [](const auto& a, const auto& b) { return a.capture_sha256 < b.capture_sha256; });
    std::set<std::string> hashes, paths, sessions, raw_hashes;
    std::array<std::size_t, 4> declared_files_per_class{};
    bool session_missing = false;
    for (const auto& sample : samples) {
        validate_reference_sample(sample);
        require(sample.windows.size() <= reference_max_windows_per_file, "maximum 32 windows per file");
        require(hashes.insert(sample.capture_sha256).second && paths.insert(sample.path).second, "duplicate capture hash or path");
        if (sample.session_id.empty()) session_missing = true;
        else require(sessions.insert(sample.session_id).second, "capture segments from the same session are not independent files");
        const auto category = reference_label_index(sample.label);
        ++declared_files_per_class[category];
        if (!sample.session_id.empty()) ++report.independent_files_per_class[category];
        const auto& first = sample.windows.front();
        require(!first.setup_id.empty() && first.setup_id.size() <= 128 && first.setup_id.find_first_of("\r\n=") == std::string::npos,
                "invalid setup identity");
        require(first.configuration.size() >= 8 && first.configuration.size() <= 4096 &&
                first.configuration.find_first_not_of("0123456789abcdef") == std::string::npos, "invalid configuration identity");
        require(first.sample_rate_hz == 5000 || first.sample_rate_hz == 10000 || first.sample_rate_hz == 25000 || first.sample_rate_hz == 50000,
                "unsupported sample rate");
        require(sample.session_id.size() <= 128, "session identity exceeds model limit");
        if (report.inputs.empty()) { report.setup_id = first.setup_id; report.configuration = first.configuration; report.sample_rate_hz = first.sample_rate_hz; }
        require(first.setup_id == report.setup_id && first.configuration == report.configuration && first.sample_rate_hz == report.sample_rate_hz,
                "mixed setup, configuration or sample rate");
        ReferenceValidationInput input;
        input.path = sample.path; input.label = sample.label; input.capture_sha256 = sample.capture_sha256;
        input.session_id = sample.session_id; input.operator_updated_at = sample.operator_updated_at;
        input.session_identity_source = "window_source_session";
        for (const auto& window : sample.windows) {
            require(window.source_session_id.empty() || window.source_session_id == sample.session_id, "window/session identity mismatch");
            if (window.source_session_id.empty()) input.session_identity_source = "sample_annotation_window_session_absent";
            require(window.source_path == sample.path, "window/file path mismatch");
            require(raw_hashes.insert(window.raw_window_sha256).second, "duplicate raw window identity across library");
            require(window.feature_schema == "lowband36.v1" && window.quality_policy == candidate_quality_policy,
                    "unsupported feature or quality policy");
            require(std::isfinite(window.rpm) && std::isfinite(window.rpm_min) && std::isfinite(window.rpm_max) &&
                    window.rpm >= window.rpm_min && window.rpm <= window.rpm_max && window.rpm_max < 1e100, "invalid finite RPM interval");
            for (double feature : window.features) require(feature >= 0 && feature < 1e100, "feature outside model validation bounds");
            input.raw_window_sha256.push_back(window.raw_window_sha256);
        }
        input.sample_input_sha256 = sample_digest(sample);
        report.inputs.push_back(std::move(input));
    }
    report.input_sha256 = digest_inputs(report.inputs);
    bool all_classes = true, enough = true;
    for (unsigned c = 0; c < 4; ++c) {
        all_classes = all_classes && declared_files_per_class[c] > 0;
        enough = enough && report.independent_files_per_class[c] >= 3;
    }
    // These calls perform full-library fit/model validation before any fold.
    // Their parameters/scales never enter a fold; only hashes are retained.
    if (all_classes) for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
        const auto full = fit_reference_model(samples, "reference_validation_full_library", policies[a]);
        report.algorithms[a].full_model_sha256 = hash(candidate_model_text(full));
        report.algorithms[a].full_reference_provenance_sha256 = full.reference_provenance_sha256;
    }
    if (!all_classes) report.reasons.push_back("full_library_fit_unavailable_missing_confirmed_class");
    if (!enough) report.reasons.push_back("each_class_requires_at_least_3_independent_files_2_train_1_heldout");
    if (session_missing) report.reasons.push_back("missing_session_identity_cannot_establish_independent_capture_groups");
    if (!enough || session_missing) return report;

    for (std::size_t held = 0; held < samples.size(); ++held) {
        const auto& query = samples[held];
        ReferenceValidationFold fold;
        fold.heldout_capture_sha256 = query.capture_sha256; fold.heldout_session_id = query.session_id;
        fold.heldout_input_sha256 = report.inputs[held].sample_input_sha256;
        std::vector<ReferenceSample> train; std::vector<ReferenceValidationInput> train_inputs;
        for (std::size_t i = 0; i < samples.size(); ++i) if (i != held) {
            require(samples[i].capture_sha256 != query.capture_sha256 && samples[i].session_id != query.session_id,
                    "held-out capture/session leaked into training");
            train.push_back(samples[i]); train_inputs.push_back(report.inputs[i]);
            fold.train_capture_sha256.push_back(samples[i].capture_sha256); fold.train_session_id.push_back(samples[i].session_id);
        }
        fold.train_inputs_sha256 = digest_inputs(train_inputs);
        for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
            // The only fit material is train: never the query, never its label.
            const auto model = fit_reference_model(train, "reference_validation_fold", policies[a]);
            for (const auto& reference : model.references)
                require(reference.capture_sha256 != query.capture_sha256 && reference.session_id != query.session_id,
                        "fitted model contains the held-out capture/session");
            auto& fm = fold.models[a]; auto& algorithm = report.algorithms[a];
            fm.policy = policy_names[a]; fm.model_kind = model.kind;
            fm.model_sha256 = hash(candidate_model_text(model)); fm.reference_provenance_sha256 = model.reference_provenance_sha256;
            ++algorithm.metrics.files;
            std::vector<std::size_t> windows_per_bin(algorithm.rpm_bins.size());
            for (const auto& window : query.windows) ++windows_per_bin[rpm_bin_index(window, algorithm.rpm_bins)];
            for (const auto& window : query.windows) {
                const auto prediction = infer_candidate(window, model);
                require(!prediction.reference_replay, "held-out prediction incorrectly marked reference replay");
                const auto truth = reference_label_index(query.label); // scoring only, after inference
                const auto predicted = prediction_index(prediction.classification);
                const double weight = 1.0 / query.windows.size();
                record(algorithm.metrics, truth, predicted, weight);
                ++algorithm.status_counts[prediction.status];
                for (const auto& reason : prediction.reasons) ++algorithm.rejection_reason_counts[reason];
                if (predicted == 4 && rpm_rejection(prediction)) {
                    ++algorithm.rpm_support_missing_windows;
                    if (prediction.reasons.empty()) ++algorithm.rpm_support_reason_counts[prediction.status];
                    for (const auto& reason : prediction.reasons) ++algorithm.rpm_support_reason_counts[reason];
                }
                const auto b = rpm_bin_index(window, algorithm.rpm_bins); auto& bin = algorithm.rpm_bins[b];
                record(bin.metrics, truth, predicted, 1.0 / windows_per_bin[b]);
                ++bin.status_counts[prediction.status];
                for (const auto& reason : prediction.reasons) ++bin.rejection_reason_counts[reason];
                ReferenceValidationPrediction result;
                result.raw_window_sha256 = window.raw_window_sha256; result.first_index = window.first_index; result.last_index = window.last_index;
                result.rpm_min = window.rpm_min; result.rpm_max = window.rpm_max; result.file_weight = weight;
                result.classification = prediction.classification; result.status = prediction.status;
                result.reference_eligible_files = prediction.reference_eligible_files; result.reasons = prediction.reasons;
                result.speed_support_status = prediction.speed_support_status;
                result.reference_speed_distance = prediction.reference_speed_distance; result.speed_support_factor = prediction.speed_support_factor;
                result.shape_support_status = prediction.shape_support_status; result.reference_second_distance = prediction.reference_second_distance;
                fm.predictions.push_back(std::move(result));
            }
            for (std::size_t bin = 0; bin < windows_per_bin.size(); ++bin)
                if (windows_per_bin[bin]) ++algorithm.rpm_bins[bin].metrics.files;
        }
        report.folds.push_back(std::move(fold));
    }
    for (auto& algorithm : report.algorithms) {
        finish(algorithm.metrics);
        for (auto& bin : algorithm.rpm_bins) finish(bin.metrics);
    }
    report.status = "completed"; report.metrics_available = true;
    report.reasons.push_back("capture_session_heldout_only_not_independent_machine_or_remount_validation");
    report.reasons.push_back("fixed_default_policies_no_threshold_search_or_best_model_selection");
    return report;
}

std::string reference_validation_json(const ReferenceValidationReport& report) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17);
    out << "{\"schema\":" << quote(report.schema) << ",\"status\":" << quote(report.status)
        << ",\"scope\":" << quote(report.scope) << ",\"validated\":false,\"metrics_available\":" << (report.metrics_available ? "true" : "false")
        << ",\"input_sha256\":" << quote(report.input_sha256) << ",\"setup_id\":" << quote(report.setup_id)
        << ",\"configuration\":" << quote(report.configuration) << ",\"sample_rate_hz\":" << report.sample_rate_hz
        << ",\"minimum_independent_files_per_class\":3,\"maximum_files\":256,\"maximum_windows_per_file\":32"
        << ",\"independent_files_per_class\":"; numbers(out, report.independent_files_per_class);
    out << ",\"row_labels\":[\"normal\",\"imbalance\",\"misalignment\",\"looseness\"]"
        << ",\"column_labels\":[\"normal\",\"imbalance\",\"misalignment\",\"looseness\",\"unknown\"]"
        << ",\"fold_unit\":\"whole_capture_file_with_distinct_session\",\"severity_labels_used\":false,\"threshold_tuning\":false,\"best_model_selected\":false"
        << ",\"reasons\":"; strings(out, report.reasons);
    out << ",\"inputs\":[";
    for (std::size_t i = 0; i < report.inputs.size(); ++i) {
        if (i) out << ',';
        const auto& input = report.inputs[i];
        out << "{\"path\":" << quote(input.path) << ",\"label\":" << quote(input.label)
            << ",\"capture_sha256\":" << quote(input.capture_sha256) << ",\"session_id\":" << quote(input.session_id)
            << ",\"session_identity_source\":" << quote(input.session_identity_source)
            << ",\"operator_updated_at\":" << quote(input.operator_updated_at) << ",\"sample_input_sha256\":" << quote(input.sample_input_sha256)
            << ",\"raw_window_sha256\":"; strings(out, input.raw_window_sha256); out << '}';
    }
    out << "],\"algorithms\":[";
    for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
        if (a) out << ',';
        const auto& algorithm = report.algorithms[a];
        out << "{\"policy\":" << quote(algorithm.policy) << ",\"model_kind\":" << quote(algorithm.model_kind)
            << ",\"full_model_sha256\":" << quote(algorithm.full_model_sha256)
            << ",\"full_reference_provenance_sha256\":" << quote(algorithm.full_reference_provenance_sha256)
            << ",\"metrics\":"; write_metrics(out, algorithm.metrics);
        out << ",\"status_counts\":"; counts(out, algorithm.status_counts);
        out << ",\"rejection_reason_counts\":"; counts(out, algorithm.rejection_reason_counts);
        out << ",\"rpm_support_missing_windows\":" << algorithm.rpm_support_missing_windows << ",\"rpm_support_reason_counts\":";
        counts(out, algorithm.rpm_support_reason_counts); out << ",\"rpm_bins\":[";
        for (std::size_t b = 0; b < algorithm.rpm_bins.size(); ++b) {
            if (b) out << ',';
            const auto& bin = algorithm.rpm_bins[b];
            out << "{\"name\":" << quote(bin.name) << ",\"lower_rpm\":" << bin.lower_rpm << ",\"upper_rpm\":";
            if (bin.upper_unbounded || bin.spans_bins) out << "null"; else out << bin.upper_rpm;
            out << ",\"spans_bins\":" << (bin.spans_bins ? "true" : "false") << ",\"metrics\":"; write_metrics(out, bin.metrics);
            out << ",\"status_counts\":"; counts(out, bin.status_counts);
            out << ",\"rejection_reason_counts\":"; counts(out, bin.rejection_reason_counts); out << '}';
        }
        out << "]}";
    }
    out << "],\"folds\":[";
    for (std::size_t i = 0; i < report.folds.size(); ++i) {
        if (i) out << ',';
        const auto& fold = report.folds[i];
        out << "{\"heldout_capture_sha256\":" << quote(fold.heldout_capture_sha256) << ",\"heldout_session_id\":" << quote(fold.heldout_session_id)
            << ",\"heldout_input_sha256\":" << quote(fold.heldout_input_sha256) << ",\"train_inputs_sha256\":" << quote(fold.train_inputs_sha256)
            << ",\"train_capture_sha256\":"; strings(out, fold.train_capture_sha256);
        out << ",\"train_session_id\":"; strings(out, fold.train_session_id); out << ",\"models\":[";
        for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
            if (a) out << ',';
            const auto& model = fold.models[a];
            out << "{\"policy\":" << quote(model.policy) << ",\"model_kind\":" << quote(model.model_kind)
                << ",\"model_sha256\":" << quote(model.model_sha256) << ",\"reference_provenance_sha256\":" << quote(model.reference_provenance_sha256)
                << ",\"predictions\":[";
            for (std::size_t w = 0; w < model.predictions.size(); ++w) {
                if (w) out << ',';
                const auto& p = model.predictions[w];
                out << "{\"raw_window_sha256\":" << quote(p.raw_window_sha256) << ",\"first_index\":" << p.first_index << ",\"last_index\":" << p.last_index
                    << ",\"rpm_min\":" << p.rpm_min << ",\"rpm_max\":" << p.rpm_max << ",\"file_weight\":" << p.file_weight
                    << ",\"classification\":" << quote(p.classification) << ",\"status\":" << quote(p.status) << ",\"reference_eligible_files\":";
                numbers(out, p.reference_eligible_files);
                if (model.model_kind == "reference_ensemble_v3") out << ",\"speed_support_status\":" << quote(p.speed_support_status)
                    << ",\"reference_speed_distance\":" << p.reference_speed_distance << ",\"speed_support_factor\":" << p.speed_support_factor
                    << ",\"shape_support_status\":" << quote(p.shape_support_status) << ",\"reference_second_distance\":" << p.reference_second_distance;
                out << ",\"reasons\":"; strings(out, p.reasons); out << '}';
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "]}"; return out.str();
}
} // namespace vibration
