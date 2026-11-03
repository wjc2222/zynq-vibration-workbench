#include "vibration/candidate_model.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

namespace vibration {
namespace {
const char* labels[]{"normal", "imbalance", "misalignment", "looseness"};
void need(bool b,const std::string& s){if(!b)throw AnalysisError("Candidate model: "+s);}
bool finite(double x){return std::isfinite(x)&&std::abs(x)<1e100;}
std::string quote(const std::string& s){std::ostringstream o;o<<'"';for(unsigned char c:s){if(c=='"'||c=='\\')o<<'\\'<<char(c);else if(c<32)o<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c)<<std::dec;else o<<char(c);}return o.str()+'"';}
std::string hex(const std::string& s){const char*d="0123456789abcdef";std::string r;for(unsigned char c:s){r+=d[c>>4];r+=d[c&15];}return r;}
template<class T>void numbers(std::ostream& o,const T& a){bool first=true;for(double x:a){if(!first)o<<',';o<<x;first=false;}}
double number(const std::string& s){std::istringstream in(s);in.imbue(std::locale::classic());double x=0;in>>std::noskipws>>x;need(!in.fail()&&in.peek()==EOF&&finite(x),"invalid finite number");return x;}
template<class T>void parse_numbers(const std::string& s,T& a){std::istringstream in(s);std::string item;std::size_t i=0;while(std::getline(in,item,',')){need(i<a.size(),"too many vector elements");a[i++]=number(item);}need(i==a.size()&&!s.empty()&&s.back()!=',',"wrong vector size");}
void validate(const CandidateModel& m){
    const bool speedTolerant=m.kind=="reference_ensemble_v3";
    const bool ensemble=m.kind=="reference_ensemble_v2"||speedTolerant;
    const bool reference=m.kind=="reference_prototype"||m.kind=="reference_exemplars"||ensemble;
    need(m.schema==(speedTolerant?"zynq_vibration.candidate_model.v3":ensemble?"zynq_vibration.candidate_model.v2":"zynq_vibration.candidate_model.v1")&&m.status=="provisional","unsupported schema/status");
    need(!m.id.empty()&&m.id.size()<=128&&!m.setup_id.empty()&&m.setup_id.size()<=128,"invalid identity");
    need(m.kind=="engineering_prior"||m.kind=="linear_softmax"||reference,"unsupported kind");
    need(ensemble?m.reference_policy==(speedTolerant?reference_speed_tolerant_policy:reference_ensemble_policy):m.reference_policy.empty(),"unsupported reference aggregation policy");
    need(m.reference_parent_model_sha256.empty()||(speedTolerant&&m.reference_parent_model_sha256.size()==64&&m.reference_parent_model_sha256.find_first_not_of("0123456789abcdef")==std::string::npos),"invalid parent model digest");
    need(m.configuration.size()>=8&&m.configuration.size()<=4096&&m.configuration.find_first_not_of("0123456789abcdef")==std::string::npos,"invalid configuration hex");
    need((m.fs==5000||m.fs==10000||m.fs==25000||m.fs==50000)&&m.baseline_count>=(reference?1u:5u)&&m.baseline_count<=1000000,"invalid fs or baseline count");
    if(reference){
        for(double count:m.reference_file_counts)need(finite(count)&&count>=1&&count<=1000000&&std::floor(count)==count,"invalid reference file counts");
        need(m.reference_file_counts[0]==m.baseline_count,"reference normal file count mismatch");
        need(m.reference_provenance_sha256.size()==64&&m.reference_provenance_sha256.find_first_not_of("0123456789abcdef")==std::string::npos,"missing reference provenance digest");
    }
    need(finite(m.rpm_min)&&finite(m.rpm_max)&&m.rpm_min>0&&m.rpm_min<=m.rpm_max,"invalid speed domain");
    need(finite(m.minimum_score)&&m.minimum_score>=0&&m.minimum_score<=100&&finite(m.minimum_margin)&&m.minimum_margin>=0&&m.minimum_margin<=100&&finite(m.maximum_distance)&&m.maximum_distance>0,"invalid rejection thresholds");
    for(std::size_t i=0;i<36;++i)need(finite(m.mean[i])&&finite(m.scale[i])&&m.scale[i]>0,"invalid standardization");
    for(double x:m.bias)need(finite(x),"invalid bias");
    for(double x:m.weights)need(finite(x),"invalid weight");
    if(m.kind=="reference_exemplars"||ensemble){
        need(!m.references.empty()&&m.references.size()<=candidate_reference_max_exemplars,"invalid exemplar count");std::array<std::set<std::string>,4>files;std::map<std::string,unsigned>classes;
        std::map<std::string,std::string>fileSessions;std::set<std::tuple<std::string,double,double>>groups;
        for(const auto&e:m.references){need(e.class_index<4&&finite(e.rpm_min)&&finite(e.rpm_max)&&e.rpm_min>0&&e.rpm_min<=e.rpm_max,"invalid exemplar class/RPM");need(e.capture_sha256.size()==64&&e.capture_sha256.find_first_not_of("0123456789abcdef")==std::string::npos&&e.session_id.size()<=128,"invalid exemplar source identity");for(double x:e.features)need(finite(x)&&(!ensemble||x>=0),"invalid exemplar feature");files[e.class_index].insert(e.capture_sha256);const auto found=classes.emplace(e.capture_sha256,e.class_index);need(found.second||found.first->second==e.class_index,"one source has conflicting class labels");if(ensemble){const auto session=fileSessions.emplace(e.capture_sha256,e.session_id);need(session.second||session.first->second==e.session_id,"one source has conflicting acquisition sessions");need(groups.emplace(e.capture_sha256,e.rpm_min,e.rpm_max).second,"duplicate file/speed representative");}}
        need(classes.size()<=256,"reference model exceeds 256 source files");
        std::map<std::string,std::string>sessions;for(const auto&e:m.references)if(!e.session_id.empty()){const auto found=sessions.emplace(e.session_id,e.capture_sha256);need(found.second||found.first->second==e.capture_sha256,"one acquisition session has multiple source files");}
        for(unsigned c=0;c<4;++c)need(files[c].size()==m.reference_file_counts[c],"exemplar source counts mismatch");
    }
    for(const auto* s:{&m.id,&m.setup_id})need(s->find_first_of("\r\n=") == std::string::npos,"invalid text");
}
double clip(double x){return std::clamp(x,0.,1.);}
double rpm_center(double lo,double hi){return lo+(hi-lo)*.5;}
// Empirical inverse CDF: the smallest value whose cumulative weight reaches p.
// Epsilon only compensates summation at an exact rational-weight boundary.
double weighted_quantile(std::vector<std::pair<double,long double>> values,long double p){
    std::sort(values.begin(),values.end(),[](const auto&a,const auto&b){return a.first<b.first;});
    long double total=0;for(const auto&v:values)total+=v.second;
    long double cumulative=0;const auto threshold=p*total;
    const auto epsilon=16*std::numeric_limits<long double>::epsilon()*total;
    for(const auto&v:values){cumulative+=v.second;if(cumulative+epsilon>=threshold)return v.first;}
    return values.back().first;
}
CandidateInference infer_reference_ensemble(const CandidateWindow&w,const CandidateModel&m,CandidateInference r){
    const bool speedTolerant=m.kind=="reference_ensemble_v3";
    // One vote candidate per source file: nearest RPM centre, then lower support
    // endpoints to break ties. Query feature distance never selects a speed group.
    std::map<std::string,const CandidateReferenceExemplar*> selected;
    std::set<std::string> missingSessionFiles;
    const double queryRpm=rpm_center(w.rpm_min,w.rpm_max);
    for(const auto&e:m.references){
        if((!w.capture_sha256.empty()&&w.capture_sha256==e.capture_sha256)||(!w.source_session_id.empty()&&w.source_session_id==e.session_id))r.reference_replay=true;
        // Different BIN hashes alone do not establish independent acquisitions.
        // Keep such files in the artifact, but let neither support nor scaling
        // infer independence from an absent capture-session identity.
        if(e.session_id.empty()){missingSessionFiles.insert(e.capture_sha256);continue;}
        if(!speedTolerant&&(w.rpm_min<e.rpm_min*(1-reference_ensemble_speed_tolerance)||w.rpm_max>e.rpm_max*(1+reference_ensemble_speed_tolerance)))continue;
        const auto found=selected.find(e.capture_sha256);
        bool replace=found==selected.end();
        if(!replace){const auto*prior=found->second;const double delta=std::abs(queryRpm-rpm_center(e.rpm_min,e.rpm_max)),old=std::abs(queryRpm-rpm_center(prior->rpm_min,prior->rpm_max));replace=delta<old||(delta==old&&(e.rpm_min<prior->rpm_min||(e.rpm_min==prior->rpm_min&&e.rpm_max<prior->rpm_max)));}
        if(replace)selected[e.capture_sha256]=&e;
    }
    for(const auto&entry:selected)++r.reference_eligible_files[entry.second->class_index];
    std::ostringstream coverage;coverage<<(speedTolerant?"RPM-nearest distinct BIN counts normal/imbalance/misalignment/looseness=":"RPM-local distinct BIN counts normal/imbalance/misalignment/looseness=");
    bool complete=true;for(unsigned c=0;c<4;++c){if(c)coverage<<'/';coverage<<r.reference_eligible_files[c];complete=complete&&r.reference_eligible_files[c]>=reference_ensemble_minimum_files_per_class;}
    r.evidence.push_back(coverage.str());r.evidence.push_back("reference_policy="+m.reference_policy);r.evidence.push_back("reference_provenance_sha256="+m.reference_provenance_sha256);
    if(!missingSessionFiles.empty()){
        r.reasons.push_back("reference_session_identity_missing_ignored");
        r.evidence.push_back("reference files excluded from independent support and local scaling because acquisition session identity is missing="+std::to_string(missingSessionFiles.size()));
    }
    if(r.reference_replay)r.evidence.push_back("reference data replay or same acquisition session; not an independent validation result");
    if(!complete){r.status="reference_support_insufficient";r.reasons.push_back(speedTolerant?"at_least_two_independent_reference_files_per_class_required":"at_least_two_independent_reference_files_per_class_required_at_current_speed");return r;}
    for(unsigned j=0;j<candidate_feature_count;++j){
        std::vector<std::pair<double,long double>> values,absolute;
        for(const auto&entry:selected){const auto*e=entry.second;const long double weight=.25L/r.reference_eligible_files[e->class_index];values.emplace_back(e->features[j],weight);absolute.emplace_back(std::abs(e->features[j]),weight);}
        const double q25=weighted_quantile(values,.25L),q75=weighted_quantile(values,.75L),medianAbsolute=weighted_quantile(std::move(absolute),.5L);
        const bool amplitude=j%12==0||j%12==3;
        r.local_feature_scale[j]=speedTolerant&&amplitude?std::max(medianAbsolute*reference_ensemble_relative_scale_floor,reference_ensemble_amplitude_scale_floor):std::max({(q75-q25)/reference_ensemble_iqr_normalizer,medianAbsolute*reference_ensemble_relative_scale_floor,amplitude?reference_ensemble_amplitude_scale_floor:reference_ensemble_shape_scale_floor});
    }
    std::array<std::vector<double>,4> distances;
    std::array<std::vector<std::pair<double,double>>,4> speedDistances;
    for(const auto&entry:selected){
        const auto*e=entry.second;double squared=0,amplitudeSquared=0;
        for(unsigned j=0;j<candidate_feature_count;++j){
            if(speedTolerant&&(j%12==0||j%12==3)){
                const double floor=r.local_feature_scale[j];
                const double delta=(std::log(w.features[j]+floor)-std::log(e->features[j]+floor))/std::log(2.);
                amplitudeSquared+=delta*delta;
            }else{const double delta=(w.features[j]-e->features[j])/r.local_feature_scale[j];squared+=delta*delta;}
        }
        const double centre=rpm_center(e->rpm_min,e->rpm_max);
        // Include both measured support endpoints: a permissible mean must not
        // hide a much more remote measured speed interval. No fixed cutoff.
        const double speedLog=std::max(std::abs(std::log(w.rpm_min)-std::log(centre)),std::abs(std::log(w.rpm_max)-std::log(centre)));
        const double speedDistance=speedTolerant?reference_speed_maximum_penalty*speedLog/(speedLog+std::log(reference_speed_transition_ratio)):(queryRpm/centre-1)/reference_ensemble_speed_tolerance;
        const double featureSquared=speedTolerant?squared/30.+reference_speed_amplitude_weight*reference_speed_amplitude_weight*amplitudeSquared/6.:squared/candidate_feature_count;
        const double distance=std::hypot(std::sqrt(featureSquared),speedDistance);
        distances[e->class_index].push_back(distance);speedDistances[e->class_index].emplace_back(distance,speedLog);
    }
    std::array<double,4> classDistance{},secondDistance{};
    for(unsigned c=0;c<4;++c){auto&d=distances[c];std::sort(d.begin(),d.end());secondDistance[c]=d[1];classDistance[c]=speedTolerant?d[0]:std::hypot(d[0],d[1])/std::sqrt(2.);r.scores[c]=100*std::exp(std::max(-700.,-.5*classDistance[c]*classDistance[c]));}
    std::array<unsigned,4>order{0,1,2,3};std::sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return r.scores[a]>r.scores[b];});const auto winner=order[0];r.distance=classDistance[winner];r.margin=r.scores[winner]-r.scores[order[1]];
    if(speedTolerant){
        auto& support=speedDistances[winner];std::sort(support.begin(),support.end());
        r.reference_speed_distance=support[0].second;
        r.speed_support_factor=std::exp(-r.reference_speed_distance);
        const bool extrapolated=r.reference_speed_distance>std::log(reference_speed_transition_ratio);
        r.speed_support_status=extrapolated?"extrapolated":"near_reference";
        r.reference_second_distance=secondDistance[winner];
        r.shape_support_status=secondDistance[winner]<=m.maximum_distance?"corroborated_mode":"single_reference_mode";
        if(r.shape_support_status=="single_reference_mode")r.reasons.push_back("single_reference_mode_match_requires_independent_confirmation");
        if(extrapolated)r.reasons.push_back("cross_speed_reference_support_limited_requires_machine_validation");
        r.evidence.push_back("V3 multi-mode shape-primary: 30 order-ratio/crest/kurtosis coordinates use reference-only class-balanced IQR; 6 amplitudes use log2 ratios with 0.25 auxiliary weight, no RPM power-law normalization; nearest distinct-file mode defines each class distance, second-nearest corroborates but never vetoes a different damage severity; smooth RPM penalty bounded by 0.35; fixed provisional engineering choices, not fitted or independently validated");
        r.evidence.push_back("speed_support_factor is exp(-winning-nearest-reference absolute log-RPM displacement), an engineering speed similarity, not a calibrated probability or evidence of cross-speed diagnostic accuracy");
        if(r.shape_support_status=="single_reference_mode")r.evidence.push_back("only one nearby file supports the winning mode; library has multiple acquisitions per class but a singleton mode may be an erroneous label or isolated outlier; candidate requires independent confirmation");
        if(extrapolated)r.evidence.push_back("winning support is remote in measured RPM: candidate is extrapolated; cross-speed reference support is limited and requires actual machine validation");
        if(!m.reference_parent_model_sha256.empty())r.evidence.push_back("policy upgraded from aggregated V2 references without new feature extraction/training; parent_model_sha256="+m.reference_parent_model_sha256);
    }else r.evidence.push_back("reference-only class-balanced weighted IQR scale; RMS similarity of two closest distinct files; extra files still provide more matching opportunities; uncalibrated engineering thresholds 55/12/1.5 by default; not independently validated");
    if(!speedTolerant&&secondDistance[winner]>m.maximum_distance){r.status="rejected_unknown";r.reasons.push_back("insufficient_two_reference_support");return r;}
    if(r.scores[winner]<m.minimum_score||r.margin<m.minimum_margin||r.distance>m.maximum_distance){r.status="rejected_unknown";r.reasons.push_back("insufficient_score_margin_or_out_of_distribution");return r;}
    r.status="provisional_candidate";r.classification=labels[winner];return r;
}
}

