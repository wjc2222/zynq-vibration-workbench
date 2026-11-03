#include "vibration/accelerator.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <stdexcept>
using namespace vibration::accelerator;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F f,const char* why){bool rejected=false;try{f();}catch(const Error&){rejected=true;}check(rejected,why);}
Request sine(unsigned fs,double hz,double amplitude=10000,double dc=0){
    Request q;q.sample_rate_hz=fs;q.raw.resize(required_raw_frames(fs));
    for(unsigned n=0;n<q.raw.size();++n)for(unsigned ch=0;ch<3;++ch)
        q.raw[n][ch]=std::int16_t(std::lrint(dc+amplitude*std::sin(2*std::acos(-1.0)*hz*n/fs+ch*.3)));
    return q;
}
int main(){try{
    check(required_raw_frames(50000)==82551,"50k raw support");
    check(required_raw_frames(25000)==41276,"25k raw support");
    check(required_raw_frames(10000)==16511,"10k raw support");
    check(required_raw_frames(5000)==8256,"5k raw support");
    rejects([]{required_raw_frames(44100);},"unsupported sample rate accepted");
    for(unsigned fs:{50000,25000,10000,5000}){
        auto q=sine(fs,25,10000,3000);
        auto r=run(q,Backend::Reference);
        check(r.backend==Backend::Reference && r.hardware_id==0,"reference mislabeled PL");
        check(r.valid && r.saturation_count==0,"valid sine rejected");
        check(r.coefficients_sha256.size()==64,"missing coefficient identity");
        for(auto& c:r.channels){
            check(c.lowband_q8.size()==8192 && c.power_adc2.size()==4097,"result shape");
            const auto peak=std::max_element(c.power_adc2.begin()+1,c.power_adc2.end())-c.power_adc2.begin();
            check(std::abs(double(peak)*5000/8192-25)<5000.0/8192,"sine peak");
            const double total=std::accumulate(c.power_adc2.begin(),c.power_adc2.end(),0.0);
            check(std::abs(total-50000000)/50000000<.002,"Hann RMS normalization or filter gain");
            check(std::all_of(c.lowband_q8.begin(),c.lowband_q8.end(),[](int x){return x%2==0;}),"Q7 expansion is not exact");
        }
        auto stop=run(sine(fs,fs==5000?2450:3000,10000),Backend::Reference);
        for(auto& c:stop.channels){
            const double power=std::accumulate(c.power_adc2.begin(),c.power_adc2.end(),0.0);
            check(std::sqrt(power)<1.0,"stopband aliases into detection window");
        }
        auto dc=q;for(auto& f:dc.raw)f={1234,-2345,100};
        auto d=run(dc,Backend::Reference);
        for(auto& c:d.channels)check(*std::min_element(c.lowband_q8.begin(),c.lowband_q8.end())==*std::max_element(c.lowband_q8.begin(),c.lowband_q8.end()),"full-history DC has transient");
    }
    auto q=sine(5000,25);q.raw.pop_back();
    rejects([&]{run(q,Backend::Reference);},"short input accepted");
    q=sine(5000,25);q.timeout_ms=30001;
    rejects([&]{run(q,Backend::Reference);},"unbounded timeout accepted");
    q=sine(5000,25);q.bands_valid=true;for(unsigned i=0;i<13;++i)q.order_bands[i]={10+i*5,12+i*5};
    validate_request(q);q.order_bands[1]={11,15};
    rejects([&]{run(q,Backend::Reference);},"overlapping bands accepted");
    q=sine(5000,25);q.raw[0][0]=-32768;q.raw.back()[1]=32767;
    auto clipping=run(q,Backend::Reference);
    check(!clipping.valid && clipping.raw_endpoint_count[0]==1 && clipping.raw_endpoint_count[1]==1,"raw clipping gate");
    auto p=probe("/definitely/missing/vibration-accel");
    check(!p.available,"missing device available");
    rejects([&]{run(q,Backend::Hardware,"/definitely/missing/vibration-accel");},"missing hardware silently fell back");
    std::cout<<"accelerator contract, four-rate FIR/FFT, anti-alias, clipping and fail-closed checks passed\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
