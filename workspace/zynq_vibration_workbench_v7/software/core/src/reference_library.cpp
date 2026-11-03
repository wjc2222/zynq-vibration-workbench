#include "vibration/reference_library.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
namespace vibration {
namespace {
void require(bool condition,const std::string& message){if(!condition)throw AnalysisError("Reference library: "+message);}
bool digest(const std::string& s){return s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos;}
void validate_ensemble_window(const CandidateWindow& w,const ReferenceSample& s){
    require(w.feature_schema=="lowband36.v1"&&w.quality_policy==candidate_quality_policy,"unsupported feature or quality policy");
    require(std::isfinite(w.rpm)&&std::isfinite(w.rpm_min)&&std::isfinite(w.rpm_max)&&w.rpm_min>0&&w.rpm>=w.rpm_min&&w.rpm<=w.rpm_max&&w.rpm_max<1e100,"invalid finite RPM support");
    require(std::isfinite(w.speed_cv)&&w.speed_cv>=0&&w.speed_cv<=.05,"invalid or unstable reference speed");
    require(w.source_session_id.empty()||w.source_session_id==s.session_id,"window/session identity mismatch");
    for(double x:w.features)require(std::isfinite(x)&&x>=0&&x<1e100,"invalid nonnegative reference feature");
}
double coordinate_median(std::vector<double> values){
    std::sort(values.begin(),values.end());const auto mid=values.size()/2;
    return values.size()%2?values[mid]:values[mid-1]+(values[mid]-values[mid-1])/2;
}
std::string ensemble_identity(const CandidateModel& m){
    std::ostringstream identity;identity.imbue(std::locale::classic());identity<<std::setprecision(17)<<m.schema<<'|'<<m.kind<<'|'<<m.reference_policy<<'|'<<reference_ensemble_speed_group_ratio<<'|'<<reference_ensemble_speed_tolerance<<'|'<<reference_ensemble_minimum_files_per_class<<'|'<<reference_ensemble_iqr_normalizer<<'|'<<reference_ensemble_relative_scale_floor<<'|'<<reference_ensemble_amplitude_scale_floor<<'|'<<reference_ensemble_shape_scale_floor<<'\n';
    if(m.kind=="reference_ensemble_v3")identity<<"shape_count=30|amplitude_count=6|log2_amplitude_weight="<<reference_speed_amplitude_weight<<"|smooth_speed_max="<<reference_speed_maximum_penalty<<"|smooth_speed_transition="<<reference_speed_transition_ratio<<"|parent_model_sha256="<<m.reference_parent_model_sha256<<'\n';
    for(const auto&e:m.references){identity<<e.class_index<<'|'<<e.capture_sha256<<'|'<<e.session_id<<'|'<<e.rpm_min<<'|'<<e.rpm_max;for(double x:e.features)identity<<'|'<<x;identity<<'\n';}return identity.str();
}
}
unsigned reference_label_index(const std::string& s){const char*labels[]{"normal","imbalance","misalignment","looseness"};for(unsigned i=0;i<4;++i)if(s==labels[i])return i;throw AnalysisError("Reference library: operator label must be one of four confirmed classes");}
void validate_reference_sample(const ReferenceSample& s){
    reference_label_index(s.label);require(!s.path.empty()&&digest(s.capture_sha256)&&!s.windows.empty(),"missing committed BIN identity or valid windows");
    const auto&first=s.windows.front();std::uint64_t previous=0;bool initial=true;
    for(const auto&w:s.windows){require(w.valid&&!w.synthetic&&w.reasons.empty()&&w.input_kind=="committed_bin"&&w.capture_sha256==s.capture_sha256&&digest(w.raw_window_sha256),"reference must contain quality-valid non-synthetic committed BIN windows");require(w.setup_id==first.setup_id&&w.configuration==first.configuration&&w.sample_rate_hz==first.sample_rate_hz,"reference configurations differ");require(initial||w.first_index>previous,"reference windows overlap or are unordered");require(w.last_index>=w.first_index&&w.rpm_min>0&&w.rpm_max>=w.rpm_min,"invalid reference support or RPM");for(double x:w.features)require(std::isfinite(x),"nonfinite reference feature");initial=false;previous=w.last_index;}
}
ReferenceSample extract_reference_sample(const Capture&capture,const std::string&label,const AnalysisConfig&cfg,const std::string&setup,accelerator::Backend backend){
    reference_label_index(label);require(capture.integrity_verified&&!capture.source_metadata.synthetic&&digest(capture.capture_sha256),"only CRC-validated non-synthetic saved BIN files may enter the library");require(cfg.signal_kind==SignalKind::VibrationConfirmed,"mechanical source must be explicitly confirmed");ReferenceSample s;s.path=capture.source_path;s.label=label;s.capture_sha256=capture.capture_sha256;
    const auto n=accelerator::required_raw_frames(capture.header.sample_rate_hz);
    const auto complete=capture.records.size()/n;const auto selected=std::min(complete,reference_max_windows_per_file);for(std::size_t i=0;i<selected;++i){const auto windowIndex=selected==1?0:i*(complete-1)/(selected-1);auto w=extract_candidate_window(capture,windowIndex*n,cfg,setup,backend);if(w.valid){if(s.session_id.empty())s.session_id=w.source_session_id;s.windows.push_back(std::move(w));}}
    require(!s.windows.empty(),"no complete quality-valid window (check RPM, PPR, clipping, amplitude and source confirmation)");validate_reference_sample(s);return s;
}
CandidateModel fit_reference_model(const std::vector<ReferenceSample>&samples,const std::string&id,ReferenceFitPolicy policy){
    require(samples.size()>=4&&samples.size()<=reference_max_files,"all four classes require distinct saved BIN files; maximum 256 files");
    require(policy==ReferenceFitPolicy::RobustEnsemble||policy==ReferenceFitPolicy::LegacyNearest||policy==ReferenceFitPolicy::SpeedTolerantEnsemble,"unsupported fit policy");
    const bool speedTolerant=policy==ReferenceFitPolicy::SpeedTolerantEnsemble;
    const bool robust=policy!=ReferenceFitPolicy::LegacyNearest;
    CandidateModel m;m.kind=speedTolerant?"reference_ensemble_v3":robust?"reference_ensemble_v2":"reference_exemplars";m.id=id;m.maximum_distance=1.5;m.minimum_score=55;m.minimum_margin=12;
    if(robust){m.schema=speedTolerant?"zynq_vibration.candidate_model.v3":"zynq_vibration.candidate_model.v2";m.reference_policy=speedTolerant?reference_speed_tolerant_policy:reference_ensemble_policy;}
    require(!samples.front().windows.empty(),"missing reference windows");const auto&first=samples.front().windows.front();m.setup_id=first.setup_id;m.configuration=speedTolerant?canonical_candidate_configuration_for_v3(first.configuration):first.configuration;m.fs=first.sample_rate_hz;m.rpm_min=std::numeric_limits<double>::max();
    std::set<std::string>hashes,paths,sessions;std::vector<std::string>provenance;
    for(const auto&s:samples){
        validate_reference_sample(s);require(s.windows.size()<=reference_max_windows_per_file,"maximum 32 selected complete windows per BIN");const auto c=reference_label_index(s.label);
        require(hashes.insert(s.capture_sha256).second&&paths.insert(s.path).second,"duplicate BIN or byte-identical copied reference");require(s.session_id.empty()||sessions.insert(s.session_id).second,"multiple segments of one capture session are not independent reference files");
        for(const auto&w:s.windows){require(w.setup_id==m.setup_id&&(speedTolerant?candidate_configurations_compatible_for_v3(w.configuration,m.configuration):w.configuration==m.configuration)&&w.sample_rate_hz==m.fs,"setup, units, conversion, FIR, PPR or sample rate mismatch");if(robust)validate_ensemble_window(w,s);}
        ++m.reference_file_counts[c];
        // Collapse stable-speed windows inside a file to one representative.
        // Keep separate speed operating points and separately acquired severities.
        std::vector<const CandidateWindow*>ordered;for(const auto&w:s.windows)ordered.push_back(&w);std::sort(ordered.begin(),ordered.end(),[&](const auto*a,const auto*b){if(!robust)return a->rpm_min<b->rpm_min;if(a->rpm_min!=b->rpm_min)return a->rpm_min<b->rpm_min;if(a->rpm_max!=b->rpm_max)return a->rpm_max<b->rpm_max;return a->first_index<b->first_index;});
        for(std::size_t begin=0;begin<ordered.size();){
            std::size_t end=begin+1;double lo=ordered[begin]->rpm_min,hi=ordered[begin]->rpm_max;
            while(end<ordered.size()&&ordered[end]->rpm_max<=lo*(robust?reference_ensemble_speed_group_ratio:1.04)){hi=std::max(hi,ordered[end]->rpm_max);++end;}
            CandidateReferenceExemplar e;e.class_index=c;e.rpm_min=lo;e.rpm_max=hi;e.capture_sha256=s.capture_sha256;e.session_id=s.session_id;
            for(unsigned j=0;j<candidate_feature_count;++j){
                if(robust){std::vector<double> coordinate;for(std::size_t i=begin;i<end;++i)coordinate.push_back(ordered[i]->features[j]);e.features[j]=coordinate_median(std::move(coordinate));}
                else for(std::size_t i=begin;i<end;++i)e.features[j]+=ordered[i]->features[j]/(end-begin);
            }
            m.references.push_back(std::move(e));m.rpm_min=std::min(m.rpm_min,lo*(speedTolerant?1.:.9));m.rpm_max=std::max(m.rpm_max,hi*(speedTolerant?1.:1.1));begin=end;
        }
        provenance.push_back(s.label+"|"+s.capture_sha256+"|"+s.session_id+"|"+s.operator_updated_at);
    }
    for(double count:m.reference_file_counts)require(count>=1,"all four classes need at least one independently acquired BIN");
    require(m.references.size()<=candidate_reference_max_exemplars,"reference model exceeds exemplar limit");m.baseline_count=unsigned(m.reference_file_counts[0]);m.scale.fill(1.);
    std::sort(provenance.begin(),provenance.end());std::string material;
    if(robust){
        // Version and aggregation are part of provenance, not a reinterpretation
        // of old nearest-neighbour artifacts. Source order cannot change it.
        std::sort(m.references.begin(),m.references.end(),[](const auto&a,const auto&b){if(a.capture_sha256!=b.capture_sha256)return a.capture_sha256<b.capture_sha256;if(a.rpm_min!=b.rpm_min)return a.rpm_min<b.rpm_min;return a.rpm_max<b.rpm_max;});
        material=ensemble_identity(m);
    }
    for(const auto&s:provenance)material+=s+"\n";
    m.reference_provenance_sha256=sha256_hex(reinterpret_cast<const std::uint8_t*>(material.data()),material.size());
    return parse_candidate_model(candidate_model_text(m));
}
CandidateModel upgrade_reference_model_to_speed_tolerant(const CandidateModel& original){
    const auto parentText=candidate_model_text(original); // validate old semantics first
    require(original.kind=="reference_ensemble_v2","explicit policy upgrade requires a V2 ensemble model");
    CandidateModel m=original;m.kind="reference_ensemble_v3";m.schema="zynq_vibration.candidate_model.v3";m.reference_policy=reference_speed_tolerant_policy;
    m.configuration=canonical_candidate_configuration_for_v3(original.configuration);
    m.rpm_min=std::numeric_limits<double>::max();m.rpm_max=0;
    for(const auto& e:m.references){m.rpm_min=std::min(m.rpm_min,e.rpm_min);m.rpm_max=std::max(m.rpm_max,e.rpm_max);}
    m.reference_parent_model_sha256=sha256_hex(reinterpret_cast<const std::uint8_t*>(parentText.data()),parentText.size());
    const std::string material="explicit_v2_aggregated_reference_policy_upgrade_no_reextraction_v1\nparent_provenance="+original.reference_provenance_sha256+"\n"+ensemble_identity(m);
    m.reference_provenance_sha256=sha256_hex(reinterpret_cast<const std::uint8_t*>(material.data()),material.size());
    return parse_candidate_model(candidate_model_text(m));
}
} // namespace vibration