std::string canonical_candidate_configuration_for_v3(const std::string& configuration){
    if(configuration.empty()||configuration.size()>4096||configuration.size()%2||configuration.find_first_not_of("0123456789abcdef")!=std::string::npos)return configuration;
    std::string decoded;for(std::size_t i=0;i<configuration.size();i+=2)decoded+=char(std::stoul(configuration.substr(i,2),nullptr,16));
    std::istringstream in(decoded);std::vector<std::string> fields;std::string part;while(std::getline(in,part,'|'))fields.push_back(part);
    if(fields.size()!=21||decoded.back()=='|'||fields[0]!="lowband36.v1"||fields[1]!="v6_fir_q23_q7_periodic_hann8192_fft_v1"||fields[2].size()!=64||fields[2].find_first_not_of("0123456789abcdef")!=std::string::npos||fields[9]!="V"||fields[10]!=std::string("quality_policy=")+candidate_quality_policy||fields[11]!="min_reference_rms_adc=64")return configuration;
    try{
        const auto fs=number(fields[5]);if(fs!=5000&&fs!=10000&&fs!=25000&&fs!=50000)return configuration;
        if(number(fields[3])!=accelerator::fir_taps(std::uint32_t(fs))||number(fields[4])!=fs/5000)return configuration;
        for(unsigned i=6;i<=8;++i)if(!(number(fields[i])>0))return configuration;
        for(unsigned ch=0;ch<3;++ch){const unsigned i=12+3*ch;const double sensitivity=number(fields[i]),gain=number(fields[i+1]),bias=number(fields[i+2]);const double slope=1000./gain/sensitivity;if(sensitivity<=0||gain==0||!finite(slope)||slope==0||!finite(bias*slope))return configuration;}
    }catch(const AnalysisError&){return configuration;}
    for(unsigned ch=0;ch<3;++ch){const unsigned i=12+3*ch;fields[i]="1";fields[i+1]="1";fields[i+2]="0";}
    std::string canonical;for(const auto& field:fields){if(!canonical.empty())canonical+='|';canonical+=field;}return hex(canonical);
}
bool candidate_configurations_compatible_for_v3(const std::string& a,const std::string& b){return canonical_candidate_configuration_for_v3(a)==canonical_candidate_configuration_for_v3(b);}

