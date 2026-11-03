#include "vibration/supervised_features.hpp"
#include "vibration/candidate_model.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace vibration;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void near(double a,double b,const char* message){check(std::abs(a-b)<1e-10,message);}
Capture fixture(bool quietThird=false){Capture c;c.header.sample_rate_hz=5000;c.header.clock_hz=50000000;c.header.adc_range_uv=5000000;c.header.channel_map=0x50301;c.header.pulses_per_rev=1;c.integrity_verified=true;c.source_path="mathematical_rpmfree_extraction_only";const auto n=accelerator::required_raw_frames(5000);for(unsigned i=0;i<n;++i){Record r;r.index=i;r.tick=10000ULL*i;r.flags=3;r.speed_period_ticks=2500000;r.speed_edge_count=i/250;for(unsigned ch=0;ch<3;++ch)r.raw[ch]=quietThird&&ch==2?0:std::int16_t(std::lround((500+100*ch)*std::sin(2*std::acos(-1.)*(30+10*ch)*i/5000.)));c.records.push_back(r);}return c;}
}
int main(){try{
    AnalysisConfig cfg;cfg.signal_kind=SignalKind::VibrationConfirmed;cfg.amplitude_unit=AmplitudeUnit::Voltage;
    const auto original=fixture();const auto w=extract_supervised_window(original,0,cfg,"math-rpmfree");check(w.valid&&w.features.size()==69&&w.elapsed_ms>0,"complete mathematical input produces finite RPM-free69 features and end-to-end timing");
    auto noTach=original;noTach.header.pulses_per_rev=0;for(auto& r:noTach.records){r.flags=1;r.speed_period_ticks=0;r.speed_edge_count=0;}auto changedConfig=cfg;changedConfig.ppr_override=123;changedConfig.sensor_sensitivity_v_per_g=99;changedConfig.analog_gain=17;
    const auto without=extract_supervised_window(noTach,0,changedConfig,"math-rpmfree");check(without.valid&&without.features==w.features&&without.configuration==w.configuration,"absent tach/PPR and unused acceleration conversion cannot change features, identity or validity");check(without.raw_window_sha256!=w.raw_window_sha256,"complete-record integrity identity still preserves changed tach bytes");
    const auto old=extract_candidate_window(original,0,cfg,"math-rpmfree");check(old.raw_window_sha256==w.raw_window_sha256,"new raw-window identity matches existing complete 32-byte record encoding");
    const auto quiet=extract_supervised_window(fixture(true),0,cfg,"math-rpmfree");check(quiet.valid&&quiet.ac_rms_adc[2]==0,"one quiet axis cannot veto other usable measured channels");for(unsigned j=42;j<63;++j)near(quiet.features[j],0,"quiet-axis feature coordinates are defined zeros");
    auto allFlat=original;for(auto& r:allFlat.records)r.raw={0,0,0};check(!extract_supervised_window(allFlat,0,cfg,"math-rpmfree").valid,"entirely flat three-axis input cannot fabricate mechanical signal");
    auto synthetic=original;synthetic.source_metadata.synthetic=true;check(!extract_supervised_window(synthetic,0,cfg,"math-rpmfree").valid,"synthetic fixture source cannot become real-machine evidence");
    auto clipped=original;clipped.records[20].raw[0]=32767;const auto clipping=extract_supervised_window(clipped,0,cfg,"math-rpmfree");check(clipping.valid&&!clipping.warnings.empty(),"finite complete clipped input remains analyzable with explicit detail warning");
    auto gap=original;gap.records[100].index+=1;check(!extract_supervised_window(gap,0,cfg,"math-rpmfree").valid,"corrupt raw continuity remains rejected without tach dependence");
    for(unsigned ch=0;ch<3;++ch){double spectral=0,packets=0;for(unsigned j=3;j<10;++j)spectral+=w.features[21*ch+j];for(unsigned j=12;j<20;++j)packets+=w.features[21*ch+j];near(spectral,1,"fixed-Hz energy partition sums to one");near(packets,1,"orthogonal db4 packet energy partition sums to one");check(w.features[21*ch+11]>=0&&w.features[21*ch+11]<=1&&w.features[21*ch+20]>=0&&w.features[21*ch+20]<=1,"normalized entropies bounded");}
    near(w.features[63]+w.features[64]+w.features[65],1,"three-axis AC-energy partition sums to one");
    const auto& names=supervised_feature_names();check(std::set<std::string>(names.begin(),names.end()).size()==69,"69 fixed feature coordinates have unique stable names");check(supervised_window_json(w).find("\"rpm_used\":false")!=std::string::npos,"export explicitly identifies RPM-free inputs");
    std::cout<<"PASS: mathematical RPM-free69 extraction, absent tach/PPR invariance, low-axis support, complete-record hashes, db4/spectral partitions, quality and schema contracts; no machine accuracy measured.\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
