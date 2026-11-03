#include "vibration/reference_validation.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace vibration;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void near(double a, double b, const char* message) { check(std::abs(a - b) < 1e-10, message); }
template<class F> void rejects(F operation, const char* message) {
    bool failed = false;
    try { operation(); } catch (const AnalysisError&) { failed = true; }
    check(failed, message);
}
std::string hash(const std::string& value) {
    return sha256_hex(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}
// MATHEMATICAL POST-EXTRACTION FIXTURES ONLY. Fabricated identities and ideal
// separated features exercise software contracts; they are not measurements,
// saved machine captures, mechanical validation, or reported real accuracy.
ReferenceSample sample(unsigned category, unsigned identity, unsigned windows = 1) {
    const char* labels[]{"normal", "imbalance", "misalignment", "looseness"};
    ReferenceSample s;
    s.path = "mathematical-only-" + std::to_string(identity) + ".BIN";
    s.label = labels[category]; s.capture_sha256 = hash(s.path);
    s.session_id = "math-session-" + std::to_string(identity); s.operator_updated_at = "fixture-only";
    for (unsigned i = 0; i < windows; ++i) {
        CandidateWindow w;
        w.setup_id = "mathematical-setup"; w.configuration = "abcd1234"; w.sample_rate_hz = 5000;
        w.capture_sha256 = s.capture_sha256; w.raw_window_sha256 = hash(s.path + ":window:" + std::to_string(i));
        w.source_path = s.path; w.source = "mathematical-feature-fixture"; w.source_session_id = s.session_id;
        w.valid = true; w.first_index = 10000 * i; w.last_index = w.first_index + 9999;
        w.rpm = w.rpm_min = w.rpm_max = 1350;
        for (unsigned j = 0; j < candidate_feature_count; ++j) w.features[j] = j % 4 == category ? 10.0 : 0.1;
        s.windows.push_back(w);
    }
    return s;
}
std::vector<ReferenceSample> library(unsigned files_per_class = 3) {
    std::vector<ReferenceSample> samples;
    for (unsigned c = 0; c < 4; ++c)
        for (unsigned i = 0; i < files_per_class; ++i) samples.push_back(sample(c, c * 100 + i));
    return samples;
}
const ReferenceValidationFold& fold_for(const ReferenceValidationReport& report, const std::string& digest) {
    for (const auto& fold : report.folds) if (fold.heldout_capture_sha256 == digest) return fold;
    throw std::runtime_error("expected fold missing");
}
double mass(const ReferenceValidationMetrics& m) {
    double n = 0; for (const auto& row : m.confusion_file_weights) for (double value : row) n += value;
    return n;
}
void rename(ReferenceSample& sample, const std::string& path) {
    sample.path = path; for (auto& window : sample.windows) window.source_path = path;
}
}