CandidateWindow extract_candidate_window(const Capture& c,std::size_t start,const AnalysisConfig& cfg,const std::string& setup,accelerator::Backend backend){
    validate_calibration_config(cfg);CandidateWindow w;w.setup_id=setup;w.sample_rate_hz=c.header.sample_rate_hz;w.synthetic=c.source_metadata.synthetic;w.source_path=c.source_path;w.input_kind=c.source_path=="local_contiguous_raw_chunks"?"local_full_raw_crc_continuous":"committed_bin";w.capture_sha256=c.capture_sha256;if(c.header.stream_segment)w.source_session_id=hex(std::string(reinterpret_cast<const char*>(c.header.session_id.data()),c.header.session_id.size()));
    const auto n=accelerator::required_raw_frames(w.sample_rate_hz);need(start<=c.records.size()&&n<=c.records.size()-start,"window needs complete FIR history plus 8192 lowband points");
    accelerator::Request request;request.sample_rate_hz=w.sample_rate_hz;request.raw.reserve(n);request.window_seq=c.records[start].index;w.first_index=c.records[start].index;w.last_index=c.records[start+n-1].index;
    const auto centerFirst=(accelerator::fir_taps(w.sample_rate_hz)-1)/2;
    const auto centerLast=centerFirst+8191*(w.sample_rate_hz/5000);
    w.analysis_first_index=c.records[start+centerFirst].index;w.analysis_last_index=c.records[start+centerLast].index;
    if(!c.integrity_verified)w.reasons.push_back("input_integrity_unverified");
    if(w.synthetic)w.reasons.push_back("synthetic_not_machine_evidence");
    if(c.source_metadata.status=="invalid_or_unreadable_sidecar")w.reasons.push_back("source_metadata_invalid");
    if(cfg.signal_kind!=SignalKind::VibrationConfirmed)w.reasons.push_back("mechanical_source_not_confirmed");
    if(setup.empty())w.reasons.push_back("setup_id_missing");
    const double ppr=c.header.pulses_per_rev?c.header.pulses_per_rev:cfg.ppr_override.value_or(0);if(!(ppr>0))w.reasons.push_back("ppr_missing");
    if(cfg.amplitude_unit!=AmplitudeUnit::Voltage&&!cfg.calibration_confirmed)w.reasons.push_back("acceleration_calibration_unconfirmed");
    std::vector<double> speeds;unsigned valid=0;const auto tickStep=c.header.clock_hz/w.sample_rate_hz;Sha256 windowSha;
    for(std::size_t i=0;i<n;++i){const auto&r=c.records[start+i];request.raw.push_back(r.raw);
        std::array<std::uint8_t,32> encoded{};auto put=[&](unsigned offset,std::uint64_t value,unsigned bytes){for(unsigned b=0;b<bytes;++b)encoded[offset+b]=std::uint8_t(value>>(8*b));};put(0,r.index,8);put(8,r.tick,8);for(unsigned ch=0;ch<3;++ch)put(16+ch*2,std::uint16_t(r.raw[ch]),2);put(22,r.flags,2);put(24,r.speed_period_ticks,4);put(28,r.speed_edge_count,4);windowSha.update(encoded.data(),encoded.size());
        if(!(r.flags&1u)||(r.flags&~31u)||(i&&(r.index!=c.records[start+i-1].index+1||r.tick!=c.records[start+i-1].tick+tickStep))) {w.reasons.push_back("raw_sequence_or_tick_gap");break;}
        const bool speed=(r.flags&2u)&&!(r.flags&4u)&&r.speed_period_ticks>0;
        if(i>=centerFirst&&i<=centerLast){if(speed)++valid;
            if(i&&speed&&ppr>0&&std::uint32_t(r.speed_edge_count-c.records[start+i-1].speed_edge_count)==1)speeds.push_back(60.*c.header.clock_hz/(ppr*r.speed_period_ticks));}
    }
    if(request.raw.size()!=n){w.source=accelerator::backend_name(backend);return w;}
    w.raw_window_sha256=windowSha.hex_digest();
    if(valid<double(centerLast-centerFirst+1)*.9)w.reasons.push_back("speed_valid_fraction_below_90pct");
    w.fresh_periods=unsigned(speeds.size());if(speeds.size()<8)w.reasons.push_back("fewer_than_8_fresh_speed_periods");
    if(!speeds.empty()){w.rpm_min=*std::min_element(speeds.begin(),speeds.end());w.rpm_max=*std::max_element(speeds.begin(),speeds.end());w.rpm=std::clamp(std::accumulate(speeds.begin(),speeds.end(),0.)/speeds.size(),w.rpm_min,w.rpm_max);double v=0;for(double x:speeds)v+=(x-w.rpm)*(x-w.rpm);w.speed_cv=std::sqrt(v/speeds.size())/w.rpm;if(w.speed_cv>.05)w.reasons.push_back("speed_cv_above_5pct");}
    const double df=5000./8192;const double orders[]{.5,1,1.5,2,2.5,3,4,5,6,7,8,9,10};bool bands=!speeds.empty();std::set<unsigned> unionBins;
    const unsigned refFirst=unsigned(std::ceil(2/df)),refLast=unsigned(std::floor(500/df));
    for(unsigned i=0;i<13;++i){const int first=int(std::ceil(orders[i]*w.rpm_min/60/df-1.5)),last=int(std::floor(orders[i]*w.rpm_max/60/df+1.5));if(first<int(refFirst)||last>int(refLast)||first>last){bands=false;continue;}request.order_bands[i]={unsigned(first),unsigned(last)};for(int k=first;k<=last;++k)if(!unionBins.insert(unsigned(k)).second)bands=false;}
    request.bands_valid=bands;if(!bands)w.reasons.push_back("required_order_bands_overlap_or_outside_2_500Hz");
    const auto a=accelerator::run(request,backend);w.source=a.source;w.elapsed_ms=a.elapsed_ms;
    std::ostringstream identity;identity.imbue(std::locale::classic());identity<<std::setprecision(17)<<"lowband36.v1|"<<a.algorithm_version<<'|'<<a.coefficients_sha256<<'|'<<a.fir_taps<<'|'<<a.decimation<<'|'<<w.sample_rate_hz<<'|'<<c.header.adc_range_uv<<'|'<<c.header.channel_map<<'|'<<ppr<<'|'<<amplitude_unit_symbol(cfg);
    identity<<"|quality_policy="<<candidate_quality_policy<<"|min_reference_rms_adc="<<candidate_minimum_reference_rms_adc;
    for(unsigned ch=0;ch<3;++ch){const auto cal=effective_channel_calibration(cfg,ch);identity<<'|'<<cal.sensitivity_mv_per_m_s2<<'|'<<cal.analog_gain<<'|'<<cal.voltage_bias_v;}
    w.configuration=hex(identity.str());if(!a.valid||a.saturation_count)w.reasons.push_back("accelerator_invalid_or_saturated");
    for(unsigned ch=0;ch<3;++ch){const auto&s=a.channels[ch];if(a.raw_endpoint_count[ch])w.reasons.push_back("raw_adc_clipping_CH"+std::to_string(ch+1));
        need(s.lowband_q8.size()==8192&&s.power_adc2.size()==4097,"incomplete accelerator window");
        const double adcV=double(c.header.adc_range_uv)*1e-6/32768;const double gain=voltage_to_amplitude(adcV,ch,cfg)-voltage_to_amplitude(0,ch,cfg);double mean=0;for(auto x:s.lowband_q8)mean+=x/256.;mean/=8192;double m2=0,m4=0,peak=0;
        for(auto x:s.lowband_q8){const double d=x/256.-mean;m2+=d*d;m4+=d*d*d*d;peak=std::max(peak,std::abs(d));}m2/=8192;m4/=8192;
        auto*x=w.features.data()+ch*12;x[0]=std::sqrt(m2)*std::abs(gain);if(m2<1e-6){w.reasons.push_back("flatline_or_below_0.001_ADC_rms_CH"+std::to_string(ch+1));continue;}x[1]=peak/std::sqrt(m2);x[2]=m4/(m2*m2);
        double ref=0;for(unsigned k=refFirst;k<=refLast;++k)ref+=s.power_adc2[k];
        w.reference_rms_adc[ch]=finite(ref)&&ref>=0?std::sqrt(ref):0;
        // A nearly zero denominator is not the only unreliable case. Require
        // a fixed resolution margin in the actual 2..500 Hz denominator band,
        // even when a strong out-of-band tone raises full-window AC RMS.
        // No low-reference channel is silently omitted from the 36-D model.
        if(!(ref>=candidate_minimum_reference_rms_adc*candidate_minimum_reference_rms_adc)){
            w.reasons.push_back("reference_band_rms_below_policy_CH"+std::to_string(ch+1));continue;
        }
        std::array<double,13> powers{};if(bands)for(unsigned b=0;b<13;++b)for(unsigned k=request.order_bands[b].first;k<=request.order_bands[b].last;++k)powers[b]+=s.power_adc2[k];
        x[3]=std::sqrt(powers[1])*std::abs(gain);for(unsigned b=0;b<6;++b)x[4+b]=powers[b]/ref;double harmonics=0,total=0;for(unsigned b=0;b<13;++b){total+=powers[b];if(b>=6)harmonics+=powers[b];}x[10]=harmonics/ref;x[11]=std::max(0.,(ref-total)/ref);
        w.ratio_features_valid[ch]=bands;
    }
    for(double x:w.features)if(!finite(x)){w.reasons.push_back("nonfinite_feature");break;}
    std::sort(w.reasons.begin(),w.reasons.end());w.reasons.erase(std::unique(w.reasons.begin(),w.reasons.end()),w.reasons.end());w.valid=w.reasons.empty();return w;
}

