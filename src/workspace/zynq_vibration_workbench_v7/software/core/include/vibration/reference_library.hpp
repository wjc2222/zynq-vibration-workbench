#pragma once
#include "vibration/candidate_model.hpp"
namespace vibration {
inline constexpr std::size_t reference_max_files=256;
inline constexpr std::size_t reference_max_windows_per_file=32;
struct ReferenceSample {
    std::string path, label, capture_sha256, session_id, operator_updated_at;
    std::vector<CandidateWindow> windows;
};
// Operator label is a fit target only. This object is never an inference input.
// Capture must come from CRC-validated load_capture, never a preview snapshot.
ReferenceSample extract_reference_sample(const Capture&, const std::string& label,
    const AnalysisConfig&, const std::string& setup_id,
    accelerator::Backend = accelerator::Backend::Reference);
unsigned reference_label_index(const std::string&);
void validate_reference_sample(const ReferenceSample&);
// LegacyNearest is retained for unchanged replay/evaluation of v1 models.
// New libraries use a separately versioned, two-independent-file policy.
enum class ReferenceFitPolicy { RobustEnsemble, LegacyNearest, SpeedTolerantEnsemble };
CandidateModel fit_reference_model(const std::vector<ReferenceSample>&,
    const std::string& id = "four_class_reference",
    ReferenceFitPolicy policy = ReferenceFitPolicy::SpeedTolerantEnsemble);
inline CandidateModel fit_reference_model(const std::vector<ReferenceSample>& samples,
    ReferenceFitPolicy policy, const std::string& id = "four_class_reference") {
    return fit_reference_model(samples, id, policy);
}
} // namespace vibration