int main(int argc, char** argv) {
    try {
        const auto samples = library();
        const auto report = evaluate_reference_models(samples);
        check(report.status == "completed" && report.metrics_available && !report.validated, "completed is never mechanical validation");
        check(report.scope == "capture_session_heldout_not_independent_remount", "scope is explicit");
        check(report.folds.size() == samples.size(), "exactly one whole-file fold per independent capture");
        for (auto count : report.independent_files_per_class) check(count == 3, "minimum distinct-file counts retained");
        for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
            const auto& result = report.algorithms[a];
            check(result.metrics.files == 12 && result.metrics.windows == 12, "files and windows are separately counted");
            near(result.metrics.accuracy, 1, "separated mathematical fixtures are recognized when both policies have support");
            near(result.metrics.macro_f1, 1, "macro F1 uses four classes");
            near(result.metrics.acceptance_coverage, 1, "acceptance coverage independently recorded");
            check(result.full_model_sha256.size() == 64 && result.full_reference_provenance_sha256.size() == 64, "full-library validation model hashes retained");
        }
        for (const auto& fold : report.folds) {
            check(fold.train_capture_sha256.size() == 11 && fold.train_session_id.size() == 11, "all other whole files train");
            check(std::find(fold.train_capture_sha256.begin(), fold.train_capture_sha256.end(), fold.heldout_capture_sha256) == fold.train_capture_sha256.end(), "heldout hash absent from training");
            check(std::find(fold.train_session_id.begin(), fold.train_session_id.end(), fold.heldout_session_id) == fold.train_session_id.end(), "heldout session absent from training");
            check(fold.train_inputs_sha256.size() == 64 && fold.heldout_input_sha256.size() == 64, "fold exact input digests retained");
            for (const auto& model : fold.models) check(model.model_sha256.size() == 64 && model.predictions.size() == 1, "same fold contains both policies and heldout predictions");
            std::vector<ReferenceSample> train;
            for (const auto& s : samples) if (s.capture_sha256 != fold.heldout_capture_sha256) train.push_back(s);
            std::sort(train.begin(), train.end(), [](const auto& x, const auto& y) { return x.capture_sha256 < y.capture_sha256; });
            for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
                const auto model = fit_reference_model(train, "reference_validation_fold", a == 0 ? ReferenceFitPolicy::LegacyNearest : a == 1 ? ReferenceFitPolicy::RobustEnsemble : ReferenceFitPolicy::SpeedTolerantEnsemble);
                const auto text = candidate_model_text(model);
                check(hash(text) == fold.models[a].model_sha256, "fold model hash equals isolated training-only fit, including scaling");
                check(candidate_model_text(parse_candidate_model(text)) == text, "each compared policy model roundtrips");
            }
        }
        // Mutating only the heldout label, features and window count must leave
        // that fold's training models identical. This detects all-window and
        // full-library fit/scaling leakage, not just the first-window leakage.
        const auto leakage_samples = library(4);
        const auto leakage_base = evaluate_reference_models(leakage_samples);
        auto changed = leakage_samples; changed[0] = sample(0, 0, 7);
        changed[0].label = "imbalance";
        for (auto& window : changed[0].windows) window.features.fill(1e12);
        const auto altered = evaluate_reference_models(changed);
        const auto& before = fold_for(leakage_base, samples[0].capture_sha256);
        const auto& after = fold_for(altered, samples[0].capture_sha256);
        check(before.train_inputs_sha256 == after.train_inputs_sha256, "heldout target/features/windows never enter train input digest");
        for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
            check(before.models[a].model_sha256 == after.models[a].model_sha256, "heldout data and label cannot affect fold fit or scaling");
            check(after.models[a].predictions.size() == 7, "all heldout file windows are evaluated");
        }
        auto reordered = samples; std::reverse(reordered.begin(), reordered.end());
        check(reference_validation_json(evaluate_reference_models(reordered)) == reference_validation_json(report), "canonical report independent of input order");
        auto unbalanced_windows = samples; unbalanced_windows[0] = sample(0, 0, 32);
        const auto balanced = evaluate_reference_models(unbalanced_windows);
        for (unsigned a = 0; a < reference_validation_policy_count; ++a) {
            const auto& m = balanced.algorithms[a].metrics;
            check(m.windows == 43 && m.files == 12, "32 windows are one file, not 32 trials");
            near(mass(m), 12, "every file has exactly one confusion weight");
            for (unsigned c = 0; c < 4; ++c) near(m.confusion_file_weights[c][c], 3, "long normal capture does not dominate other classes");
        }
        // A single remote-RPM file has no local training support in its fold.
        // In addition it removes one member of that class from the low-RPM
        // training pool: V2 truthfully needs two local files per class.
        auto gap = samples;
        for (auto& w : gap[0].windows) w.rpm = w.rpm_min = w.rpm_max = 6000;
        const auto missing = evaluate_reference_models(gap);
        for (unsigned a = 0; a < 2; ++a) {
            const auto& m = missing.algorithms[a].metrics;
            check(m.confusion_window_counts[0][4] >= 1, "RPM-unsupported query stays unknown in confusion");
            check(m.recall[0] < 1 && m.accuracy < 1 && m.acceptance_coverage < 1, "unknown lowers recall, accuracy and coverage");
            check(missing.algorithms[a].rpm_support_missing_windows >= 1 && !missing.algorithms[a].rpm_support_reason_counts.empty(), "local RPM support failure reason retained");
            check(missing.algorithms[a].rpm_bins[4].metrics.windows == 1, "high RPM bin retains rejected file");
        }
        // Every query has a unique operating speed, so no fold has local speed
        // support. Empty accepted denominator must be null, never 100 percent.
        auto zero_accept = samples;
        for (unsigned i = 0; i < zero_accept.size(); ++i)
            for (auto& w : zero_accept[i].windows) w.rpm = w.rpm_min = w.rpm_max = std::pow(2.0, i + 8);
        const auto rejected = evaluate_reference_models(zero_accept);
        for (unsigned a = 0; a < 2; ++a) {
            const auto& result = rejected.algorithms[a];
            near(result.metrics.acceptance_coverage, 0, "all-unknown coverage is zero");
            check(!result.metrics.accepted_accuracy_available, "accepted accuracy unavailable with zero accepted cases");
            near(result.metrics.accuracy, 0, "all-unknown accuracy includes every file");
        }
        check(rejected.algorithms[2].metrics.acceptance_coverage > 0, "V3 does not fabricate an RPM-only hard rejection for shape-supported mathematical fixtures");
        check(missing.algorithms[2].metrics.confusion_window_counts[0][0] == 3, "V3 retains the remote-RPM shape-supported query in scoring");
        check(reference_validation_json(missing).find("cross_speed_reference_support_limited_requires_machine_validation") != std::string::npos, "V3 report retains cross-speed caution rather than implying validation");
        check(reference_validation_json(rejected).find("\"accepted_accuracy\":null") != std::string::npos, "undefined accepted accuracy serializes null");
        auto class_imbalance = library(4);
        // Three extra normal trials, all too far in feature space, must count
        // as errors even when the class has many accepted easy trials.
        for (unsigned i = 0; i < 3; ++i) {
            auto s = sample(0, 500 + i); s.windows[0].features.fill(1e15 * (i + 1));
            s.windows[0].rpm = s.windows[0].rpm_min = s.windows[0].rpm_max = 10000 + i * 10000;
            class_imbalance.push_back(s);
        }
        const auto imbalance = evaluate_reference_models(class_imbalance);
        for (const auto& result : imbalance.algorithms) {
            const auto& m = result.metrics;
            near(mass(m), 19, "class imbalance does not change per-file mass");
            near(m.macro_f1, (m.f1[0] + m.f1[1] + m.f1[2] + m.f1[3]) / 4, "macro F1 averages classes equally despite file imbalance");
            check(m.recall[0] < 1, "hard files cannot be silently omitted");
        }
        for (unsigned count : {0u, 1u, 2u}) {
            const auto insufficient = evaluate_reference_models(library(count));
            check(insufficient.status == "insufficient_independent_files" && !insufficient.metrics_available && insufficient.folds.empty(), "minimum three independent files per class before evaluation");
            for (const auto& a : insufficient.algorithms) check(!a.metrics.available, "insufficient evidence has no fabricated metrics");
            check(reference_validation_json(insufficient).find("\"metrics\":null") != std::string::npos, "insufficient JSON uses null metrics");
        }
        auto unknown_session = samples; unknown_session[0].session_id.clear(); unknown_session[0].windows[0].source_session_id.clear();
        const auto missing_session = evaluate_reference_models(unknown_session);
        check(missing_session.status == "insufficient_independent_files", "missing session cannot assert independence");
        check(missing_session.independent_files_per_class[0] == 2, "unidentified session does not count as an independent file");
        auto annotation_session = samples; annotation_session[0].windows[0].source_session_id.clear();
        const auto annotated = evaluate_reference_models(annotation_session);
        check(annotated.status == "completed", "legacy capture can use an explicit distinct saved session annotation");
        check(std::any_of(annotated.inputs.begin(), annotated.inputs.end(), [](const auto& input) {
            return input.session_identity_source == "sample_annotation_window_session_absent";
        }), "report distinguishes annotated session from raw-window session provenance");
        auto duplicate = samples; duplicate.push_back(samples[0]);
        rejects([&] { evaluate_reference_models(duplicate); }, "duplicate file/hash rejected");
        duplicate = samples; duplicate[1].session_id = duplicate[0].session_id;
        for (auto& w : duplicate[1].windows) w.source_session_id = duplicate[1].session_id;
        rejects([&] { evaluate_reference_models(duplicate); }, "same-session segments rejected");
        duplicate = samples; duplicate[1].windows[0].raw_window_sha256 = duplicate[0].windows[0].raw_window_sha256;
        rejects([&] { evaluate_reference_models(duplicate); }, "copied raw window identity rejected");
        auto invalid = samples; invalid[0].windows[0].synthetic = true;
        rejects([&] { evaluate_reference_models(invalid); }, "synthetic rejected before metrics");
        invalid = samples; invalid[0].windows[0].valid = false;
        rejects([&] { evaluate_reference_models(invalid); }, "invalid-quality input rejected before metrics");
        invalid = library(2); invalid[0].windows[0].synthetic = true;
        rejects([&] { evaluate_reference_models(invalid); }, "insufficient count cannot bypass sample validation");
        invalid = samples; invalid[0].windows[0].configuration = "other";
        rejects([&] { evaluate_reference_models(invalid); }, "mixed extraction configuration rejected before folds");
        invalid = samples; invalid[0].windows[0].source_session_id = "forged-alias";
        rejects([&] { evaluate_reference_models(invalid); }, "inconsistent window session rejected");
        invalid = samples; invalid[0].windows[0].features[0] = std::numeric_limits<double>::quiet_NaN();
        rejects([&] { evaluate_reference_models(invalid); }, "nonfinite feature rejected");
        invalid = samples; invalid[0].windows[0].rpm_max = std::numeric_limits<double>::infinity();
        rejects([&] { evaluate_reference_models(invalid); }, "nonfinite RPM rejected");
        invalid = samples; invalid[0] = sample(0, 0, 33);
        rejects([&] { evaluate_reference_models(invalid); }, "32 windows budget enforced before folds");
        invalid.clear(); for (unsigned i = 0; i < 257; ++i) invalid.push_back(sample(i % 4, 1000 + i));
        rejects([&] { evaluate_reference_models(invalid); }, "256 file budget enforced before folds");
        auto escaped = samples;
        rename(escaped[0], "math-quote\"-slash\\-tab\t-newline\n-control" + std::string(1, '\x01') + ".BIN");
        escaped[0].operator_updated_at = "mathematical\"operator\nonly";
        const auto escaped_report = evaluate_reference_models(escaped);
        const auto json = reference_validation_json(escaped_report);
        check(json.find("math-quote\\\"-slash\\\\-tab\\u0009-newline\\u000a-control\\u0001.BIN") != std::string::npos, "JSON escapes quotes, slashes, tabs, newlines and controls");
        check(json.find("\n") == std::string::npos && json.find("\t") == std::string::npos, "JSON has no unescaped control bytes");
        auto provenance_change = samples; provenance_change[0].operator_updated_at += "-changed";
        check(evaluate_reference_models(provenance_change).input_sha256 != report.input_sha256, "operator provenance is bound into input hash");
        if (argc > 1 && std::string(argv[1]) == "--json") std::cout << json << '\n';
        else std::cout << "PASS " << checks << " checks; mathematical post-extraction fixtures only. No measured mechanical validation.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