CandidateModel fit_normal_baseline(const std::vector<CandidateWindow>& windows,const std::string& id){
    need(windows.size()>=5,"at least five disjoint, quality-valid normal windows are required");CandidateModel m;m.id=id;m.setup_id=windows.front().setup_id;m.configuration=windows.front().configuration;m.fs=windows.front().sample_rate_hz;m.baseline_count=unsigned(windows.size());m.rpm_min=1e100;
    std::uint64_t previous=0;bool first=true;
    for(const auto&w:windows){need(w.valid&&!w.synthetic&&w.setup_id==m.setup_id&&w.configuration==m.configuration,"normal windows invalid or configurations differ");need(first||w.first_index>previous,"baseline windows must be disjoint, ordered raw supports");first=false;previous=w.last_index;m.rpm_min=std::min(m.rpm_min,w.rpm_min);m.rpm_max=std::max(m.rpm_max,w.rpm_max);for(unsigned i=0;i<36;++i)m.mean[i]+=w.features[i]/windows.size();}
    need(m.rpm_max/m.rpm_min<=1.15,"baseline spans more than one speed operating point");
    for(const auto&w:windows)for(unsigned i=0;i<36;++i)m.scale[i]+=(w.features[i]-m.mean[i])*(w.features[i]-m.mean[i]);
    for(unsigned i=0;i<36;++i)m.scale[i]=std::max({std::sqrt(m.scale[i]/(windows.size()-1)),std::abs(m.mean[i])*.10,i%12==0||i%12==3?1e-9:.01});
    m.rpm_min*=.9;m.rpm_max*=1.1;validate(m);return m;
}

