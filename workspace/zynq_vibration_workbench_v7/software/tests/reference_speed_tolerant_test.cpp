#include "vibration/reference_validation.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
using namespace vibration;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,const char* message){check(std::abs(a-b)<=1e-10*std::max({1.,std::abs(a),std::abs(b)}),message);}
template<class F>void rejects(F f,const char* message){bool rejected=false;try{f();}catch(const AnalysisError&){rejected=true;}check(rejected,message);}
std::string hash(const std::string& s){return sha256_hex(reinterpret_cast<const std::uint8_t*>(s.data()),s.size());}
std::string hex(const std::string& s){constexpr const char* digits="0123456789abcdef";std::string r;for(unsigned char c:s){r+=digits[c>>4];r+=digits[c&15];}return r;}
std::string voltage_configuration(const std::string& calibration="10|-1|0|10|-1|0|10|-1|0",const std::string& unit="V"){
    return hex("lowband36.v1|v6_fir_q23_q7_periodic_hann8192_fft_v1|"+hash("mathematical-coefficients")+"|"+std::to_string(accelerator::fir_taps(5000))+"|1|5000|5000000|328449|1|"+unit+"|quality_policy=reference_adc_rms64_v1|min_reference_rms_adc=64|"+calibration);
}
bool reason(const CandidateInference& r,const std::string& s){return std::find(r.reasons.begin(),r.reasons.end(),s)!=r.reasons.end();}
// Mathematical post-extraction fixtures. Neither invented features nor their
// fabricated identities are measurements or machine-accuracy observations.
ReferenceSample sample(unsigned cls,unsigned identity,double rpm=1200){
    const char* labels[]{"normal","imbalance","misalignment","looseness"};
    const double ratios[4][8]={{.02,.30,.02,.08,.02,.04,.07,.45},{.01,.78,.01,.03,.01,.02,.03,.11},{.02,.15,.02,.50,.03,.10,.08,.10},{.12,.18,.10,.13,.08,.08,.20,.11}};
    const double crest[]{2,1.5,2.2,4},kurtosis[]{3,2,3.5,7};
    ReferenceSample s;s.path="speed-mathematical-only-"+std::to_string(identity)+".BIN";s.label=labels[cls];s.capture_sha256=hash(s.path);s.session_id="speed-math-session-"+std::to_string(identity);s.operator_updated_at="mathematical-fixture-only";
    CandidateWindow w;w.setup_id="mathematical-speed-setup";w.configuration="abcd1234";w.sample_rate_hz=5000;w.valid=true;w.fresh_periods=30;w.first_index=0;w.last_index=9999;w.rpm=w.rpm_min=w.rpm_max=rpm;w.source_path=s.path;w.source="post-extraction-mathematical-fixture";w.capture_sha256=s.capture_sha256;w.source_session_id=s.session_id;w.raw_window_sha256=hash(s.path+"-window");
    for(unsigned ch=0;ch<3;++ch){auto* f=w.features.data()+12*ch;f[0]=1+.1*cls;f[1]=crest[cls];f[2]=kurtosis[cls];f[3]=.5+.05*cls;for(unsigned j=0;j<8;++j)f[4+j]=ratios[cls][j];}
    s.windows.push_back(w);return s;
}
std::vector<ReferenceSample> library(){std::vector<ReferenceSample> s;for(unsigned c=0;c<4;++c)for(unsigned i=0;i<2;++i)s.push_back(sample(c,c*10+i));return s;}
CandidateWindow query(CandidateWindow w,double rpm=1200){w.capture_sha256=hash("independent-mathematical-query");w.source_session_id="independent-mathematical-query-session";w.rpm=w.rpm_min=w.rpm_max=rpm;return w;}
void same_features(const CandidateModel& a,const CandidateModel& b){
    check(a.references.size()==b.references.size(),"upgrade cannot create training representatives");
    for(std::size_t i=0;i<a.references.size();++i){const auto& x=a.references[i];const auto& y=b.references[i];check(x.class_index==y.class_index&&x.capture_sha256==y.capture_sha256&&x.session_id==y.session_id&&x.rpm_min==y.rpm_min&&x.rpm_max==y.rpm_max&&x.features==y.features,"upgrade must preserve all original aggregated references byte-value exactly");}
}
void replay_report(const std::string& path,const std::string& output){
    const auto old=load_candidate_model(path),model=upgrade_reference_model_to_speed_tolerant(old);same_features(old,model);
    std::ofstream out(std::filesystem::u8path(output));check(bool(out),"cannot open replay audit report");
    out<<"{\"schema\":\"zynq_vibration.v3_aggregated_reference_replay_audit.v1\",\"validated\":false,\"accuracy_measured\":false,\"scope\":\"stored_aggregated_feature_replay_and_counterfactual_RPM_policy_audit_only_not_new_raw_extraction_or_heldout_validation\",\"parent_model_sha256\":\""<<model.reference_parent_model_sha256<<"\",\"v3_reference_provenance_sha256\":\""<<model.reference_provenance_sha256<<"\",\"representatives\":[";
    bool first=true;for(const auto& e:old.references){
        CandidateWindow w;w.valid=true;w.setup_id=old.setup_id;w.configuration=old.configuration;w.sample_rate_hz=old.fs;w.rpm_min=e.rpm_min;w.rpm_max=e.rpm_max;w.rpm=(e.rpm_min+e.rpm_max)/2;w.features=e.features;w.capture_sha256=e.capture_sha256;w.source_session_id=e.session_id;w.source="stored-aggregated-reference-replay";
        const auto prior=infer_candidate(w,old),current=infer_candidate(w,model);
        auto remote=w;remote.rpm=remote.rpm_min=remote.rpm_max=2500;
        const auto changed=infer_candidate(remote,model);
        check(current.reference_replay&&changed.reference_replay&&!current.validated&&!changed.validated,"reference replay must remain labelled, never validation");
        check(changed.status!="outside_operating_range"&&changed.status!="reference_support_insufficient","V3 may reject shape/margin but cannot reject complete independent library solely at different RPM");
        if(!first)out<<',';first=false;out<<"{\"source_class_index\":"<<e.class_index<<",\"capture_sha256\":\""<<e.capture_sha256<<"\",\"measured_aggregate_rpm_min\":"<<e.rpm_min<<",\"measured_aggregate_rpm_max\":"<<e.rpm_max<<",\"v2_replay\":"<<candidate_inference_json(prior)<<",\"v3_replay\":"<<candidate_inference_json(current)<<",\"counterfactual_fixed_features_at_2500rpm_not_measured\":"<<candidate_inference_json(changed)<<'}';
    }
    // Nearest-representative selection remains piecewise. A smooth RPM penalty
    // does not promise globally smooth scores where a file switches modes.
    out<<"],\"rpm_nearest_switch_audit\":[";first=true;
    std::map<std::string,std::vector<const CandidateReferenceExemplar*>> byFile;
    for(const auto& e:old.references)byFile[e.capture_sha256].push_back(&e);
    for(auto& file:byFile){auto& reps=file.second;std::sort(reps.begin(),reps.end(),[](const auto* a,const auto* b){return a->rpm_min+a->rpm_max<b->rpm_min+b->rpm_max;});
        for(std::size_t i=1;i<reps.size();++i){const auto& e=*reps[i-1];const auto& next=*reps[i];const double midpoint=(e.rpm_min+e.rpm_max+next.rpm_min+next.rpm_max)/4.;
            CandidateWindow w;w.valid=true;w.setup_id=old.setup_id;w.configuration=old.configuration;w.sample_rate_hz=old.fs;w.features=e.features;w.capture_sha256=e.capture_sha256;w.source_session_id=e.session_id;w.source="counterfactual-nearest-RPM-switch-fixed-aggregated-features-not-measured";
            auto left=w,right=w;left.rpm=left.rpm_min=left.rpm_max=midpoint-1e-7;right.rpm=right.rpm_min=right.rpm_max=midpoint+1e-7;
            const auto a=infer_candidate(left,model),b=infer_candidate(right,model);for(double score:a.scores)check(std::isfinite(score),"RPM representative switch left scores finite");for(double score:b.scores)check(std::isfinite(score),"RPM representative switch right scores finite");
            if(!first)out<<',';first=false;out<<"{\"capture_sha256\":\""<<file.first<<"\",\"switch_midpoint_rpm\":"<<midpoint<<",\"left\":"<<candidate_inference_json(a)<<",\"right\":"<<candidate_inference_json(b)<<'}';
        }
    }out<<"]}\n";check(bool(out),"replay report write failed");
}
// A lossless whitespace/hex transport is produced by the adjacent Python
// auditor. It supplies extracted values only; operator labels never enter C++.
void window_replay_report(const std::string& path,const std::string& matrix,const std::string& output){
    const auto old=load_candidate_model(path),model=upgrade_reference_model_to_speed_tolerant(old);same_features(old,model);
    std::ifstream in(std::filesystem::u8path(matrix));std::string header;std::getline(in,header);check(header=="V3_MEASURED_FEATURE_REPLAY_V1","unknown measured-feature transport");
    std::ofstream out(std::filesystem::u8path(output));check(bool(out),"cannot open measured-window replay audit");
    out<<"{\"schema\":\"zynq_vibration.v3_measured_feature_replay_audit.v1\",\"validated\":false,\"accuracy_measured\":false,\"scope\":\"actual_preextracted_windows_policy_replay_only_not_new_blind_validation\",\"parent_model_sha256\":\""<<model.reference_parent_model_sha256<<"\",\"v3_model_sha256\":\""<<hash(candidate_model_text(model))<<"\",\"windows\":[";
    std::string line;bool first=true;unsigned count=0;while(std::getline(in,line)){
        check(++count<=8192,"replay audit exceeds 8192 windows");std::istringstream row(line);CandidateWindow w;
        row>>w.sample_rate_hz>>w.valid>>w.synthetic>>w.first_index>>w.last_index>>w.analysis_first_index>>w.analysis_last_index>>w.rpm>>w.rpm_min>>w.rpm_max>>w.speed_cv>>w.fresh_periods>>w.elapsed_ms;
        auto textField=[&](){std::string encoded;row>>encoded;check(bool(row),"missing encoded window field");if(encoded=="-")return std::string();check(encoded.size()%2==0&&encoded.find_first_not_of("0123456789abcdef")==std::string::npos,"bad encoded window field");std::string decoded;for(std::size_t i=0;i<encoded.size();i+=2)decoded+=char(std::stoul(encoded.substr(i,2),nullptr,16));return decoded;};
        w.setup_id=textField();w.configuration=textField();w.source=textField();w.input_kind=textField();w.source_path=textField();w.raw_window_sha256=textField();w.capture_sha256=textField();w.source_session_id=textField();w.feature_schema=textField();w.quality_policy=textField();
        for(auto& value:w.reference_rms_adc)row>>value;for(unsigned ch=0;ch<3;++ch){unsigned value=0;row>>value;check(value<=1,"bad ratio validity");w.ratio_features_valid[ch]=value;}
        for(auto& value:w.features)row>>value;unsigned reasons=0;row>>reasons;check(bool(row)&&reasons<=128,"bad window features or reason count");for(unsigned i=0;i<reasons;++i)w.reasons.push_back(textField());std::string extra;check(!(row>>extra),"extra measured-feature transport fields");
        const auto previous=infer_candidate(w,old),current=infer_candidate(w,model);
        check(!previous.validated&&!current.validated,"policy replay cannot become independent validation");
        if(!w.valid||w.synthetic||!w.reasons.empty())check(current.status=="quality_gated","actual invalid window must remain quality gated");
        if(!first)out<<',';first=false;out<<"{\"window\":"<<candidate_window_json(w)<<",\"v2\":"<<candidate_inference_json(previous)<<",\"v3\":"<<candidate_inference_json(current)<<'}';
    }check(count>0,"no measured windows to audit");out<<"]}\n";check(bool(out),"measured-window audit write failed");
}
}
int main(int argc,char** argv){try{
    const auto s=library();const auto model=fit_reference_model(s);const auto old=fit_reference_model(s,ReferenceFitPolicy::RobustEnsemble);
    check(model.kind=="reference_ensemble_v3"&&model.schema=="zynq_vibration.candidate_model.v3"&&model.reference_policy==reference_speed_tolerant_policy,"default new fit must use explicit V3 identity");
    check(old.kind=="reference_ensemble_v2"&&old.reference_policy==reference_ensemble_policy,"explicit V2 keeps its original policy identity");
    const auto oldText=candidate_model_text(old),newText=candidate_model_text(model);
    check(candidate_model_text(parse_candidate_model(oldText))==oldText&&candidate_model_text(parse_candidate_model(newText))==newText,"old and new model formats roundtrip without reinterpretation");
    const auto upgraded=upgrade_reference_model_to_speed_tolerant(old);same_features(old,upgraded);
    check(upgraded.rpm_min==1200&&upgraded.rpm_max==1200&&model.rpm_min==1200&&model.rpm_max==1200,"V3 new fit and migration report actual exemplar RPM envelope, never old padded coverage");
    check(upgraded.reference_parent_model_sha256==hash(oldText)&&upgraded.reference_provenance_sha256!=old.reference_provenance_sha256,"explicit upgrade binds canonical old model SHA and distinct policy provenance");
    check(candidate_model_text(old)==oldText,"upgrade cannot mutate old artifact");
    rejects([&]{upgrade_reference_model_to_speed_tolerant(model);},"already V3 cannot be silently upgraded again");
    rejects([&]{upgrade_reference_model_to_speed_tolerant(fit_reference_model(s,ReferenceFitPolicy::LegacyNearest));},"legacy V1 cannot masquerade as V2 aggregation migration");
    auto malformed=model;malformed.reference_policy=reference_ensemble_policy;rejects([&]{candidate_model_text(malformed);},"V3 policy identity cannot silently use V2");
    malformed=model;malformed.schema="zynq_vibration.candidate_model.v2";rejects([&]{candidate_model_text(malformed);},"V3 cannot masquerade as schema V2");
    malformed=model;malformed.reference_parent_model_sha256="wrong";rejects([&]{candidate_model_text(malformed);},"invalid parent digest rejected");
    auto reordered=s;std::reverse(reordered.begin(),reordered.end());check(candidate_model_text(fit_reference_model(reordered))==newText,"new fit/provenance independent of library enumeration order");
    for(unsigned c=0;c<4;++c){
        for(double rpm:{1080.,1200.,1320.,1351.,2500.}){
            const auto r=infer_candidate(query(s[2*c].windows[0],rpm),model);
            check(r.classification==s[2*c].label&&r.status=="provisional_candidate"&&!r.validated&&!r.reference_replay,"strong mathematical order shape remains only a provisional candidate across RPM");
            check(r.reference_eligible_files==std::array<unsigned,4>{2,2,2,2},"all independent files remain eligible without RPM exclusion");
            check(r.distance<=reference_speed_maximum_penalty&&r.speed_support_factor>0&&r.speed_support_factor<=1,"smooth RPM penalty bounded and engineering speed similarity finite");
            if(rpm==2500)check(r.speed_support_status=="extrapolated"&&reason(r,"cross_speed_reference_support_limited_requires_machine_validation"),"remote speed explicitly cautioned, never independently validated");
        }
        const auto r=infer_candidate(query(s[2*c].windows[0],2500),old);check(r.status=="outside_operating_range"&&r.classification=="unknown","V2 old hard RPM domain behavior unchanged");
    }
    // The old +/-10% endpoints and 25% caution transition cannot create score
    // cliffs. This controls a fixed post-extraction shape, not a real spin-up.
    for(double boundary:{1080.,1320.,1500.}){
        const auto a=infer_candidate(query(s[0].windows[0],std::nextafter(boundary,0.)),model),b=infer_candidate(query(s[0].windows[0],std::nextafter(boundary,1e6)),model);
        near(a.distance,b.distance,"RPM endpoint distance remains continuous");for(unsigned c=0;c<4;++c)near(a.scores[c],b.scores[c],"RPM endpoint scores remain continuous");
    }
    auto amplified=query(s[2].windows[0],2500);for(unsigned ch=0;ch<3;++ch){amplified.features[12*ch]*=4;amplified.features[12*ch+3]*=4;}
    check(infer_candidate(amplified,model).classification=="imbalance","moderate amplitude changes assist rather than override a strongly matching shape; no RPM normalization inferred");
    for(unsigned ch=0;ch<3;++ch){amplified.features[12*ch]*=1e9;amplified.features[12*ch+3]*=1e9;}
    check(infer_candidate(amplified,model).classification=="unknown","extreme amplitude OOD remains rejectable despite shape similarity");
    auto alien=query(s[0].windows[0],2500);alien.features.fill(1e12);check(infer_candidate(alien,model).status=="rejected_unknown","strong shape OOD remains unknown at remote RPM");
    auto ambiguous=s;for(auto& file:ambiguous)file.windows[0].features=s[0].windows[0].features;
    check(infer_candidate(query(ambiguous[0].windows[0],2500),fit_reference_model(ambiguous)).classification=="unknown","overlapping normal and fault shapes still fail the hard score margin");
    // A distinct within-class damage mode is allowed without a severity label.
    // Its one nearby reference cannot imply repeated confirmation.
    auto modes=s;for(unsigned ch=0;ch<3;++ch){modes[1].windows[0].features[12*ch+1]=10;modes[1].windows[0].features[12*ch+2]=30;}
    const auto modal=fit_reference_model(modes);const auto modeResult=infer_candidate(query(modes[1].windows[0]),modal);
    check(modeResult.classification=="normal"&&modeResult.shape_support_status=="single_reference_mode"&&modeResult.reference_second_distance>modal.maximum_distance&&reason(modeResult,"single_reference_mode_match_requires_independent_confirmation"),"a class may contain separated modes; second mode cannot make every query unreachable");
    auto outlier=sample(3,1000);outlier.windows[0].features=s[0].windows[0].features;auto attacked=s;attacked.push_back(outlier);
    check(infer_candidate(query(s[0].windows[0]),fit_reference_model(attacked)).classification=="unknown","an isolated wrong-class shape overlapping another class cannot be accepted unconditionally");
    auto sparse=s;sparse.erase(sparse.begin());check(infer_candidate(query(s[0].windows[0]),fit_reference_model(sparse)).status=="reference_support_insufficient","library minimum remains two independent acquisitions per class");
    auto missing=s;for(auto& file:missing){file.session_id.clear();file.windows[0].source_session_id.clear();}check(infer_candidate(query(s[0].windows[0]),fit_reference_model(missing)).reference_eligible_files==std::array<unsigned,4>{0,0,0,0},"missing sessions cannot fabricate independent library breadth");
    auto repeated=s;repeated[1].session_id=repeated[0].session_id;repeated[1].windows[0].source_session_id=repeated[0].session_id;rejects([&]{fit_reference_model(repeated);},"same-session segments remain non-independent");
    auto replay=s[0].windows[0];check(infer_candidate(replay,model).reference_replay,"training/reference replay remains labelled");replay=query(replay);replay.source_session_id=s[0].session_id;check(infer_candidate(replay,model).reference_replay,"same-session replay remains labelled");
    const auto good=infer_candidate(query(s[0].windows[0]),model);auto bad=query(s[0].windows[0]);bad.features.fill(1e9);const auto badResult=infer_candidate(bad,model);check(good.local_feature_scale==badResult.local_feature_scale,"query features cannot fit their own scale");
    auto extraSpeeds=s;for(auto& file:extraSpeeds){auto remote=file.windows[0];remote.first_index=10000;remote.last_index=19999;remote.rpm=remote.rpm_min=remote.rpm_max=2000;remote.features.fill(1e8);remote.raw_window_sha256=hash(file.path+"-remote-speed-group");file.windows.push_back(remote);}
    const auto speedGroups=fit_reference_model(extraSpeeds);const auto close=infer_candidate(query(s[0].windows[0],1208),speedGroups);
    check(close.local_feature_scale==good.local_feature_scale,"only the nearest RPM representative per file contributes scaling, even without RPM exclusion");
    auto featureAttack=query(s[0].windows[0],1208);featureAttack.features.fill(1e8);check(infer_candidate(featureAttack,speedGroups).local_feature_scale==good.local_feature_scale,"query shape cannot select a different speed representative");
    bad=query(s[0].windows[0]);bad.configuration="abcd1235";check(infer_candidate(bad,model).status=="configuration_mismatch","filter/calibration/setup identity remains hard checked");
    bad=query(s[0].windows[0]);bad.speed_cv=.051;check(infer_candidate(bad,model).status=="quality_gated","RPM tolerance does not bypass speed instability quality policy");
    bad=query(s[0].windows[0]);bad.features[0]=std::numeric_limits<double>::quiet_NaN();check(infer_candidate(bad,model).status=="quality_gated","nonfinite feature cannot bypass OOD");
    bad=query(s[0].windows[0]);bad.reasons={"raw_adc_clipping_CH1"};check(infer_candidate(bad,model).status=="quality_gated","quality reasons cannot be overridden by valid flag");
    bad=query(s[0].windows[0]);bad.synthetic=true;check(infer_candidate(bad,model).status=="quality_gated","synthetic source never becomes machine evidence");
    check(candidate_inference_json(good).find("\"shape_support_status\":")!=std::string::npos&&candidate_inference_json(infer_candidate(query(s[0].windows[0]),old)).find("shape_support_status")==std::string::npos,"V3 audit extension does not change old-kind result fields");
    const auto va=voltage_configuration(),vb=voltage_configuration("20|2|.1|30|-3|-.2|40|4|.3");
    const auto canonical=canonical_candidate_configuration_for_v3(va);
    check(va!=vb&&canonical==canonical_candidate_configuration_for_v3(vb)&&canonical_candidate_configuration_for_v3(canonical)==canonical,"known Voltage identity ignores only unused nine conversion values, idempotently");
    auto voltage=s;for(auto& file:voltage)file.windows[0].configuration=va;
    const auto vm=fit_reference_model(voltage),v2=fit_reference_model(voltage,ReferenceFitPolicy::RobustEnsemble);
    auto vq=query(voltage[0].windows[0]);vq.configuration=vb;
    check(infer_candidate(vq,vm).classification=="normal"&&infer_candidate(vq,v2).status=="configuration_mismatch","unused Voltage conversion edits are compatible only for V3");
    check(upgrade_reference_model_to_speed_tolerant(v2).configuration==canonical,"V2 Voltage upgrade canonicalizes only new model identity");
    auto mixed=voltage;mixed[1].windows[0].configuration=vb;check(fit_reference_model(mixed).configuration==canonical,"new V3 library accepts semantically identical Voltage conversions");
    rejects([&]{fit_reference_model(mixed,ReferenceFitPolicy::RobustEnsemble);},"V2 mixed configuration semantics remain strict");
    rejects([&]{evaluate_reference_models(mixed);},"comparison report refuses mixed raw configurations rather than inventing V1/V2 results; UI re-extracts all files with one current configuration");
    for(const auto& changed:{voltage_configuration("20|2|.1|30|-3|-.2|40|4|.3","m/s^2"),voltage_configuration("0|1|0|10|1|0|10|1|0"),voltage_configuration("10|0|0|10|1|0|10|1|0"),voltage_configuration("10|1|nan|10|1|0|10|1|0"),std::string("abcd1234")})check(!candidate_configurations_compatible_for_v3(va,changed),"acceleration, invalid or unknown encodings cannot bypass exact configuration checks");
    const auto aa=voltage_configuration("10|-1|0|10|-1|0|10|-1|0","m/s^2"),ab=voltage_configuration("20|-1|0|10|-1|0|10|-1|0","m/s^2");check(!candidate_configurations_compatible_for_v3(aa,ab),"acceleration sensitivity remains identity-changing");
    for(const auto& replacement:{std::pair<std::string,std::string>{"|5000000|","|2500000|"},{"|328449|","|123456|"},{"|328449|1|","|328449|2|"},{"min_reference_rms_adc=64","min_reference_rms_adc=65"},{"v6_fir_q23_q7_periodic_hann8192_fft_v1","unknown_algorithm"}}){
        std::string raw="lowband36.v1|v6_fir_q23_q7_periodic_hann8192_fft_v1|"+hash("mathematical-coefficients")+"|"+std::to_string(accelerator::fir_taps(5000))+"|1|5000|5000000|328449|1|V|quality_policy=reference_adc_rms64_v1|min_reference_rms_adc=64|20|2|.1|30|-3|-.2|40|4|.3";
        const auto where=raw.find(replacement.first);check(where!=std::string::npos,"test configuration replacement exists");raw.replace(where,replacement.first.size(),replacement.second);check(!candidate_configurations_compatible_for_v3(va,hex(raw)),"ADC/channel/PPR/quality/algorithm remain exact in Voltage compatibility");
    }
    if(argc==4&&std::string(argv[1])=="--replay")replay_report(argv[2],argv[3]);
    else if(argc==5&&std::string(argv[1])=="--windows")window_replay_report(argv[2],argv[3],argv[4]);
    else check(argc==1,"usage: reference_speed_tolerant_test [--replay saved-v2.model report.json | --windows saved-v2.model feature-matrix.txt report.json]");
    std::cout<<"PASS: V3 mathematical multi-mode/cross-RPM policy, continuous speed penalty, OOD/margin/quality/independence/replay, explicit migration and unchanged V2 behavior; no machine accuracy measured.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
