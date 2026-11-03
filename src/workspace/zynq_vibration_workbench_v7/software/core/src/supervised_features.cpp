#include "vibration/supervised_features.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>

namespace vibration { namespace {
// Successful per-bin checks must not allocate a temporary error string.
void need(bool value,const char* message){if(!value)throw AnalysisError(std::string("Supervised features: ")+message);}
std::string hex(const std::string& value){constexpr const char* digits="0123456789abcdef";std::string out;for(unsigned char ch:value){out+=digits[ch>>4];out+=digits[ch&15];}return out;}
std::string quote(const std::string& value){std::ostringstream out;out<<'"';for(unsigned char ch:value){if(ch=='"'||ch=='\\')out<<'\\'<<char(ch);else if(ch<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(ch)<<std::dec;else out<<char(ch);}return out.str()+'"';}
template<class T>void numbers(std::ostream& out,const T& values){bool first=true;for(double value:values){if(!first)out<<',';first=false;out<<value;}}
double entropy(const std::vector<double>& fractions){double result=0;for(double p:fractions)if(p>0)result-=p*std::log(p);return fractions.size()>1?result/std::log(double(fractions.size())):0;}
std::array<double,8> db4_packets(const std::vector<double>& centered){
    // Orthogonal db4 analysis filters, periodized at every decimation level.
    // A global sign of a detail filter does not change packet energy.
    constexpr std::array<double,8> h{{-.010597401785069032,.0328830116668852,.030841381835560764,-.18703481171909309,-.027983769416859854,.6308807679298587,.7148465705529154,.2303778133088964}};
    std::vector<std::vector<double>> nodes{centered};
    for(unsigned depth=0;depth<3;++depth){std::vector<std::vector<double>> next;next.reserve(nodes.size()*2);
        // Every node has 8192, 4096 or 2048 samples. Thus 2*i+k<N+6,
        // and one subtraction is the identical periodic index without an ARM
        // software remainder call per tap. Keep all FP operations in order.
        for(const auto& node:nodes){std::vector<double> low(node.size()/2),high(node.size()/2);for(std::size_t i=0;i<low.size();++i)for(unsigned k=0;k<8;++k){const auto index=2*i+k;const double value=node[index<node.size()?index:index-node.size()];low[i]+=h[k]*value;high[i]+=(k%2?-1.:1.)*h[7-k]*value;}next.push_back(std::move(low));next.push_back(std::move(high));}nodes=std::move(next);
    }
    std::array<double,8> energies{};double total=0;for(unsigned i=0;i<8;++i){for(double value:nodes[i])energies[i]+=value*value;total+=energies[i];}if(total>0)for(double& value:energies)value/=total;return energies;
}
} // namespace

const std::array<std::string,supervised_feature_count>& supervised_feature_names(){
    static const auto names=[](){std::array<std::string,supervised_feature_count> result{};const char* perChannel[]{"log1p_ac_rms_adc","crest_factor","pearson_kurtosis","power_fraction_2_16Hz","power_fraction_16_32Hz","power_fraction_32_64Hz","power_fraction_64_125Hz","power_fraction_125_250Hz","power_fraction_250_500Hz","power_fraction_500_2000Hz","spectral_centroid_over_2500Hz","spectral_entropy_2_2000Hz","db4_packet_AAA_fraction","db4_packet_AAD_fraction","db4_packet_ADA_fraction","db4_packet_ADD_fraction","db4_packet_DAA_fraction","db4_packet_DAD_fraction","db4_packet_DDA_fraction","db4_packet_DDD_fraction","db4_packet_entropy"};for(unsigned ch=0;ch<3;++ch)for(unsigned j=0;j<21;++j)result[21*ch+j]="CH"+std::to_string(ch+1)+"."+perChannel[j];for(unsigned ch=0;ch<3;++ch)result[63+ch]="CH"+std::to_string(ch+1)+".ac_energy_fraction";result[66]="correlation_CH1_CH2";result[67]="correlation_CH1_CH3";result[68]="correlation_CH2_CH3";return result;}();return names;
}

SupervisedFeatureVector supervised_features_from_accelerator(const accelerator::Result& result,std::array<double,3>* rmsOut){
    need(result.analysis_sample_rate_hz==5000,"requires 5 kHz lowband output");SupervisedFeatureVector features{};std::array<std::vector<double>,3> centered;std::array<double,3> variance{};
    constexpr std::array<double,8> edges{{2,16,32,64,125,250,500,2000}};
    for(unsigned ch=0;ch<3;++ch){const auto& channel=result.channels[ch];need(channel.lowband_q8.size()==8192&&channel.power_adc2.size()==4097,"incomplete PL FIR/FFT result");auto& x=centered[ch];x.reserve(8192);double mean=0;for(auto sample:channel.lowband_q8)mean+=sample/256.;mean/=8192.;for(auto sample:channel.lowband_q8)x.push_back(sample/256.-mean);
        double fourth=0,peak=0;for(double value:x){variance[ch]+=value*value;fourth+=value*value*value*value;peak=std::max(peak,std::abs(value));}variance[ch]/=8192.;fourth/=8192.;auto* f=features.data()+21*ch;const double rms=std::sqrt(variance[ch]);if(rmsOut)(*rmsOut)[ch]=rms;f[0]=std::log1p(rms);if(rms>0){f[1]=peak/rms;f[2]=fourth/(variance[ch]*variance[ch]);}
        std::array<double,7> band{};std::vector<double> powers;powers.reserve(channel.power_adc2.size());double total=0,moment=0;
        for(unsigned k=0;k<channel.power_adc2.size();++k){const double power=channel.power_adc2[k];need(std::isfinite(power)&&power>=0,"invalid FFT power");const double frequency=5000.*k/8192.;if(frequency<2||frequency>2000)continue;powers.push_back(power);total+=power;moment+=frequency*power;for(unsigned b=0;b<7;++b)if(frequency>=edges[b]&&(frequency<edges[b+1]||(b==6&&frequency<=edges[b+1]))){band[b]+=power;break;}}
        if(total>0){for(unsigned b=0;b<7;++b)f[3+b]=band[b]/total;f[10]=moment/total/2500.;for(double& value:powers)value/=total;f[11]=entropy(powers);}
        const auto packets=db4_packets(x);std::vector<double> packetFractions;packetFractions.reserve(8);for(unsigned p=0;p<8;++p){f[12+p]=packets[p];packetFractions.push_back(packets[p]);}f[20]=entropy(packetFractions);
    }
    const double energy=std::accumulate(variance.begin(),variance.end(),0.);if(energy>0)for(unsigned ch=0;ch<3;++ch)features[63+ch]=variance[ch]/energy;
    unsigned index=66;for(unsigned a=0;a<3;++a)for(unsigned b=a+1;b<3;++b){double covariance=0;for(unsigned i=0;i<8192;++i)covariance+=centered[a][i]*centered[b][i];covariance/=8192.;if(variance[a]>0&&variance[b]>0)features[index]=std::clamp(covariance/std::sqrt(variance[a]*variance[b]),-1.,1.);++index;}
    for(double value:features)need(std::isfinite(value),"nonfinite feature");
    return features;
}

SupervisedWindow extract_supervised_window(const Capture& capture,std::size_t start,const AnalysisConfig& cfg,const std::string& setup,accelerator::Backend backend){
    const auto begun=std::chrono::steady_clock::now();
    SupervisedWindow w;w.setup_id=setup;w.sample_rate_hz=capture.header.sample_rate_hz;w.source_path=capture.source_path;w.capture_sha256=capture.capture_sha256;w.synthetic=capture.source_metadata.synthetic;w.input_kind=capture.source_path=="local_contiguous_raw_chunks"?"local_full_raw_crc_continuous":"committed_bin";
    if(capture.header.stream_segment)w.source_session_id=hex(std::string(reinterpret_cast<const char*>(capture.header.session_id.data()),capture.header.session_id.size()));
    const auto n=accelerator::required_raw_frames(w.sample_rate_hz);need(start<=capture.records.size()&&n<=capture.records.size()-start,"window needs complete FIR history plus 8192 lowband points");need(capture.header.clock_hz>0&&capture.header.clock_hz%w.sample_rate_hz==0,"invalid record clock/sample-rate relationship");
    if(!capture.integrity_verified)w.reasons.push_back("input_integrity_unverified");
    if(w.synthetic)w.reasons.push_back("synthetic_not_machine_evidence");
    if(capture.source_metadata.status=="invalid_or_unreadable_sidecar")w.reasons.push_back("source_metadata_invalid");
    if(cfg.signal_kind!=SignalKind::VibrationConfirmed)w.reasons.push_back("mechanical_source_not_confirmed");
    if(setup.empty())w.reasons.push_back("setup_id_missing");
    accelerator::Request request;request.sample_rate_hz=w.sample_rate_hz;request.window_seq=capture.records[start].index;request.bands_valid=false;request.raw.reserve(n);w.first_index=capture.records[start].index;w.last_index=capture.records[start+n-1].index;const unsigned center=(accelerator::fir_taps(w.sample_rate_hz)-1)/2;w.analysis_first_index=capture.records[start+center].index;w.analysis_last_index=capture.records[start+center+8191*(w.sample_rate_hz/5000)].index;
    Sha256 identity;const auto tickStep=capture.header.clock_hz/w.sample_rate_hz;
    // Preserve the complete-record identity used by existing extraction. Tach
    // bytes are hashed as provenance only; none enters the feature vector.
    for(std::size_t i=0;i<n;++i){const auto& record=capture.records[start+i];request.raw.push_back(record.raw);std::array<std::uint8_t,32> bytes{};auto put=[&](unsigned offset,std::uint64_t value,unsigned count){for(unsigned b=0;b<count;++b)bytes[offset+b]=std::uint8_t(value>>(8*b));};put(0,record.index,8);put(8,record.tick,8);for(unsigned ch=0;ch<3;++ch)put(16+2*ch,std::uint16_t(record.raw[ch]),2);put(22,record.flags,2);put(24,record.speed_period_ticks,4);put(28,record.speed_edge_count,4);identity.update(bytes.data(),bytes.size());if(!(record.flags&1u)||(record.flags&~31u)||(i&&(record.index!=capture.records[start+i-1].index+1||record.tick!=capture.records[start+i-1].tick+tickStep))){w.reasons.push_back("raw_sequence_or_tick_gap");break;}}
    if(request.raw.size()!=n){w.source=accelerator::backend_name(backend);w.elapsed_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count();return w;}w.raw_window_sha256=identity.hex_digest();const auto result=accelerator::run(request,backend);w.source=result.source;
    if(result.saturation_count)w.warnings.push_back("accelerator_arithmetic_saturation");
    for(unsigned ch=0;ch<3;++ch)if(result.raw_endpoint_count[ch])w.warnings.push_back("raw_adc_clipping_CH"+std::to_string(ch+1));
    for(const auto& reason:result.reasons)if(reason!="arithmetic_saturation"&&reason.rfind("raw_ADC_endpoint_CH",0)!=0)w.reasons.push_back("accelerator_"+reason);
    if(!result.valid&&result.reasons.empty())w.reasons.push_back("accelerator_invalid_result");
    w.features=supervised_features_from_accelerator(result,&w.ac_rms_adc);if(std::all_of(w.ac_rms_adc.begin(),w.ac_rms_adc.end(),[](double value){return value==0;}))w.reasons.push_back("all_axes_flat_no_ac_signal");
    // ADC-domain definitions: user acceleration conversion and tachometer/PPR
    // are absent. Hardware ADC range/map and FIR/FFT identity remain explicit.
    std::ostringstream config;config.imbue(std::locale::classic());config<<supervised_feature_schema<<'|'<<result.algorithm_version<<'|'<<result.coefficients_sha256<<'|'<<result.fir_taps<<'|'<<result.decimation<<'|'<<w.sample_rate_hz<<'|'<<capture.header.adc_range_uv<<'|'<<capture.header.channel_map<<"|adc_domain|periodic_db4_natural_depth3|spectral_2_2000Hz";w.configuration=hex(config.str());std::sort(w.reasons.begin(),w.reasons.end());w.reasons.erase(std::unique(w.reasons.begin(),w.reasons.end()),w.reasons.end());w.valid=w.reasons.empty();w.elapsed_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count();return w;
}

std::string supervised_window_json(const SupervisedWindow& w){
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<"{\"feature_schema\":"<<quote(w.feature_schema)<<",\"configuration\":"<<quote(w.configuration)<<",\"setup_id\":"<<quote(w.setup_id)<<",\"source\":"<<quote(w.source)<<",\"source_path\":"<<quote(w.source_path)<<",\"input_kind\":"<<quote(w.input_kind)<<",\"capture_sha256\":"<<quote(w.capture_sha256)<<",\"source_session_id\":"<<quote(w.source_session_id)<<",\"raw_window_sha256\":"<<quote(w.raw_window_sha256)<<",\"fs\":"<<w.sample_rate_hz<<",\"first_index\":"<<w.first_index<<",\"last_index\":"<<w.last_index<<",\"analysis_first_index\":"<<w.analysis_first_index<<",\"analysis_last_index\":"<<w.analysis_last_index<<",\"quality\":"<<quote(w.valid?"valid":"invalid")<<",\"synthetic\":"<<(w.synthetic?"true":"false")<<",\"rpm_used\":false,\"elapsed_ms\":"<<w.elapsed_ms<<",\"features\":[";numbers(out,w.features);out<<"],\"ac_rms_adc\":[";numbers(out,w.ac_rms_adc);out<<"],\"reasons\":[";for(std::size_t i=0;i<w.reasons.size();++i){if(i)out<<',';out<<quote(w.reasons[i]);}out<<"],\"warnings\":[";for(std::size_t i=0;i<w.warnings.size();++i){if(i)out<<',';out<<quote(w.warnings[i]);}return out.str()+"]}";
}
} // namespace vibration