CandidateInference infer_candidate(const CandidateWindow&w,const CandidateModel&m){
    validate(m);CandidateInference r;r.model_id=m.id;r.model_kind=m.kind;r.source=w.source;r.configuration=w.configuration;
    r.reasons=w.reasons;if(w.synthetic){r.status="quality_gated";if(std::find(r.reasons.begin(),r.reasons.end(),"synthetic_not_machine_evidence")==r.reasons.end())r.reasons.push_back("synthetic_not_machine_evidence");return r;}if(!w.valid){r.status="quality_gated";return r;}
    if(m.kind=="reference_ensemble_v2"||m.kind=="reference_ensemble_v3"){
        if(!r.reasons.empty()||w.feature_schema!="lowband36.v1"||w.quality_policy!=candidate_quality_policy){r.status="quality_gated";r.reasons.push_back("invalid_feature_or_quality_policy");return r;}
        if(!finite(w.rpm)||!finite(w.rpm_min)||!finite(w.rpm_max)||w.rpm_min<=0||w.rpm_min>w.rpm_max||w.rpm<w.rpm_min||w.rpm>w.rpm_max||!finite(w.speed_cv)||w.speed_cv<0||w.speed_cv>.05){r.status="quality_gated";r.reasons.push_back("invalid_or_unstable_speed_support");return r;}
        for(double x:w.features)if(!finite(x)||x<0){r.status="quality_gated";r.reasons.push_back("invalid_nonnegative_feature");return r;}
    }
    if((m.kind=="reference_ensemble_v3"?!candidate_configurations_compatible_for_v3(w.configuration,m.configuration):w.configuration!=m.configuration)||w.setup_id!=m.setup_id||w.sample_rate_hz!=m.fs){r.status="configuration_mismatch";r.reasons.push_back("model_setup_filter_conversion_or_sample_rate_mismatch");return r;}
    if(m.kind!="reference_ensemble_v3"&&(w.rpm_min<m.rpm_min||w.rpm_max>m.rpm_max)){r.status="outside_operating_range";r.reasons.push_back((m.kind=="reference_prototype"||m.kind=="reference_exemplars"||m.kind=="reference_ensemble_v2")?"speed_outside_reference_coverage":"speed_outside_normal_training_domain");return r;}
    if(m.kind=="reference_ensemble_v2"||m.kind=="reference_ensemble_v3")return infer_reference_ensemble(w,m,std::move(r));
    if(m.kind=="reference_exemplars"){
        std::array<std::set<std::string>,4>files;std::vector<const CandidateReferenceExemplar*>eligible;
        for(const auto&e:m.references){if((!w.capture_sha256.empty()&&w.capture_sha256==e.capture_sha256)||(!w.source_session_id.empty()&&w.source_session_id==e.session_id))r.reference_replay=true;if(w.rpm_min>0&&w.rpm_min>=e.rpm_min*.9&&w.rpm_max<=e.rpm_max*1.1){eligible.push_back(&e);files[e.class_index].insert(e.capture_sha256);}}
        bool complete=true;std::ostringstream coverage;coverage<<"RPM-local distinct BIN counts normal/imbalance/misalignment/looseness=";for(unsigned c=0;c<4;++c){r.reference_eligible_files[c]=unsigned(files[c].size());complete=complete&&!files[c].empty();if(c)coverage<<'/';coverage<<files[c].size();}r.evidence.push_back(coverage.str());
        if(r.reference_replay)r.evidence.push_back("reference data replay or same acquisition session; not an independent validation result");
        if(!complete){r.status="coverage_insufficient";r.reasons.push_back("reference_class_coverage_missing_at_current_speed");return r;}
        std::array<double,36>lo,hi,localScale;lo.fill(std::numeric_limits<double>::max());hi.fill(-std::numeric_limits<double>::max());for(const auto*e:eligible)for(unsigned j=0;j<36;++j){lo[j]=std::min(lo[j],e->features[j]);hi[j]=std::max(hi[j],e->features[j]);}for(unsigned j=0;j<36;++j)localScale[j]=std::max({(hi[j]-lo[j])*.1,std::max(std::abs(lo[j]),std::abs(hi[j]))*.01,j%12==0||j%12==3?1e-9:.01});
        std::array<double,4>nearest;nearest.fill(std::numeric_limits<double>::max());const double rpm=(w.rpm_min+w.rpm_max)/2;
        for(const auto*e:eligible){double distance=0;for(unsigned j=0;j<36;++j){const double delta=(w.features[j]-e->features[j])/localScale[j];distance+=delta*delta;}const double center=(e->rpm_min+e->rpm_max)/2;const double speedDistance=(rpm-center)/(center*.1);distance=std::sqrt(distance/36+speedDistance*speedDistance);nearest[e->class_index]=std::min(nearest[e->class_index],distance);}
        r.distance=*std::min_element(nearest.begin(),nearest.end());for(unsigned c=0;c<4;++c)r.scores[c]=100*std::exp(std::max(-700.,-.5*nearest[c]*nearest[c]));r.evidence.push_back("RPM-local nearest reference matching; file speed groups retained, scores uncalibrated, class sample counts may be unequal; not independently validated");r.evidence.push_back("reference_provenance_sha256="+m.reference_provenance_sha256);
        std::array<unsigned,4>order{0,1,2,3};std::sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return r.scores[a]>r.scores[b];});r.margin=r.scores[order[0]]-r.scores[order[1]];if(r.scores[order[0]]<m.minimum_score||r.margin<m.minimum_margin||r.distance>m.maximum_distance){r.status="rejected_unknown";r.reasons.push_back("insufficient_score_margin_or_out_of_distribution");return r;}r.status="provisional_candidate";r.classification=labels[order[0]];return r;
    }
    std::array<double,36>z{};for(unsigned i=0;i<36;++i){z[i]=(w.features[i]-m.mean[i])/m.scale[i];r.distance+=z[i]*z[i];}r.distance=std::sqrt(r.distance/36);
    if(m.kind=="reference_prototype"){
        r.distance=std::numeric_limits<double>::max();
        for(unsigned c=0;c<4;++c){double d=0;for(unsigned i=0;i<36;++i){const double delta=z[i]-m.weights[c*36+i];d+=delta*delta;}d=std::sqrt(d/36);r.distance=std::min(r.distance,d);r.scores[c]=100*std::exp(std::max(-700.,-.5*d*d));}
        r.evidence.push_back("four-class equally weighted file-reference matching; not independently validated; similarity scores are not probabilities");
        r.evidence.push_back("reference_provenance_sha256="+m.reference_provenance_sha256);
    }else if(m.kind=="linear_softmax"){
        std::array<double,4>logits=m.bias;for(unsigned c=0;c<4;++c)for(unsigned i=0;i<36;++i)logits[c]+=m.weights[c*36+i]*z[i];const auto top=*std::max_element(logits.begin(),logits.end());double sum=0;for(unsigned c=0;c<4;++c){r.scores[c]=std::exp(std::clamp(logits[c]-top,-700.,0.));sum+=r.scores[c];}for(double&x:r.scores)x=100*x/sum;r.evidence.push_back("linear model discriminant scores are not calibrated probabilities");
    }else{
        double rmsRatio=1,oneRise=0,twoRise=0,oneFraction=0,twoFraction=0,loose=0;
        for(unsigned ch=0;ch<3;++ch){auto i=ch*12;auto ratio=[&](unsigned k){return w.features[i+k]/std::max(m.mean[i+k],1e-9);};rmsRatio=std::max(rmsRatio,ratio(0));oneRise=std::max(oneRise,ratio(3));twoRise=std::max(twoRise,w.features[i+7]/std::max(m.mean[i+7],.01));oneFraction=std::max(oneFraction,w.features[i+5]);twoFraction=std::max(twoFraction,w.features[i+7]);loose=std::max(loose,clip((w.features[i+4]+w.features[i+6]+w.features[i+8]+w.features[i+10]-.05)/.30)*.65+clip((ratio(2)-1)/2)*.2+clip((ratio(1)-1)/1.5)*.15);}
        const double abnormal=clip((r.distance-1.5)/3);r.scores[0]=100*(1-clip((r.distance-1)/3));r.scores[1]=100*abnormal*(.6*clip((oneRise-1)/1.5)+.4*clip(oneFraction/.65))*(1-.6*clip(twoFraction/.35));r.scores[2]=100*abnormal*(.55*clip((twoRise-1)/3)+.45*clip(twoFraction/.35));r.scores[3]=100*abnormal*loose;
        std::ostringstream e;e<<std::setprecision(5)<<"normal standardized distance="<<r.distance<<", maximum RMS ratio="<<rmsRatio<<", 1X RMS ratio="<<oneRise<<", 2X power ratio="<<twoRise;r.evidence.push_back(e.str());r.evidence.push_back("engineering-prior pattern scores plus measured normal baseline; no fault-label training has occurred");
    }
    std::array<unsigned,4>order{0,1,2,3};std::sort(order.begin(),order.end(),[&](unsigned a,unsigned b){return r.scores[a]>r.scores[b];});r.margin=r.scores[order[0]]-r.scores[order[1]];
    if(r.scores[order[0]]<m.minimum_score||r.margin<m.minimum_margin||((m.kind=="linear_softmax"||m.kind=="reference_prototype")&&r.distance>m.maximum_distance)){r.status="rejected_unknown";r.reasons.push_back("insufficient_score_margin_or_out_of_distribution");return r;}
    r.status="provisional_candidate";r.classification=labels[order[0]];return r;
}

