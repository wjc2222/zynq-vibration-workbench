#include "vibration/reference_library.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace vibration;
namespace {
CandidateModel fit_v2(const std::vector<ReferenceSample>& s){return vibration::fit_reference_model(s,ReferenceFitPolicy::RobustEnsemble);}
CandidateModel fit_v2(const std::vector<ReferenceSample>& s,ReferenceFitPolicy p){return vibration::fit_reference_model(s,p);}
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,const char* message){check(std::abs(a-b)<=1e-11*std::max({1.,std::abs(a),std::abs(b)}),message);}
template<class F>void rejects(F f,const char* message){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,message);}
std::string hash(const std::string& text){return sha256_hex(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());}
// Mathematical post-extraction fixtures exercise policy, never machine accuracy.
ReferenceSample sample(unsigned cls,unsigned identity,double value,double rpm=1200,unsigned windows=1){
    const char* labels[]{"normal","imbalance","misalignment","looseness"};ReferenceSample s;
    s.path="ensemble-mathematical-only-"+std::to_string(identity)+".BIN";s.label=labels[cls];s.capture_sha256=hash(s.path);s.session_id="ensemble-session-"+std::to_string(identity);s.operator_updated_at="fixture-only";
    for(unsigned i=0;i<windows;++i){CandidateWindow w;w.setup_id="mathematical-setup";w.configuration="abcd1234";w.sample_rate_hz=5000;w.capture_sha256=s.capture_sha256;w.raw_window_sha256=hash(s.path+"-window-"+std::to_string(i));w.source_path=s.path;w.source="mathematical-feature-fixture";w.source_session_id=s.session_id;w.valid=true;w.first_index=10000*i;w.last_index=w.first_index+9999;w.rpm=w.rpm_min=w.rpm_max=rpm;w.features.fill(value);s.windows.push_back(w);}return s;
}
std::vector<ReferenceSample> library(unsigned perClass=2){std::vector<ReferenceSample> s;for(unsigned c=0;c<4;++c)for(unsigned i=0;i<perClass;++i)s.push_back(sample(c,c*10+i,1+10*c));return s;}
CandidateWindow independent(CandidateWindow w){w.capture_sha256=hash("independent-query");w.source_session_id="independent-query-session";w.source_path="independent-query.BIN";return w;}
const CandidateReferenceExemplar& representative(const CandidateModel& m,const std::string& digest){for(const auto& e:m.references)if(e.capture_sha256==digest)return e;throw std::runtime_error("representative missing");}
bool reason(const CandidateInference& r,const std::string& value){return std::find(r.reasons.begin(),r.reasons.end(),value)!=r.reasons.end();}
void same_scale(const CandidateInference& a,const CandidateInference& b,const char* message){for(unsigned j=0;j<candidate_feature_count;++j)near(a.local_feature_scale[j],b.local_feature_scale[j],message);}
}
int main(){try{
    auto s=library();const auto model=fit_v2(s);
    check(model.kind=="reference_ensemble_v2"&&model.schema=="zynq_vibration.candidate_model.v2"&&model.reference_policy==reference_ensemble_policy,"explicit V2 fit retains its versioned ensemble policy");
    for(unsigned c=0;c<4;++c){const auto r=infer_candidate(independent(s[c*2].windows[0]),model);check(r.status=="provisional_candidate"&&r.classification==s[c*2].label&&!r.validated&&!r.reference_replay,"two independent mathematical supports yield only provisional four-class decisions");check(r.reference_eligible_files==std::array<unsigned,4>{2,2,2,2},"eligible counts are files per class");}
    auto query=independent(s[0].windows[0]);const auto base=infer_candidate(query,model);
    // The inverse empirical CDF is left-continuous at exact 1/4 and 3/4 mass:
    // Q25=1, Q75=21, median(abs)=11 for these equal class-weight fixtures.
    for(double scale:base.local_feature_scale)near(scale,20/1.349,"weighted quantile boundary/tie definition is stable");
    const auto encoded=candidate_model_text(model);const auto reloaded=parse_candidate_model(encoded);
    check(candidate_model_text(reloaded)==encoded,"v2 policy, provenance, all representatives and values roundtrip exactly");
    check(candidate_inference_json(base).find("\"local_feature_scale\":[")!=std::string::npos,"v2 JSON exposes reference-only scaling audit");
    const auto legacy=fit_v2(s,ReferenceFitPolicy::LegacyNearest);
    check(legacy.kind=="reference_exemplars"&&legacy.schema=="zynq_vibration.candidate_model.v1"&&legacy.reference_policy.empty(),"explicit legacy API retains old kind/schema");
    check(candidate_inference_json(infer_candidate(query,legacy)).find("local_feature_scale")==std::string::npos,"legacy result JSON remains unchanged");
    check(model.reference_provenance_sha256!=legacy.reference_provenance_sha256,"aggregation version is bound into provenance");
    auto wrongPolicy=model;wrongPolicy.reference_policy="future-unknown-policy";rejects([&]{candidate_model_text(wrongPolicy);},"unknown v2 aggregation policy fails closed");
    auto wrongSchema=model;wrongSchema.schema="zynq_vibration.candidate_model.v1";rejects([&]{candidate_model_text(wrongSchema);},"new kind cannot silently masquerade as old schema");
    auto unknownKind=model;unknownKind.kind="reference_ensemble_v4";rejects([&]{candidate_model_text(unknownKind);},"unsupported kind rejected");
    auto reversed=s;std::reverse(reversed.begin(),reversed.end());const auto reordered=fit_v2(reversed);check(candidate_model_text(model)==candidate_model_text(reordered),"file order cannot change policy, representatives or provenance");
    auto one=library(1);const auto sparse=fit_v2(one);const auto unsupported=infer_candidate(one[0].windows[0],sparse);
    check(unsupported.status=="reference_support_insufficient"&&unsupported.classification=="unknown"&&unsupported.reference_eligible_files==std::array<unsigned,4>{1,1,1,1},"one file per class is retained but never fabricates two-file certainty");
    auto noSessions=s;for(auto& file:noSessions){file.session_id.clear();for(auto& window:file.windows)window.source_session_id.clear();}
    const auto storedWithoutSessions=fit_v2(noSessions);const auto noSessionSupport=infer_candidate(query,storedWithoutSessions);
    check(storedWithoutSessions.references.size()==8&&storedWithoutSessions.reference_file_counts==std::array<double,4>{2,2,2,2},"missing-session files remain stored without fabricating acquisition identities");
    check(noSessionSupport.status=="reference_support_insufficient"&&noSessionSupport.classification=="unknown"&&noSessionSupport.reference_eligible_files==std::array<unsigned,4>{0,0,0,0}&&reason(noSessionSupport,"reference_session_identity_missing_ignored"),"two different BIN hashes with empty sessions cannot establish independent support");
    check(infer_candidate(query,fit_v2(noSessions,ReferenceFitPolicy::LegacyNearest)).classification=="normal","missing-session handling does not change legacy inference");
    auto partlyIdentified=s;for(unsigned c=0;c<4;++c){partlyIdentified[c*2].session_id.clear();partlyIdentified[c*2].windows[0].source_session_id.clear();}
    check(infer_candidate(query,fit_v2(partlyIdentified)).reference_eligible_files==std::array<unsigned,4>{1,1,1,1}&&infer_candidate(query,fit_v2(partlyIdentified)).status=="reference_support_insufficient","one identified and one unidentified BIN cannot meet two-file support");
    auto identifiedPlusUnknown=s;for(unsigned c=0;c<4;++c){auto unidentified=sample(c,1000+c,1e12);unidentified.session_id.clear();unidentified.windows[0].source_session_id.clear();identifiedPlusUnknown.push_back(unidentified);}
    const auto withUnknownSessions=infer_candidate(query,fit_v2(identifiedPlusUnknown));
    check(withUnknownSessions.status=="provisional_candidate"&&withUnknownSessions.classification=="normal"&&withUnknownSessions.reference_eligible_files==std::array<unsigned,4>{2,2,2,2}&&reason(withUnknownSessions,"reference_session_identity_missing_ignored"),"enough identified acquisitions permit inference while unidentified extras are explained and ignored");
    same_scale(base,withUnknownSessions,"missing-session files cannot change any local feature scale");for(unsigned c=0;c<4;++c)near(base.scores[c],withUnknownSessions.scores[c],"missing-session files cannot change class distances or scores");
    auto windowsOnly=one;for(auto& file:windowsOnly){auto other=file.windows[0];other.first_index=10000;other.last_index=19999;other.rpm=other.rpm_min=other.rpm_max=1250;other.raw_window_sha256=hash(file.path+"extra-rpm");file.windows.push_back(other);}
    check(infer_candidate(query,fit_v2(windowsOnly)).status=="reference_support_insufficient","multiple speed representatives from one file do not create independent support");
    auto median=s;median[0]=sample(0,0,1,1200,3);median[0].windows[2].features.fill(1e6);const auto robustMedian=fit_v2(median),oldMean=fit_v2(median,ReferenceFitPolicy::LegacyNearest);
    near(representative(robustMedian,median[0].capture_sha256).features[0],1,"within-file coordinate median resists a corrupted window");near(representative(oldMean,median[0].capture_sha256).features[0],(1e6+2)/3,"legacy arithmetic averaging remains available unchanged");
    auto multiple=s;for(unsigned c=0;c<4;++c)for(unsigned i=0;i<2;++i)multiple.push_back(sample(c,100+c*10+i,100+100*c,2000));const auto multi=fit_v2(multiple);
    check(multi.references.size()==16,"independently acquired multi-speed references retained");
    for(unsigned c=0;c<4;++c)check(infer_candidate(independent(multiple[8+c*2].windows[0]),multi).classification==multiple[8+c*2].label,"high RPM uses its own complete local library");
    same_scale(base,infer_candidate(query,multi),"remote RPM operating points do not fit local scaling");
    auto hole=query;hole.rpm=hole.rpm_min=hole.rpm_max=1600;const auto gap=infer_candidate(hole,multi);check(gap.status=="reference_support_insufficient"&&gap.reference_eligible_files==std::array<unsigned,4>{0,0,0,0},"RPM hole within global envelope is not interpolated");
    auto wide=query;wide.rpm=2000;wide.rpm_min=1750;wide.rpm_max=2199;check(infer_candidate(wide,multi).status=="reference_support_insufficient","entire query RPM interval must be covered, not only its mean");
    auto extraSpeeds=s;for(auto& file:extraSpeeds){auto other=file.windows[0];other.first_index=10000;other.last_index=19999;other.rpm=other.rpm_min=other.rpm_max=1250;other.raw_window_sha256=hash(file.path+"speed-group");other.features.fill(1e8);file.windows.push_back(other);}
    auto nearby=query;nearby.rpm=nearby.rpm_min=nearby.rpm_max=1208;const auto onlyNearest=infer_candidate(nearby,fit_v2(extraSpeeds)),withoutExtra=infer_candidate(nearby,model);
    same_scale(onlyNearest,withoutExtra,"one closest-RPM representative per file selected before seeing query features");for(unsigned c=0;c<4;++c)near(onlyNearest.scores[c],withoutExtra.scores[c],"nonselected same-file speed groups cannot cast extra votes");
    auto attackQuery=nearby;attackQuery.features.fill(1e8);same_scale(onlyNearest,infer_candidate(attackQuery,fit_v2(extraSpeeds)),"query features cannot change speed selection or scaling");
    auto imbalanced=s;for(unsigned i=0;i<80;++i)imbalanced.push_back(sample(3,200+i,31));const auto excess=infer_candidate(query,fit_v2(imbalanced));same_scale(base,excess,"class-balanced reference quantiles ignore duplicated class mass");for(unsigned c=0;c<4;++c)near(base.scores[c],excess.scores[c],"extra identical class files do not cast additional votes");
    auto outlier=imbalanced;auto bad=sample(3,500,31);bad.windows[0].features[0]=1e12;outlier.push_back(bad);const auto robustOutlier=infer_candidate(query,fit_v2(outlier));same_scale(base,robustOutlier,"one extreme reference cannot globally rescale all feature coordinates");
    auto impostor=s;for(auto& file:impostor){const auto c=reference_label_index(file.label);file.windows[0].features.fill(c==0?1:11+10*c);}impostor[2].windows[0].features.fill(1);const auto attack=infer_candidate(query,fit_v2(impostor));check(attack.classification=="normal","one corrupted wrong-class file cannot overcome two agreeing normal files");
    // No severity annotation is introduced. A separately acquired within-class
    // operating condition is retained; one such close file still cannot borrow
    // certainty from a more distant damage condition in the same class.
    auto degrees=s;auto rare=sample(1,600,11);rare.windows[0].features[0]=11+1.51*(30/1.349)*6;degrees.push_back(rare);
    const auto degreeQuery=independent(rare.windows[0]);const auto degreeOne=infer_candidate(degreeQuery,fit_v2(degrees));
    check(degreeOne.status=="rejected_unknown"&&degreeOne.classification=="unknown"&&reason(degreeOne,"insufficient_two_reference_support"),"one perfect degree match plus a too-distant second file must remain unknown");
    check(degreeOne.scores[1]>=55&&degreeOne.margin>=12&&degreeOne.distance<1.5,"second-file guard adds information beyond RMS score and margin");
    auto sameDegree=sample(1,601,11);sameDegree.windows[0].features=rare.windows[0].features;degrees.push_back(sameDegree);const auto degreeSupported=fit_v2(degrees);
    check(degreeSupported.references.size()==10&&infer_candidate(degreeQuery,degreeSupported).classification=="imbalance","separate same-class damage conditions retained; two same-condition files support a provisional decision");
    auto changedQuery=query;changedQuery.features.fill(1e6);const auto far=infer_candidate(changedQuery,model);check(far.classification=="unknown"&&far.status=="rejected_unknown","far query remains unknown");same_scale(base,far,"test query cannot fit any scale");
    check(infer_candidate(s[0].windows[0],model).reference_replay,"reference hash replay remains marked");auto sessionReplay=query;sessionReplay.source_session_id=s[0].session_id;check(infer_candidate(sessionReplay,model).reference_replay,"same-session replay remains marked");
    auto repeated=s;repeated[1].session_id=repeated[0].session_id;repeated[1].windows[0].source_session_id=repeated[0].session_id;rejects([&]{fit_v2(repeated);},"segments of one session cannot be independent files");
    auto copied=s;copied[1].capture_sha256=copied[0].capture_sha256;copied[1].windows[0].capture_sha256=copied[0].capture_sha256;rejects([&]{fit_v2(copied);},"byte-identical copies remain rejected");
    auto invalid=s;invalid[0].windows[0].features[0]=-1;rejects([&]{fit_v2(invalid);},"negative new-kind reference fails closed");invalid=s;invalid[0].windows[0].rpm_max=std::numeric_limits<double>::infinity();rejects([&]{fit_v2(invalid);},"nonfinite reference RPM fails closed");
    auto malformed=model;malformed.references[0].features[0]=-1;rejects([&]{candidate_model_text(malformed);},"negative imported representative fails closed");
    for(double value:{-1.,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){auto badQuery=query;badQuery.features[0]=value;check(infer_candidate(badQuery,model).status=="quality_gated","invalid query features fail closed");}
    auto nonfiniteSpeed=query;nonfiniteSpeed.rpm_min=std::numeric_limits<double>::quiet_NaN();check(infer_candidate(nonfiniteSpeed,model).status=="quality_gated","NaN query RPM cannot bypass coverage comparisons");
    auto mismatch=query;mismatch.configuration="abcd1235";check(infer_candidate(mismatch,model).status=="configuration_mismatch","fixed units/calibration identity mismatch remains gated");mismatch=query;mismatch.setup_id="different-installation";check(infer_candidate(mismatch,model).status=="configuration_mismatch","mounting setup remains gated");mismatch=query;mismatch.sample_rate_hz=10000;check(infer_candidate(mismatch,model).status=="configuration_mismatch","sample-rate/FIR identity remains gated");
    auto calibration=query;calibration.reasons={"acceleration_calibration_unconfirmed"};check(infer_candidate(calibration,model).status=="quality_gated","supplied valid flag cannot override calibration/source quality reasons");
    auto synthetic=query;synthetic.synthetic=true;check(infer_candidate(synthetic,model).status=="quality_gated","synthetic source cannot enter mechanical inference");
    std::cout<<"PASS: mathematical policy fixtures only; v2 median/two-file support, local weighted IQR, class imbalance/outlier resistance, multi-speed selection, replay, finite/units/calibration gates, old-kind compatibility and roundtrip. No independent machine accuracy measured.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
