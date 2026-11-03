// Exercise the actual CLI comparator with deliberately corrupted results.
// Renaming its entrypoint avoids maintaining a second comparison algorithm.
#define main accelerator_cli_entry_for_test
#include "../src/cli.cpp"
#undef main
int main() {
    Request q;q.sample_rate_hz=5000;q.raw.resize(required_raw_frames(5000));
    q.bands_valid=true;
    const double orders[]={.5,1,1.5,2,2.5,3,4,5,6,7,8,9,10};
    for(unsigned b=0;b<13;++b){const double k=25*orders[b]*8192/5000;q.order_bands[b]={unsigned(std::ceil(k-1.5)),unsigned(std::floor(k+1.5))};}
    for(unsigned n=0;n<q.raw.size();++n)for(unsigned ch=0;ch<3;++ch)
        q.raw[n][ch]=std::int16_t(std::lrint(10000*std::sin(2*std::acos(-1.0)*25*n/5000)));
    auto ref=run(q,Backend::Reference);
    // Comparator fixture: all required bands have declared energy above floor.
    // This is a report-contract test, not a measured or consistent FFT signal.
    for(auto& c:ref.channels){
        c.power_adc2.assign(4097,1.0);c.power_adc2[41]=100;
        c.reference_power_adc2=915;
        for(unsigned b=0;b<13;++b)c.order_power_adc2[b]=double(q.order_bands[b].last-q.order_bands[b].first+1)+(b==1?99:0);
    }
    if(!compare_results(q,ref,ref,80).passed)return 1;
    auto permutation=ref;
    for(auto& c:permutation.channels)std::swap(c.power_adc2[41],c.power_adc2[82]);
    if(compare_results(q,permutation,ref,80).passed)return 2;
    auto wrong_band=ref;wrong_band.channels[0].order_power_adc2[1]*=.5;
    if(compare_results(q,wrong_band,ref,80).passed)return 3;
    auto wrong_stats=ref;++wrong_stats.channels[0].statistics.sum_q8;
    if(compare_results(q,wrong_stats,ref,80).passed)return 4;
    auto wrong_clip=ref;++wrong_clip.raw_endpoint_count[1];
    if(compare_results(q,wrong_clip,ref,80).passed)return 5;
    auto wrong_window=ref;++wrong_window.channels[2].lowband_q8[8000];
    if(compare_results(q,wrong_window,ref,80).passed)return 6;
    auto limited=ref;limited.channels[0].order_power_adc2[0]=0;
    const auto low=compare_results(q,limited,limited,80);
    if(low.passed || low.text.find("\"validity\":\"quantization_limited\"")==std::string::npos ||
       low.text.find("\"passed\":null")==std::string::npos)return 7;
    auto no_orders=q;no_orders.bands_valid=false;
    const auto absent=compare_results(no_orders,ref,ref,80);
    if(absent.passed || absent.text.find("\"validity\":\"not_evaluated\"")==std::string::npos)return 8;
    // Floor-normalized error must not be mislabeled actual relative error.
    auto small=ref;small.channels[0].reference_power_adc2=1e-9;
    auto changed=small;changed.channels[0].reference_power_adc2=4e-9;
    const auto scaled=compare_results(q,changed,small,80);
    if(scaled.passed || scaled.text.find("\"actual_relative_error\":1,")==std::string::npos)return 9;
    std::cout<<"comparison rejects bin permutations, wrong order power, statistics, clips and samples; unavailable/quantization-limited bands never pass; actual and floor-normalized errors remain distinct\n";
    return 0;
}