std::string candidate_model_text(const CandidateModel&m){
    validate(m);const bool ensemble=m.kind=="reference_ensemble_v2"||m.kind=="reference_ensemble_v3";
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17)<<"schema="<<m.schema<<"\nid="<<m.id<<"\nsetup_id="<<m.setup_id<<"\nstatus="<<m.status<<"\nkind="<<m.kind<<"\nconfiguration="<<m.configuration<<"\nfs="<<m.fs<<"\nfeature_count=36\nbaseline_count="<<m.baseline_count<<"\nrpm_min="<<m.rpm_min<<"\nrpm_max="<<m.rpm_max<<"\nminimum_score="<<m.minimum_score<<"\nminimum_margin="<<m.minimum_margin<<"\nmaximum_distance="<<m.maximum_distance<<"\nmean=";numbers(o,m.mean);o<<"\nscale=";numbers(o,m.scale);o<<"\nbias=";numbers(o,m.bias);o<<"\nweights=";numbers(o,m.weights);
    if(m.kind=="reference_prototype"||m.kind=="reference_exemplars"||ensemble){o<<"\nreference_file_counts=";numbers(o,m.reference_file_counts);o<<"\nreference_provenance_sha256="<<m.reference_provenance_sha256;}
    if(ensemble)o<<"\nreference_policy="<<m.reference_policy;
    if(m.kind=="reference_ensemble_v3")o<<"\nreference_parent_model_sha256="<<m.reference_parent_model_sha256;
    if(m.kind=="reference_exemplars"||ensemble){o<<"\nreference_count="<<m.references.size();for(std::size_t i=0;i<m.references.size();++i){const auto&e=m.references[i];o<<"\nreference_"<<i<<'='<<e.class_index<<','<<e.rpm_min<<','<<e.rpm_max<<','<<e.capture_sha256<<','<<hex(e.session_id)<<',';numbers(o,e.features);}}
    const auto text=o.str()+"\n";need(text.size()<=candidate_model_max_bytes,"model exceeds 16MiB");return text;
}
CandidateModel parse_candidate_model(const std::string&s){
    need(s.size()<=candidate_model_max_bytes,"model exceeds 16MiB");std::map<std::string,std::string>kv;std::istringstream in(s);std::string line;
    while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty()||line[0]=='#')continue;auto p=line.find('=');need(p!=std::string::npos&&p>0&&kv.emplace(line.substr(0,p),line.substr(p+1)).second,"malformed or duplicate key");}
    auto get=[&](const char*k){auto it=kv.find(k);need(it!=kv.end(),std::string("missing ")+k);auto v=it->second;kv.erase(it);return v;};
    CandidateModel m;m.schema=get("schema");m.id=get("id");m.setup_id=get("setup_id");m.status=get("status");m.kind=get("kind");m.configuration=get("configuration");
    auto integer=[&](const char*k){const auto x=number(get(k));need(x>=0&&x<=1000000&&std::floor(x)==x,"invalid integer");return unsigned(x);};
    m.fs=integer("fs");need(integer("feature_count")==36,"feature count must be 36");m.baseline_count=integer("baseline_count");m.rpm_min=number(get("rpm_min"));m.rpm_max=number(get("rpm_max"));m.minimum_score=number(get("minimum_score"));m.minimum_margin=number(get("minimum_margin"));m.maximum_distance=number(get("maximum_distance"));parse_numbers(get("mean"),m.mean);parse_numbers(get("scale"),m.scale);parse_numbers(get("bias"),m.bias);parse_numbers(get("weights"),m.weights);
    const bool ensemble=m.kind=="reference_ensemble_v2"||m.kind=="reference_ensemble_v3";
    if(m.kind=="reference_prototype"||m.kind=="reference_exemplars"||ensemble){parse_numbers(get("reference_file_counts"),m.reference_file_counts);m.reference_provenance_sha256=get("reference_provenance_sha256");}
    if(ensemble)m.reference_policy=get("reference_policy");
    if(m.kind=="reference_ensemble_v3")m.reference_parent_model_sha256=get("reference_parent_model_sha256");
    if(m.kind=="reference_exemplars"||ensemble){
        const auto count=integer("reference_count");need(count>0&&count<=candidate_reference_max_exemplars,"invalid exemplar count");
        for(unsigned i=0;i<count;++i){
            const auto key="reference_"+std::to_string(i);const auto row=get(key.c_str());std::istringstream record(row);std::vector<std::string>fields;std::string field;while(std::getline(record,field,','))fields.push_back(field);need(fields.size()==41,"invalid exemplar record");
            CandidateReferenceExemplar e;const double cls=number(fields[0]);need(cls>=0&&cls<4&&std::floor(cls)==cls,"invalid exemplar class");e.class_index=unsigned(cls);e.rpm_min=number(fields[1]);e.rpm_max=number(fields[2]);e.capture_sha256=fields[3];const auto&session=fields[4];need(session.size()<=256&&session.size()%2==0&&session.find_first_not_of("0123456789abcdef")==std::string::npos,"invalid exemplar session encoding");for(std::size_t k=0;k<session.size();k+=2)e.session_id+=char(std::stoul(session.substr(k,2),nullptr,16));for(unsigned j=0;j<36;++j)e.features[j]=number(fields[j+5]);m.references.push_back(std::move(e));
        }
    }
    need(kv.empty(),"unknown key");validate(m);return m;
}
CandidateModel load_candidate_model(const std::string&path){std::ifstream in(std::filesystem::u8path(path),std::ios::binary);need(bool(in),"cannot open model");std::string text(candidate_model_max_bytes+1,'\0');in.read(&text[0],std::streamsize(text.size()));text.resize(std::size_t(in.gcount()));return parse_candidate_model(text);}
std::string candidate_window_json(const CandidateWindow&w){
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17)
      <<"{\"feature_schema\":\"lowband36.v1\",\"setup_id\":"<<quote(w.setup_id)
      <<",\"configuration\":"<<quote(w.configuration)<<",\"source\":"<<quote(w.source)
      <<",\"input_kind\":"<<quote(w.input_kind)<<",\"source_path\":"<<quote(w.source_path)
      <<",\"quality_policy\":"<<quote(w.quality_policy)<<",\"noise_floor_status\":"<<quote(w.noise_floor_status)
      <<",\"minimum_reference_rms_adc\":"<<candidate_minimum_reference_rms_adc
      <<",\"raw_window_sha256\":"<<quote(w.raw_window_sha256)<<",\"capture_sha256\":"<<quote(w.capture_sha256)<<",\"source_session_id\":"<<quote(w.source_session_id)
      <<",\"fs\":"<<w.sample_rate_hz<<",\"rpm\":"<<w.rpm<<",\"rpm_min\":"<<w.rpm_min<<",\"rpm_max\":"<<w.rpm_max
      <<",\"speed_cv\":"<<w.speed_cv<<",\"fresh_periods\":"<<w.fresh_periods
      <<",\"first_index\":"<<w.first_index<<",\"last_index\":"<<w.last_index
      <<",\"analysis_first_index\":"<<w.analysis_first_index<<",\"analysis_last_index\":"<<w.analysis_last_index
      <<",\"quality\":"<<quote(w.valid?"valid":"invalid")<<",\"synthetic\":"<<(w.synthetic?"true":"false")
      <<",\"label\":\"unlabeled\",\"group_id\":\"\",\"run_id\":\"\",\"features\":[";
    numbers(o,w.features);o<<"],\"reference_rms_adc\":[";numbers(o,w.reference_rms_adc);
    o<<"],\"ratio_features_valid\":[";for(unsigned i=0;i<3;++i){if(i)o<<',';o<<(w.ratio_features_valid[i]?"true":"false");}
    o<<"],\"reasons\":[";for(std::size_t i=0;i<w.reasons.size();++i){if(i)o<<',';o<<quote(w.reasons[i]);}o<<"]}";return o.str();
}
std::string candidate_inference_json(const CandidateInference&r){
    std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17)<<"{\"schema\":"<<quote(r.schema)<<",\"model_id\":"<<quote(r.model_id)<<",\"model_kind\":"<<quote(r.model_kind)<<",\"status\":"<<quote(r.status)<<",\"classification\":"<<quote(r.classification)<<",\"validated\":false,\"reference_replay\":"<<(r.reference_replay?"true":"false")<<",\"reference_eligible_files\":["<<r.reference_eligible_files[0]<<','<<r.reference_eligible_files[1]<<','<<r.reference_eligible_files[2]<<','<<r.reference_eligible_files[3]<<"],\"source\":"<<quote(r.source)<<",\"configuration\":"<<quote(r.configuration)<<",\"distance\":"<<r.distance<<",\"margin\":"<<r.margin<<",\"scores\":[";numbers(o,r.scores);o<<']';
    if(r.model_kind=="reference_ensemble_v2"||r.model_kind=="reference_ensemble_v3"){o<<",\"local_feature_scale\":[";numbers(o,r.local_feature_scale);o<<']';}
    if(r.model_kind=="reference_ensemble_v3")o<<",\"speed_support_status\":"<<quote(r.speed_support_status)<<",\"reference_speed_distance\":"<<r.reference_speed_distance<<",\"speed_support_factor\":"<<r.speed_support_factor<<",\"shape_support_status\":"<<quote(r.shape_support_status)<<",\"reference_second_distance\":"<<r.reference_second_distance;
    o<<",\"evidence\":[";for(std::size_t i=0;i<r.evidence.size();++i){if(i)o<<',';o<<quote(r.evidence[i]);}o<<"],\"reasons\":[";for(std::size_t i=0;i<r.reasons.size();++i){if(i)o<<',';o<<quote(r.reasons[i]);}return o.str()+"]}";
}
} // namespace vibration
