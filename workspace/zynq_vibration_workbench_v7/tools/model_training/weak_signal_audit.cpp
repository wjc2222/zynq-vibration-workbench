// mathematical_fixture_only: audit the current production eligibility gate.
// Mock source/integrity declarations deliberately exercise the post-transport
// branch; this program creates no physical-record or mechanical-accuracy claim.
#include "vibration/candidate_model.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

int main(){
    try{
        for(const int peak:{2,3,8,32,128,1000}){
            vibration::Capture c;c.header.sample_rate_hz=5000;c.header.clock_hz=50000000;
            c.header.adc_range_uv=5000000;c.header.channel_map=0x50301;c.header.pulses_per_rev=1;
            c.integrity_verified=true;c.source_path="mathematical_fixture_only_weak_raw";
            const unsigned n=vibration::accelerator::required_raw_frames(5000);
            c.records.reserve(5*n);
            for(unsigned i=0;i<5*n;++i){vibration::Record r;r.index=i;r.tick=std::uint64_t(i)*10000;
                r.flags=3;r.speed_period_ticks=2500000;r.speed_edge_count=i/250;
                for(auto&x:r.raw)x=std::int16_t(std::lround(peak*std::sin(2*std::acos(-1.)*20*i/5000.)));
                c.records.push_back(r);}
            vibration::AnalysisConfig cfg;cfg.amplitude_unit=vibration::AmplitudeUnit::Voltage;
            cfg.signal_kind=vibration::SignalKind::VibrationConfirmed;
            std::vector<vibration::CandidateWindow>w;
            for(unsigned i=0;i<5;++i)w.push_back(vibration::extract_candidate_window(c,i*n,cfg,"qa_weak_signal"));
            std::cout<<"{\"scope\":\"mathematical_fixture_only\",\"raw_peak_adc\":"<<peak
                <<",\"ac_rms_adc\":"<<w.front().features[0]/(5./32768.)
                <<",\"reference_rms_adc\":"<<w.front().reference_rms_adc[0]
                <<",\"quality_policy\":\""<<w.front().quality_policy<<'"'
                <<",\"quality_valid\":"<<(w.front().valid?"true":"false")
                <<",\"fresh_periods\":"<<w.front().fresh_periods;
            if(std::all_of(w.begin(),w.end(),[](const auto&one){return one.valid;})){
                const auto model=vibration::fit_normal_baseline(w,"qa_weak_signal");
                std::cout<<",\"inference\":"<<vibration::candidate_inference_json(vibration::infer_candidate(w.back(),model));
            }else std::cout<<",\"window\":"<<vibration::candidate_window_json(w.front());
            std::cout<<"}\n";
        }
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
