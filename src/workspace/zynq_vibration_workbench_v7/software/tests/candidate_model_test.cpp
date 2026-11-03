#include "vibration/candidate_model.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace vibration;
void check(bool b,const char*s){if(!b)throw std::runtime_error(s);}
template<class F>void rejects(F f,const char*s){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,s);}
CandidateWindow normal(unsigned i){CandidateWindow w;w.setup_id="rig1_mount1";w.configuration="abcd1234";w.sample_rate_hz=50000;w.first_index=100000*i;w.last_index=w.first_index+99999;w.rpm=w.rpm_min=w.rpm_max=1200;w.fresh_periods=30;w.valid=true;w.source="test_mathematical_feature_fixture";for(unsigned c=0;c<3;++c){auto*x=w.features.data()+12*c;x[0]=.01;x[1]=1.5;x[2]=2;x[3]=.008;x[4]=.01;x[5]=.7;x[6]=.01;x[7]=.03;x[8]=.01;x[9]=.02;x[10]=.02;x[11]=.2;}return w;}
// Mathematical post-transport declarations exercise production extraction;
// none of these generated raw samples is a physical machine measurement.
Capture energyFixture(unsigned fs,bool weakThird,unsigned windows=1,double signalHz=20){
    Capture c;c.header.sample_rate_hz=fs;c.header.clock_hz=50000000;c.header.adc_range_uv=5000000;c.header.channel_map=0x50301;c.header.pulses_per_rev=1;c.integrity_verified=true;c.source_path="mathematical_fixture_only_reference_margin";
    const unsigned count=accelerator::required_raw_frames(fs)*windows;c.records.reserve(count);
    for(unsigned i=0;i<count;++i){Record r;r.index=i;r.tick=std::uint64_t(i)*(50000000/fs);r.flags=3;r.speed_period_ticks=2500000;r.speed_edge_count=i/(fs/20);for(unsigned ch=0;ch<3;++ch)r.raw[ch]=std::int16_t(std::lround((weakThird&&ch==2?3.:1000.)*std::sin(2*std::acos(-1.)*signalHz*i/fs)));c.records.push_back(r);}return c;
}
std::string asciiHex(const std::string&s){const char*d="0123456789abcdef";std::string out;for(unsigned char c:s){out+=d[c>>4];out+=d[c&15];}return out;}
void referenceMarginTests(){
    AnalysisConfig cfg;cfg.amplitude_unit=AmplitudeUnit::Voltage;cfg.signal_kind=SignalKind::VibrationConfirmed;
    for(unsigned fs:{5000u,10000u,25000u,50000u}){
        const auto strong=extract_candidate_window(energyFixture(fs,false),0,cfg,"qa_energy");
        check(strong.valid&&strong.fresh_periods>=8,"adequate reference amplitude and real fresh-period semantics remain eligible");
        check(strong.quality_policy==candidate_quality_policy&&strong.noise_floor_status=="not_characterized","resolution policy must not claim characterized instrument noise");
        for(unsigned ch=0;ch<3;++ch)check(strong.reference_rms_adc[ch]>=64&&strong.ratio_features_valid[ch],"all adequately energized axes expose usable ratios");
        const auto weak=extract_candidate_window(energyFixture(fs,true),0,cfg,"qa_energy");
        check(!weak.valid&&weak.fresh_periods>=8,"valid RPM must not let a weak required axis pass");
        check(std::find(weak.reasons.begin(),weak.reasons.end(),"reference_band_rms_below_policy_CH3")!=weak.reasons.end(),"weak axis rejection identifies CH3");
        check(weak.reference_rms_adc[2]<64&&!weak.ratio_features_valid[2]&&weak.ratio_features_valid[0]&&weak.ratio_features_valid[1],"whole 36-D vector rejects without silently dropping the weak axis");
        for(unsigned i=4;i<12;++i)check(weak.features[24+i]==0,"unqualified weak-axis normalized ratios are not formed");
        auto model=fit_normal_baseline({normal(0),normal(1),normal(2),normal(3),normal(4)});
        check(infer_candidate(weak,model).status=="quality_gated","loaded model cannot override the resolution gate");
        rejects([&]{fit_normal_baseline({weak,weak,weak,weak,weak});},"weak references cannot establish a normal baseline");
    }
    const auto outside=extract_candidate_window(energyFixture(5000,false,1,1000),0,cfg,"qa_energy");
    check(outside.features[0]/(5./32768.)>64&&!outside.valid,"strong 1kHz fullband RMS cannot qualify a weak 2..500Hz reference");
    check(outside.reference_rms_adc[0]<64&&!outside.ratio_features_valid[0],"resolution margin is applied to the normalization denominator band");
    const auto capture=energyFixture(5000,false,5);const auto n=accelerator::required_raw_frames(5000);std::vector<CandidateWindow>w;
    for(unsigned i=0;i<5;++i)w.push_back(extract_candidate_window(capture,i*n,cfg,"qa_energy"));
    const auto current=fit_normal_baseline(w,"qa_energy");check(infer_candidate(w.back(),current).classification=="normal","new-policy strong baseline still performs inference");
    auto old=current;const auto token=asciiHex("|quality_policy=reference_adc_rms64_v1|min_reference_rms_adc=64");const auto at=old.configuration.find(token);check(at!=std::string::npos,"policy and actual threshold are part of immutable model configuration");old.configuration.erase(at,token.size());
    check(infer_candidate(w.back(),old).status=="configuration_mismatch","pre-policy model cannot silently mix with new quality definition");
    auto fractional=energyFixture(5000,false,1,30);
    for(auto& record:fractional.records){record.speed_period_ticks=1666667;record.speed_edge_count=std::uint32_t(record.tick/1666667);}
    const auto constant=extract_candidate_window(fractional,0,cfg,"qa_fractional_rpm");
    check(constant.valid&&constant.fresh_periods>=8&&constant.rpm_min==constant.rpm_max,"constant noninteger RPM extraction remains quality valid");
    check(constant.rpm>=constant.rpm_min&&constant.rpm<=constant.rpm_max,"floating mean of constant speed cannot round outside its measured support and trigger inference quality rejection");
}
int main(){try{
    std::vector<CandidateWindow>w;for(unsigned i=0;i<5;++i)w.push_back(normal(i));auto m=fit_normal_baseline(w);check(m.baseline_count==5,"normal count");auto n=infer_candidate(normal(8),m);check(n.classification=="normal"&&!n.validated,"normal candidate must run");
    auto b=normal(8);for(unsigned c=0;c<3;++c){b.features[c*12]*=3;b.features[c*12+3]*=3;b.features[c*12+5]=.9;}check(infer_candidate(b,m).classification=="imbalance","imbalance pattern must run");
    b=normal(8);for(unsigned c=0;c<3;++c){b.features[c*12]*=3;b.features[c*12+7]=.6;b.features[c*12+5]=.2;}check(infer_candidate(b,m).classification=="misalignment","misalignment pattern must run");
    b=normal(8);for(unsigned c=0;c<3;++c){b.features[c*12]*=3;b.features[c*12+4]=.2;b.features[c*12+6]=.2;b.features[c*12+10]=.2;b.features[c*12+2]=8;b.features[c*12+1]=4;}check(infer_candidate(b,m).classification=="looseness","looseness pattern must run");
    b=normal(8);b.features[11]=.9;auto unknown=infer_candidate(b,m);check(unknown.classification=="unknown"&&unknown.status=="rejected_unknown","unsupported anomaly must reject");
    b=normal(8);b.valid=false;b.reasons={"raw_sequence_or_tick_gap"};check(infer_candidate(b,m).status=="quality_gated","input gaps must gate");b=normal(8);b.setup_id="reinstalled";check(infer_candidate(b,m).status=="configuration_mismatch","installation must bind");b=normal(8);b.rpm_min=500;b.rpm_max=500;check(infer_candidate(b,m).status=="outside_operating_range","speed domain");
    auto text=candidate_model_text(m);check(candidate_model_text(parse_candidate_model(text))==text,"model persistence must roundtrip exactly");rejects([&]{parse_candidate_model(text+"fs=50000\n");},"duplicate keys");rejects([&]{parse_candidate_model(text+"validated=true\n");},"cannot claim validation");auto bad=m;bad.scale[0]=0;rejects([&]{candidate_model_text(bad);},"zero scale");
    auto fewer=w;fewer.resize(4);rejects([&]{fit_normal_baseline(fewer);},"insufficient normal data");bad=m;auto overlapping=w;overlapping[1].first_index=1;rejects([&]{fit_normal_baseline(overlapping);},"overlap is not independent baseline data");auto synthetic=w;synthetic[0].synthetic=true;rejects([&]{fit_normal_baseline(synthetic);},"synthetic may not establish physical baseline");
    auto linear=m;linear.kind="linear_softmax";linear.bias={0,10,0,0};check(infer_candidate(normal(8),linear).classification=="imbalance","imported learned discriminant is executed");check(candidate_model_text(parse_candidate_model(candidate_model_text(linear)))==candidate_model_text(linear),"learned model load/save");
    referenceMarginTests();
    std::cout<<"PASS: mathematical four-pattern scoring, normal fit/persistence, learned-weight inference, unknown and quality/config/speed rejection; four-Fs required-axis reference resolution margin, out-of-band denominator rejection and policy compatibility; no real-machine accuracy assessed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
