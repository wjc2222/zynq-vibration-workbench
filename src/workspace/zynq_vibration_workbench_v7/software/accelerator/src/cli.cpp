#include "vibration/accelerator.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

using namespace vibration::accelerator;
namespace {
std::string json(const std::string& s) {
    std::string r="\"";
    for(unsigned char c:s) {if(c=='"'||c=='\\') {r+='\\';r+=char(c);} else if(c>=32)r+=char(c);}
    return r+'"';
}
double number(const std::string& s) {
    std::size_t end=0;double x=std::stod(s,&end);
    if(end!=s.size() || !std::isfinite(x))throw Error("invalid_number: "+s);
    return x;
}
unsigned integer(const std::string& s) {
    if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)throw Error("invalid_integer: "+s);
    std::size_t end=0;auto x=std::stoull(s,&end);
    if(end!=s.size() || x>std::numeric_limits<unsigned>::max())throw Error("integer_out_of_range: "+s);
    return unsigned(x);
}
std::string result_json(const Result& r) {
    std::ostringstream o;o<<std::setprecision(15);
    o<<"{\"schema\":\"zynq_vibration.accelerator_probe.v1\",\"backend\":"<<json(r.source)
     <<",\"hardware_executed\":"<<(r.backend==Backend::Hardware?"true":"false")
     <<",\"algorithm_version\":"<<json(r.algorithm_version)<<",\"coefficients_sha256\":"<<json(r.coefficients_sha256)
     <<",\"hardware_id\":"<<r.hardware_id<<",\"revision\":"<<r.revision
     <<",\"sample_rate_hz\":"<<r.sample_rate_hz<<",\"analysis_rate_hz\":5000,\"points\":8192"
     <<",\"fir_taps\":"<<r.fir_taps<<",\"decimation\":"<<r.decimation
     <<",\"saturation_count\":"<<r.saturation_count<<",\"valid_numeric_result\":"<<(r.valid?"true":"false")
     <<",\"bands_valid\":"<<(r.bands_valid?"true":"false")<<",\"elapsed_ms\":"<<r.elapsed_ms
     <<",\"cpu_ms\":"<<r.cpu_ms<<",\"pl_cycles\":\""<<r.pl_cycles<<"\",\"channels\":[";
    for(unsigned ch=0;ch<3;++ch) {
        const auto& c=r.channels[ch];
        const auto peak=std::max_element(c.power_adc2.begin()+1,c.power_adc2.end())-c.power_adc2.begin();
        const double mean=double(c.statistics.sum_q8)/8192/256;
        const double var=double(c.statistics.sumsq_q16)/8192/65536-mean*mean;
        if(ch)o<<',';
        o<<"{\"channel\":"<<ch+1<<",\"raw_endpoint_count\":"<<r.raw_endpoint_count[ch]
         <<",\"mean_adc\":"<<mean<<",\"ac_rms_adc\":"<<std::sqrt(std::max(0.0,var))
         <<",\"peak_hz\":"<<double(peak)*5000/8192<<",\"peak_bin\":"<<peak
         <<",\"reference_power_adc2\":"<<c.reference_power_adc2
         <<",\"fft_exponent\":"<<c.fft_exponent<<",\"fft_pre_shift\":"<<c.fft_pre_shift<<'}';
    }
    o<<"],\"quality_scope\":\"numeric_only; caller must check continuity, speed, calibration and model scope\"}";
    return o.str();
}
struct Comparison { std::string text; bool passed=false; };
Comparison compare_results(const Request& q,const Result& a,const Result& b,double floor_db) {
    Comparison result;std::ostringstream o;o<<std::setprecision(15);
    bool pass=true,all_required_evaluated=true;std::int64_t largest_sample_error=0;
    o<<"{\"schema\":\"zynq_vibration.numeric_comparison.v2\",\"reference_backend\":"<<json(b.source)
     <<",\"reference_cpu_ms\":"<<b.cpu_ms<<",\"reference_elapsed_ms\":"<<b.elapsed_ms
     <<",\"noise_floor_relative_to_reference_peak_db\":-"<<floor_db
     <<",\"noise_floor_source\":\"provisional engineering floor, not a measured FFT numeric floor\""
     <<",\"strong_bin_margin_above_floor_db\":20,\"rms_relative_tolerance\":0.01,\"peak_bin_tolerance\":1"
     <<",\"acceptance_basis\":\"strict double-reference engineering comparison; not vendor bit-exact implementation acceptance\""
     <<",\"legacy_spectrum_total_and_band_relative_fields\":\"floor_normalized; use actual_relative_error and validity for interpretation\",\"channels\":[";
    for(unsigned ch=0;ch<3;++ch) {
        const auto& ca=a.channels[ch];const auto& cb=b.channels[ch];
        const auto& pa=ca.power_adc2;const auto& pb=cb.power_adc2;
        std::int64_t sample_error=0;
        for(unsigned n=0;n<8192;++n)sample_error=std::max(sample_error,
            std::int64_t(std::abs(std::int64_t(ca.lowband_q8[n])-cb.lowband_q8[n])));
        largest_sample_error=std::max(largest_sample_error,sample_error);
        const bool stats=ca.statistics.sum_q8==cb.statistics.sum_q8 &&
            ca.statistics.sumsq_q16==cb.statistics.sumsq_q16 &&
            ca.statistics.minimum_q8==cb.statistics.minimum_q8 &&
            ca.statistics.maximum_q8==cb.statistics.maximum_q8 &&
            a.raw_endpoint_count[ch]==b.raw_endpoint_count[ch];
        const auto peak_a=std::max_element(pa.begin()+1,pa.end())-pa.begin();
        const auto peak_b=std::max_element(pb.begin()+1,pb.end())-pb.begin();
        const double peak_power=*std::max_element(pb.begin(),pb.end());
        const double floor_power=std::max(1e-12,peak_power*std::pow(10.0,-floor_db/10));
        const double strong_floor=floor_power*100;
        unsigned strong_bins=0,zero_reference_bins=0;double max_strong_rms_error=0,max_actual_bin_relative_error=0;
        double total_a=0,total_b=0,spectral_sq_error=0,max_abs_power_error=0;
        for(unsigned bin=0;bin<4097;++bin) {
            if(!std::isfinite(pa[bin]) || !std::isfinite(pb[bin]) || pa[bin]<0 || pb[bin]<0)
                throw Error("nonfinite_or_negative_spectral_power");
            total_a+=pa[bin];total_b+=pb[bin];
            const double d=std::sqrt(pa[bin])-std::sqrt(pb[bin]);
            spectral_sq_error+=d*d;
            max_abs_power_error=std::max(max_abs_power_error,std::abs(pa[bin]-pb[bin]));
            if(pb[bin]>0)max_actual_bin_relative_error=std::max(max_actual_bin_relative_error,std::abs(d)/std::sqrt(pb[bin]));
            else ++zero_reference_bins;
            if(pb[bin]>=strong_floor) {
                ++strong_bins;
                max_strong_rms_error=std::max(max_strong_rms_error,std::abs(d)/std::sqrt(pb[bin]));
            }
        }
        const double total_rms_error=std::abs(std::sqrt(total_a)-std::sqrt(total_b))/std::sqrt(std::max(total_b,strong_floor));
        const double spectral_rms_error=std::sqrt(spectral_sq_error/std::max(total_b,strong_floor));
        const auto peak_delta=std::abs(peak_a-peak_b);
        const bool peak_evaluated=peak_power>=strong_floor;
        const bool peak_ok=peak_evaluated && peak_delta<=1;
        const bool spectrum_evaluated=total_b>=strong_floor;
        bool axis_required_evaluated=peak_evaluated&&spectrum_evaluated&&strong_bins>0;
        bool axis_ok=sample_error==0 && stats && peak_ok && axis_required_evaluated && max_strong_rms_error<=.01 &&
            spectral_rms_error<=.01 && total_rms_error<=.01;
        if(ch)o<<',';
        o<<"{\"channel\":"<<ch+1<<",\"max_lowband_q8_error\":"<<sample_error
         <<",\"statistics_and_raw_clips_exact\":"<<(stats?"true":"false")
         <<",\"hardware_statistics_match_payload\":true,\"hardware_band_sums_match_bins\":true"
         <<",\"peak_bin_hardware\":"<<peak_a<<",\"peak_bin_reference\":"<<peak_b
         <<",\"peak_bin_delta\":"<<peak_delta<<",\"peak_checked\":"<<(peak_evaluated?"true":"false")
         <<",\"peak_validity\":"<<json(peak_evaluated?"evaluated":"quantization_limited")
         <<",\"noise_floor_power_adc2\":"<<floor_power<<",\"strong_bin_power_min_adc2\":"<<strong_floor
         <<",\"strong_bins\":"<<strong_bins<<",\"max_strong_bin_rms_relative_error\":"<<max_strong_rms_error
         <<",\"strong_bin_comparison_validity\":"<<json(strong_bins?"evaluated_for_eligible_bins_only":"not_evaluated")
         <<",\"excluded_low_power_bins\":"<<4097-strong_bins<<",\"zero_reference_bins\":"<<zero_reference_bins
         <<",\"max_all_positive_bins_actual_relative_error\":"<<max_actual_bin_relative_error
         <<",\"spectrum_rms_relative_error\":"<<spectral_rms_error
         <<",\"spectrum_rms_floor_normalized_error\":"<<spectral_rms_error
         <<",\"spectrum_rms_actual_relative_error\":";
        if(total_b>0)o<<std::sqrt(spectral_sq_error/total_b);else o<<"null";
        o
         <<",\"total_rms_relative_error\":"<<total_rms_error
         <<",\"total_rms_floor_normalized_error\":"<<total_rms_error
         <<",\"total_rms_actual_relative_error\":";
        if(total_b>0)o<<std::abs(std::sqrt(total_a)-std::sqrt(total_b))/std::sqrt(total_b);else o<<"null";
        o<<",\"spectrum_validity\":"<<json(spectrum_evaluated?"evaluated":"quantization_limited")
         <<",\"max_absolute_bin_power_error_adc2\":"<<max_abs_power_error<<",\"bands\":[";
        // Test every supplied order separately; a bin permutation cannot be hidden
        // by preserving just total spectrum power.
        for(unsigned band=0;band<14;++band) {
            if(band)o<<',';
            const bool valid=band==13 || q.bands_valid;
            const unsigned width=band==13?816:valid?q.order_bands[band].last-q.order_bands[band].first+1:1;
            const double va=band==13?ca.reference_power_adc2:ca.order_power_adc2[band];
            const double vb=band==13?cb.reference_power_adc2:cb.order_power_adc2[band];
            const double delta=std::abs(std::sqrt(va)-std::sqrt(vb));
            const double err=valid?delta/std::sqrt(std::max(vb,floor_power*width)):0;
            const bool evaluated=valid && vb>=floor_power*width && vb>0;
            const double actual_relative=valid&&vb>0?delta/std::sqrt(vb):0;
            const bool ok=evaluated && actual_relative<=.01;
            axis_required_evaluated=axis_required_evaluated&&evaluated;
            axis_ok=axis_ok && ok;
            o<<"{\"index\":"<<band<<",\"kind\":"<<json(band==13?"reference":"order")
             <<",\"valid\":"<<(valid?"true":"false")<<",\"hardware_power_adc2\":"<<va
             <<",\"reference_power_adc2\":"<<vb<<",\"rms_relative_error\":";
            if(valid)o<<err;else o<<"null";
            o<<",\"floor_normalized_error\":";
            if(valid)o<<err;else o<<"null";
            o<<",\"actual_relative_error\":";
            if(valid&&vb>0)o<<actual_relative;else o<<"null";
            o<<",\"absolute_rms_error_adc\":";
            if(valid)o<<delta;else o<<"null";
            o<<",\"validity\":"<<json(!valid?"not_evaluated":evaluated?"evaluated":"quantization_limited")<<",\"passed\":";
            if(evaluated)o<<(ok?"true":"false");else o<<"null";
            o<<'}';
        }
        o<<"],\"all_required_comparisons_evaluated\":"<<(axis_required_evaluated?"true":"false")<<",\"passed\":"<<(axis_ok?"true":"false")<<'}';
        pass=pass && axis_ok;
        all_required_evaluated=all_required_evaluated&&axis_required_evaluated;
    }
    pass=pass && a.saturation_count==b.saturation_count;
    o<<"],\"all_required_comparisons_evaluated\":"<<(all_required_evaluated?"true":"false")<<",\"max_lowband_q8_error\":"<<largest_sample_error<<",\"saturation_count_exact\":"
     <<(a.saturation_count==b.saturation_count?"true":"false")<<",\"passed\":"<<(pass?"true":"false")
     <<",\"scope\":\"strict subset/bin/band precision check with provisional floor; unevaluated required items are not a pass; vendor-model implementation acceptance is independent\"}";
    result.text=o.str();result.passed=pass;return result;
}
}
int main(int argc,char** argv) {
    try {
        Backend backend=Backend::Hardware;
        std::string device="/dev/vibration-accel",raw_path,output;
        bool probe_only=false,synthetic=false,compare=false;
        Request q;double frequency=25,amplitude=10000,dc=100,rotation=0,floor_db=80;
        for(int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            auto value=[&](){if(i+1>=argc)throw Error("missing_value: "+arg);return std::string(argv[++i]);};
            if(arg=="--backend") {auto v=value();if(v=="hardware")backend=Backend::Hardware;else if(v=="reference")backend=Backend::Reference;else throw Error("backend_must_be_hardware_or_reference");}
            else if(arg=="--device")device=value();
            else if(arg=="--fs")q.sample_rate_hz=integer(value());
            else if(arg=="--timeout-ms")q.timeout_ms=integer(value());
            else if(arg=="--raw")raw_path=value();
            else if(arg=="--output")output=value();
            else if(arg=="--frequency")frequency=number(value());
            else if(arg=="--amplitude")amplitude=number(value());
            else if(arg=="--dc")dc=number(value());
            else if(arg=="--rotation-hz")rotation=number(value());
            else if(arg=="--noise-floor-relative-db")floor_db=number(value());
            else if(arg=="--synthetic")synthetic=true;
            else if(arg=="--probe")probe_only=true;
            else if(arg=="--compare")compare=true;
            else if(arg=="--help") {
                std::cout<<"vibration-accel-cli --probe [--device PATH]\n"
                    <<"vibration-accel-cli --backend hardware|reference --fs 50000|25000|10000|5000 "
                    <<"(--synthetic [--frequency Hz --amplitude code --dc code] | --raw signed16le3.bin) "
                    <<"[--compare] [--rotation-hz Hz] [--noise-floor-relative-db 80] [--timeout-ms 10000] [--output report.json]\n"
                    <<"Hardware is the default. Missing hardware never invokes the reference.\n";
                return 0;
            } else throw Error("unknown_option: "+arg);
        }
        if(probe_only) {
            const auto p=probe(device);
            std::cout<<"{\"available\":"<<(p.available?"true":"false")<<",\"hardware_id\":"<<p.hardware_id
                     <<",\"revision\":"<<p.revision<<",\"clock_hz\":"<<p.clock_hz<<",\"reason\":"<<json(p.reason)<<"}\n";
            return p.available?0:3;
        }
        if(synthetic==!raw_path.empty())throw Error("choose_exactly_one_of_synthetic_or_raw");
        q.raw.resize(required_raw_frames(q.sample_rate_hz));
        if(synthetic) {
            if(frequency<=0 || frequency>=2500 || amplitude<0 || std::abs(dc)+amplitude>32766)
                throw Error("synthetic_parameters_outside_safe_ADC_range");
            for(std::size_t n=0;n<q.raw.size();++n)for(unsigned ch=0;ch<3;++ch)
                q.raw[n][ch]=std::int16_t(std::lrint(dc+amplitude*(1.0-0.2*ch)*std::sin(2*std::acos(-1.0)*frequency*n/q.sample_rate_hz+ch*0.2)));
        } else {
            std::ifstream f(raw_path,std::ios::binary);
            if(!f)throw Error("cannot_open_raw_input");
            for(auto& frame:q.raw)for(auto& sample:frame) {
                unsigned char bytes[2];if(!f.read(reinterpret_cast<char*>(bytes),2))throw Error("raw_input_too_short");
                sample=std::int16_t(unsigned(bytes[0])|(unsigned(bytes[1])<<8));
            }
            if(f.peek()!=std::char_traits<char>::eof())throw Error("raw_input_too_long");
        }
        if(rotation<0 || rotation>100 || floor_db<40 || floor_db>120)throw Error("comparison_rotation_or_floor_out_of_range");
        const double band_rotation=rotation>0?rotation:(synthetic?frequency:0);
        if(band_rotation>0) {
            const double orders[]={0.5,1,1.5,2,2.5,3,4,5,6,7,8,9,10};
            q.bands_valid=true;
            for(unsigned b=0;b<13;++b) {
                const double center=band_rotation*orders[b]*8192/5000;
                q.order_bands[b].first=unsigned(std::max(4.0,std::ceil(center-1.5)));
                q.order_bands[b].last=unsigned(std::floor(center+1.5));
                if(q.order_bands[b].last>819 || q.order_bands[b].first>q.order_bands[b].last)q.bands_valid=false;
            }
        }
        auto r=run(q,backend,device);
        std::string report=result_json(r);
        report.pop_back();
        report+=",\"input_kind\":"+json(synthetic?"synthetic":"raw_signed16le_window")+
            ",\"band_source\":"+json(rotation>0?"explicit_rotation_hz":synthetic?"synthetic_frequency":"none")+"}";
        bool pass=true;
        if(compare) {
            if(backend!=Backend::Hardware)throw Error("compare_requires_hardware_backend");
            auto ref=run(q,Backend::Reference);
            const auto comparison=compare_results(q,r,ref,floor_db);
            pass=comparison.passed;
            report.pop_back();report+=",\"comparison\":"+comparison.text+"}";
        }
        if(!output.empty()) {
            // No implicit overwrite; reports are audit evidence.
            std::ifstream old(output);if(old.good())throw Error("output_already_exists");
            std::ofstream f(output,std::ios::binary);if(!f || !(f<<report<<'\n'))throw Error("cannot_write_report");
        }
        std::cout<<report<<'\n';return pass?0:4;
    }catch(const std::exception& e) {
        std::cerr<<"{\"error\":"<<json(e.what())<<",\"hardware_result_available\":false,\"fallback_performed\":false}\n";
        return 2;
    }
}
