#include "view_math.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
using namespace vibration;
using namespace vibration::gui;
namespace { void require(bool b,const char* s){if(!b)throw std::runtime_error(s);} }
int main() {
    try {
        Capture c; c.integrity_verified=true;c.header.sample_rate_hz=1000;c.header.adc_range_uv=10000000;
        c.records.resize(20001);
        for(std::size_t i=0;i<c.records.size();++i)c.records[i].raw={std::int16_t(i%5),std::int16_t(i%7),std::int16_t(i%11)};
        const std::array<std::size_t,6> peaks{321,555,777,12345,15432,18901};
        for(int axis=0;axis<3;++axis){c.records[peaks[2*axis]].raw[axis]=-20000-axis;c.records[peaks[2*axis+1]].raw[axis]=22000+axis;}
        c.records[10005].raw={7,-8,9};
        AnalysisConfig cfg;cfg.analog_gain=-1;cfg.sensor_sensitivity_v_per_g=.5;
        auto overview=waveform_preview(c,cfg,0,20,12);require(overview.size()<=12,"preview exceeded memory budget");
        std::set<std::size_t> indices;for(const auto& p:overview)indices.insert(std::size_t(std::llround(p.time_s*1000)));
        for(auto i:peaks)require(indices.count(i),"lost an independent channel extreme");
        require(!indices.count(10005),"fixture did not distinguish raw zoom from overview zoom");
        const auto local=waveform_preview(c,cfg,10,10.01,12);require(local.size()==11,"local view did not recover every raw sample");
        for(std::size_t j=0;j<local.size();++j){const auto i=10000+j;require(std::abs(local[j].time_s-i/1000.0)<1e-12,"local timestamp mismatch");for(int axis=0;axis<3;++axis)require(std::abs(local[j].values[axis]-c.records[i].raw[axis]*(-20.0/32768.0))<1e-12,"nominal signed conversion mismatch");}
        require(waveform_preview(c,cfg,1.0001,1.0002).empty(),"invented sample inside sub-sample interval");
        auto cursor=waveform_cursor(c,cfg,10.0051);require(cursor.time_s==10.005&&std::abs(cursor.values[1]-8*20.0/32768)<1e-12,"cursor is not nearest raw sample");
        require(waveform_cursor(c,cfg,-100).time_s==0&&waveform_cursor(c,cfg,100).time_s==20,"cursor clamp mismatch");
        cfg.analog_gain=2;require(std::abs(waveform_cursor(c,cfg,10.005).values[0]-7*10.0/32768)<1e-12,"positive gain conversion mismatch");
        Spectrum sp;sp.frequency_hz={0,1.25,2.5,3.75};require(nearest_frequency_bin(sp,-1)==0&&nearest_frequency_bin(sp,2.1)==2&&nearest_frequency_bin(sp,3.125)==2&&nearest_frequency_bin(sp,99)==3,"nearest FFT bin mismatch");
        bool rejected=false;try{waveform_preview(c,cfg,2,1);}catch(const AnalysisError&){rejected=true;}require(rejected,"reversed range accepted");
        rejected=false;try{waveform_cursor(c,cfg,std::numeric_limits<double>::quiet_NaN());}catch(const AnalysisError&){rejected=true;}require(rejected,"non-finite cursor accepted");
        std::cout<<"Raw preview: three-axis extrema, sample recovery, signed scale, cursor, clipping and FFT bin checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
