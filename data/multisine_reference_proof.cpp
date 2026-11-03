// HOST proof using unchanged production reference; no hardware or model access.
#include "vibration/accelerator.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace vibration::accelerator;
int main(int argc,char** argv){
 if(argc!=2)return 2; Request q;q.sample_rate_hz=50000;q.raw.resize(required_raw_frames(50000));
 std::ifstream f(argv[1],std::ios::binary);if(!f)return 3;
 for(auto& frame:q.raw)for(auto& value:frame){unsigned char b[2];if(!f.read(reinterpret_cast<char*>(b),2))return 4;value=std::int16_t(unsigned(b[0])|(unsigned(b[1])<<8));}
 if(f.peek()!=std::char_traits<char>::eof())return 5;
 const double orders[]={.5,1,1.5,2,2.5,3,4,5,6,7,8,9,10};q.bands_valid=true;
 for(unsigned b=0;b<13;++b){const auto center=25*orders[b]*8192/5000;q.order_bands[b]={unsigned(std::max(4.,std::ceil(center-1.5))),unsigned(std::floor(center+1.5))};}
 const auto r=run(q,Backend::Reference);bool eligible=r.valid&&r.saturation_count==0;
 std::cout<<std::setprecision(16)<<"{\"scope\":\"HOST unchanged production PS reference only; SYNTHETIC engineering fixture, not physical measurement or hardware validation\",\"sample_rate_hz\":50000,\"frames\":"<<q.raw.size()<<",\"rotation_hz\":25,\"hardware_executed\":false,\"tolerances_changed\":false,\"coefficients_sha256\":\""<<r.coefficients_sha256<<"\",\"channels\":[";
 for(unsigned ch=0;ch<3;++ch){const auto& c=r.channels[ch];const auto peak=std::max_element(c.power_adc2.begin()+1,c.power_adc2.end())-c.power_adc2.begin();const auto power=c.power_adc2[peak];const auto floor=std::max(1e-12,power*1e-8);unsigned strong=0;for(auto v:c.power_adc2)if(v>=floor*100)++strong;
  if(ch)std::cout<<',';std::cout<<"{\"channel\":"<<ch+1<<",\"peak_bin\":"<<peak<<",\"peak_power_adc2\":"<<power<<",\"default_floor_power_adc2\":"<<floor<<",\"strong_bin_count\":"<<strong<<",\"raw_endpoint_count\":"<<r.raw_endpoint_count[ch]<<",\"reference_power_adc2\":"<<c.reference_power_adc2<<",\"bands\":[";
  for(unsigned b=0;b<14;++b){const auto width=b==13?816:q.order_bands[b].last-q.order_bands[b].first+1;const auto v=b==13?c.reference_power_adc2:c.order_power_adc2[b];const auto threshold=floor*width;const bool pass=v>=threshold&&v>0;eligible=eligible&&pass;
   if(b)std::cout<<',';std::cout<<"{\"index\":"<<b<<",\"kind\":\""<<(b==13?"reference":"order")<<"\",\"power_adc2\":"<<v<<",\"eligibility_threshold_adc2\":"<<threshold<<",\"eligibility_margin_ratio\":"<<v/threshold<<",\"evaluated_against_unchanged_default_floor\":"<<(pass?"true":"false")<<'}';}
  std::cout<<"]}";
 }
 std::cout<<"],\"all_42_channel_band_comparisons_eligible\":"<<(eligible?"true":"false")<<"}\n";return eligible?0:6;
}
