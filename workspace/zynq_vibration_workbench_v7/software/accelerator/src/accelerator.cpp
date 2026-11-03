#include "vibration/accelerator.hpp"
#include "analysis_coefficients.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <sstream>
#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ax7z020_accel.h"
#include "ax7z020_accel_regs.h"
static_assert(sizeof(ax7z020_accel_caps)==64,"caps ABI layout");
static_assert(sizeof(ax7z020_accel_plan)==136,"plan ABI layout");
static_assert(sizeof(ax7z020_accel_channel)==65736,"channel ABI layout");
static_assert(sizeof(ax7z020_accel_result)==197272,"result ABI layout");
#endif

namespace vibration { namespace accelerator {
namespace {
const double pi = std::acos(-1.0);
double cpu_seconds() {
#ifdef __linux__
    timespec now{};
    if(::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&now)==0)return double(now.tv_sec)+now.tv_nsec/1e9;
#endif
    return double(std::clock())/CLOCKS_PER_SEC;
}
unsigned profile(unsigned fs) {
    switch (fs) { case 50000:return 0; case 25000:return 1; case 10000:return 2; case 5000:return 3; }
    throw Error("unsupported_sample_rate");
}
const std::int32_t* coefficients(unsigned fs) {
    switch (profile(fs)) {
    case 0:return v6_coeff::fir_50000;
    case 1:return v6_coeff::fir_25000;
    case 2:return v6_coeff::fir_10000;
    default:return v6_coeff::fir_5000;
    }
}
std::int64_t round_even_shift(std::int64_t x, unsigned shift) {
    const bool neg = x < 0;
    const std::uint64_t mag = neg ? std::uint64_t(-(x + 1)) + 1 : std::uint64_t(x);
    std::uint64_t q = mag >> shift;
    const std::uint64_t r = mag & ((std::uint64_t(1) << shift) - 1);
    const std::uint64_t half = std::uint64_t(1) << (shift - 1);
    if (r > half || (r == half && (q & 1))) ++q;
    return neg ? -std::int64_t(q) : std::int64_t(q);
}
void fft(std::vector<std::complex<double>>& x) {
    const std::size_t n = x.size();
    for (std::size_t i=1,j=0; i<n; ++i) {
        std::size_t bit=n>>1;
        for (; j&bit; bit>>=1) j^=bit;
        j^=bit;
        if (i<j) std::swap(x[i],x[j]);
    }
    for (std::size_t len=2;len<=n;len<<=1) {
        auto wlen=std::polar(1.0,-2*pi/double(len));
        for (std::size_t i=0;i<n;i+=len) {
            std::complex<double> w(1,0);
            for (std::size_t j=0;j<len/2;++j) {
                auto u=x[i+j],v=x[i+j+len/2]*w;
                x[i+j]=u+v; x[i+j+len/2]=u-v; w*=wlen;
            }
        }
    }
}
std::int32_t hann(unsigned n) {
    return v6_coeff::hann_8192[n];
}
double hann_energy() {
    double e=0;
    for (unsigned n=0;n<kPoints;++n) {
        const double h=double(hann(n))/8388608.0;
        e+=h*h;
    }
    return e;
}
void aggregate(const Request& q, Channel& c) {
    c.reference_power_adc2=0;
    c.fullband_power_adc2=0;
    c.nonharmonic_power_adc2=0;
    c.orders_4_to_10_power_adc2=0;
    for (unsigned bin=0;bin<=3276;++bin) c.fullband_power_adc2+=c.power_adc2[bin];
    for (unsigned bin=4;bin<=819;++bin) {
        c.reference_power_adc2+=c.power_adc2[bin];
        bool in_any=false,in_high=false;
        if (q.bands_valid) for (unsigned b=0;b<kOrders;++b)
            if (bin>=q.order_bands[b].first && bin<=q.order_bands[b].last) {
                c.order_power_adc2[b]+=c.power_adc2[bin];
                in_any=true; if (b>=6) in_high=true;
            }
        if (q.bands_valid && !in_any) c.nonharmonic_power_adc2+=c.power_adc2[bin];
        if (in_high) c.orders_4_to_10_power_adc2+=c.power_adc2[bin];
    }
}
void initialize(const Request& q, Result& r, Backend b) {
    r.backend=b; r.source=backend_name(b);
    r.algorithm_version="v6_fir_q23_q7_periodic_hann8192_fft_v1";
    static const char* const hashes[]={
        "2a194220bdde8914d19a5b2d27826010a66e12ee1d9d2076c314d4da930147f8",
        "d01079a3bb4bd649f4a42a2e85e66f94920cb62453dac3d47d8caa1113df409e",
        "e148455dca88280ff972f7367c84a0f55706a89b322b90060712c97dda526c15",
        "5272fb22695ff46ffcea970833e2f5f786add8f60a66598f2f76e0b3448325a8"};
    r.coefficients_sha256=hashes[profile(q.sample_rate_hz)];
    r.sample_rate_hz=q.sample_rate_hz;
    r.fir_taps=fir_taps(q.sample_rate_hz);
    r.decimation=q.sample_rate_hz/kAnalysisRateHz;
    r.window_seq=q.window_seq;
    r.bands_valid=q.bands_valid;
    for (const auto& frame:q.raw) for (unsigned ch=0;ch<3;++ch)
        if (frame[ch]==-32768 || frame[ch]==32767) ++r.raw_endpoint_count[ch];
}
Result reference(const Request& q) {
    Result r;
    initialize(q,r,Backend::Reference);
    const auto* coeff=coefficients(q.sample_rate_hz);
    const double den=kPoints*hann_energy();
    for (unsigned ch=0;ch<3;++ch) {
        auto& c=r.channels[ch];
        c.lowband_q8.resize(kPoints);
        std::int64_t sum7=0;
        for (unsigned n=0;n<kPoints;++n) {
            const unsigned end=r.fir_taps-1+n*r.decimation;
            std::int64_t acc=0;
            for (unsigned tap=0;tap<r.fir_taps;++tap)
                acc+=std::int64_t(q.raw[end-tap][ch])*coeff[tap];
            std::int64_t sample=round_even_shift(acc,16);
            if (sample < -8388608 || sample > 8388607) {
                ++r.saturation_count;
                sample=std::max<std::int64_t>(-8388608,std::min<std::int64_t>(8388607,sample));
            }
            const auto sample8=std::int32_t(sample*2);
            c.lowband_q8[n]=sample8;
            sum7+=sample;
            c.statistics.sum_q8+=sample8;
            c.statistics.sumsq_q16+=std::uint64_t(std::int64_t(sample8)*sample8);
            if (!n || sample8<c.statistics.minimum_q8) c.statistics.minimum_q8=sample8;
            if (!n || sample8>c.statistics.maximum_q8) c.statistics.maximum_q8=sample8;
        }
        const std::int64_t mean7=round_even_shift(sum7,13);
        c.fft_pre_shift=1;
        std::vector<std::complex<double>> samples(kPoints);
        for (unsigned n=0;n<kPoints;++n) {
            const std::int64_t centered7=c.lowband_q8[n]/2-mean7;
            const auto windowed7=round_even_shift(centered7*hann(n),24);
            samples[n]=double(windowed7)/64.0;
        }
        fft(samples);
        c.power_adc2.resize(kBins);
        for (unsigned bin=0;bin<kBins;++bin)
            c.power_adc2[bin]=std::norm(samples[bin])*(bin==0||bin==kPoints/2?1:2)/den;
        aggregate(q,c);
    }
    return r;
}
#ifdef __linux__
class Fd {
public:
    int value;
    explicit Fd(const std::string& p):value(::open(p.c_str(),O_RDWR|O_CLOEXEC)) {}
    ~Fd(){if(value>=0)::close(value);}
};
std::string os_error(const std::string& op) { return op+": "+std::strerror(errno); }
bool caps_valid(const ax7z020_accel_caps& c) {
    return c.abi_version==AX7Z020_ACCEL_ABI && c.hardware_id==AX7Z020_ACCEL_HW_ID &&
        c.revision==ACC_EXPECTED_REVISION && c.points==kPoints && c.bins==kBins &&
        c.rate_mask==15 && c.max_raw_frames>=required_raw_frames(50000);
}
Result hardware(const Request& q,const std::string& device) {
    Fd fd(device);
    if (fd.value<0) throw Error(os_error("hardware_unavailable open "+device));
    ax7z020_accel_caps caps{};
    if (::ioctl(fd.value,AX7Z020_ACCEL_GET_CAPS,&caps)<0) throw Error(os_error("get_caps"));
    if (!caps_valid(caps)) throw Error("hardware_identity_or_abi_mismatch");
    if (::ioctl(fd.value,AX7Z020_ACCEL_RESET)<0) throw Error(os_error("reset"));
    std::vector<std::uint8_t> raw(q.raw.size()*6);
    for (std::size_t i=0;i<q.raw.size();++i) for(unsigned ch=0;ch<3;++ch) {
        const auto v=std::uint16_t(q.raw[i][ch]);
        raw[i*6+ch*2]=v&255;raw[i*6+ch*2+1]=v>>8;
    }
    std::size_t sent=0;
    while(sent<raw.size()) {
        const auto n=::write(fd.value,raw.data()+sent,raw.size()-sent);
        if(n<0) { if(errno==EINTR)continue;throw Error(os_error("input_write")); }
        if(n==0 || n%6) throw Error("short_or_unaligned_input_write");
        sent+=std::size_t(n);
    }
    ax7z020_accel_plan plan{};
    plan.abi_version=AX7Z020_ACCEL_ABI;plan.sample_rate_hz=q.sample_rate_hz;
    plan.raw_frames=q.raw.size();plan.timeout_ms=q.timeout_ms;
    plan.flags=q.bands_valid?AX7Z020_ACCEL_PLAN_BANDS_VALID:0;
    for(unsigned i=0;i<kOrders;++i) {plan.bands[i].first=q.order_bands[i].first;plan.bands[i].last=q.order_bands[i].last;}
    if(::ioctl(fd.value,AX7Z020_ACCEL_RUN,&plan)<0) throw Error(os_error("PL_transaction"));
    auto wire=std::unique_ptr<ax7z020_accel_result>(new ax7z020_accel_result{});
    std::size_t received=0;
    while(received<sizeof(*wire)) {
        const auto n=::read(fd.value,reinterpret_cast<char*>(wire.get())+received,sizeof(*wire)-received);
        if(n<0) {if(errno==EINTR)continue;throw Error(os_error("result_read"));}
        if(!n)throw Error("truncated_PL_result");
        received+=std::size_t(n);
    }
    if(wire->abi_version!=AX7Z020_ACCEL_ABI || wire->hardware_id!=AX7Z020_ACCEL_HW_ID ||
       wire->revision!=ACC_EXPECTED_REVISION || wire->sample_rate_hz!=q.sample_rate_hz ||
       wire->points!=kPoints || wire->fir_taps!=fir_taps(q.sample_rate_hz) ||
       wire->decimation!=q.sample_rate_hz/5000 || wire->error_code)
        throw Error("PL_result_identity_or_shape_mismatch");
    Result r;initialize(q,r,Backend::Hardware);
    r.hardware_id=wire->hardware_id;r.revision=wire->revision;
    r.pl_cycles=wire->cycles;r.saturation_count=wire->saturation_count;
    const double den=kPoints*hann_energy();
    for(unsigned ch=0;ch<3;++ch) {
        const auto& w=wire->channel[ch];auto& c=r.channels[ch];
        if(w.fft_exponent>31 || w.fft_pre_shift>16)throw Error("invalid_FFT_scale");
        if(w.raw_endpoint_count!=r.raw_endpoint_count[ch])throw Error("raw_endpoint_count_mismatch");
        c.fft_exponent=w.fft_exponent;c.fft_pre_shift=w.fft_pre_shift;
        if(w.sum_q7<-68719476736LL || w.sum_q7>68719476736LL ||
           w.sumsq_q14>(std::uint64_t(1)<<59) ||
           w.minimum_q7<-8388608 || w.minimum_q7>8388607 ||
           w.maximum_q7<-8388608 || w.maximum_q7>8388607)
            throw Error("invalid_PL_statistics_range");
        c.statistics.sum_q8=w.sum_q7*2;c.statistics.sumsq_q16=w.sumsq_q14*4;
        c.statistics.minimum_q8=w.minimum_q7*2;c.statistics.maximum_q8=w.maximum_q7*2;
        c.lowband_q8.reserve(kPoints);
        Statistics check{};
        for(unsigned n=0;n<kPoints;++n) {
            if(w.lowband_q7[n]<-8388608 || w.lowband_q7[n]>8388607)throw Error("invalid_lowband_word");
            const auto v=w.lowband_q7[n]*2;c.lowband_q8.push_back(v);
            check.sum_q8+=v;check.sumsq_q16+=std::uint64_t(std::int64_t(v)*v);
            if(!n || v<check.minimum_q8)check.minimum_q8=v;
            if(!n || v>check.maximum_q8)check.maximum_q8=v;
        }
        if(check.sum_q8!=c.statistics.sum_q8 || check.sumsq_q16!=c.statistics.sumsq_q16 ||
           check.minimum_q8!=c.statistics.minimum_q8 || check.maximum_q8!=c.statistics.maximum_q8)
            throw Error("PL_statistics_do_not_match_lowband_payload");
        // PL returns integer |FFT|^2; Xadc = Xinteger*2^(BFP+pre_shift)/128.
        const double scale=std::ldexp(1.0,2*int(c.fft_exponent+c.fft_pre_shift)-14)/den;
        c.power_adc2.resize(kBins);
        for(unsigned bin=0;bin<kBins;++bin)
            c.power_adc2[bin]=double(w.fft_magnitude_squared[bin])*scale*(bin==0||bin==4096?1:2);
        std::array<std::uint64_t,18> expected{};
        for(unsigned bin=0;bin<=3276;++bin) {
            const auto power=w.fft_magnitude_squared[bin];
            if(power>(std::uint64_t(1)<<48))throw Error("PL_FFT_power_outside_24bit_complex_range");
            const auto weighted=power*(bin==0?1:2);
            expected[14]+=weighted;
            if(bin<4 || bin>819)continue;
            expected[13]+=weighted;
            bool any=false,high=false;
            if(q.bands_valid)for(unsigned b=0;b<kOrders;++b)
                if(bin>=q.order_bands[b].first && bin<=q.order_bands[b].last) {
                    expected[b]+=weighted;any=true;if(b>=6)high=true;
                }
            if(any)expected[15]+=weighted;
            if(high)expected[16]+=weighted;
            if(!any)expected[17]+=weighted;
        }
        for(unsigned b=0;b<18;++b)
            if(expected[b]!=w.band_magnitude_squared[b])throw Error("PL_band_accumulator_mismatch_bin_payload");
        // PL band accumulators already include the one-sided bin weights.
        for(unsigned b=0;b<kOrders;++b)c.order_power_adc2[b]=q.bands_valid?double(w.band_magnitude_squared[b])*scale:0;
        c.reference_power_adc2=double(w.band_magnitude_squared[13])*scale;
        c.fullband_power_adc2=double(w.band_magnitude_squared[14])*scale;
        c.orders_4_to_10_power_adc2=q.bands_valid?double(w.band_magnitude_squared[16])*scale:0;
        c.nonharmonic_power_adc2=q.bands_valid?double(w.band_magnitude_squared[17])*scale:0;
    }
    return r;
}
#else
Result hardware(const Request&,const std::string&) { throw Error("hardware_unavailable: Linux driver required"); }
#endif
} // namespace

const char* backend_name(Backend b) {
    if(b==Backend::Hardware)return "pl_hardware_ps_mmio_transport";
#if defined(__arm__) || defined(__aarch64__)
    return "ps_reference";
#else
    return "host_reference";
#endif
}
unsigned fir_taps(unsigned fs) {static const unsigned taps[]={641,321,129,65};return taps[profile(fs)];}
unsigned required_raw_frames(unsigned fs) {return fir_taps(fs)+(kPoints-1)*(fs/5000);}
void validate_request(const Request& q) {
    const auto count=required_raw_frames(q.sample_rate_hz);
    if(q.raw.size()!=count)throw Error("raw_frame_count_must_equal_"+std::to_string(count));
    if(q.timeout_ms<10 || q.timeout_ms>30000)throw Error("timeout_out_of_range_10_to_30000_ms");
    if(q.bands_valid)for(unsigned i=0;i<kOrders;++i) {
        const auto& b=q.order_bands[i];
        if(b.first<4 || b.last>819 || b.first>b.last)throw Error("order_band_outside_2_to_500_Hz_reference");
        for(unsigned j=0;j<i;++j)if(b.first<=q.order_bands[j].last && q.order_bands[j].first<=b.last)
            throw Error("overlapping_order_bands");
    }
}
Capabilities probe(const std::string& device) {
    Capabilities r;r.device=device;
#ifdef __linux__
    Fd fd(device);
    if(fd.value<0) {r.reason=os_error("hardware_unavailable");return r;}
    ax7z020_accel_caps caps{};
    if(::ioctl(fd.value,AX7Z020_ACCEL_GET_CAPS,&caps)<0) {r.reason=os_error("get_caps");return r;}
    r.hardware_id=caps.hardware_id;r.revision=caps.revision;r.clock_hz=caps.clock_hz;
    r.available=caps_valid(caps);r.reason=r.available?"matched_v6_hardware":"hardware_identity_or_abi_mismatch";
#else
    r.reason="hardware_unavailable: Linux driver required";
#endif
    return r;
}
Result run(const Request& q,Backend b,const std::string& device) {
    validate_request(q);
    const auto start=std::chrono::steady_clock::now();const auto cpu=cpu_seconds();
    Result r=b==Backend::Hardware?hardware(q,device):reference(q);
    r.elapsed_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    r.cpu_ms=1000.0*(cpu_seconds()-cpu);
    if(r.saturation_count)r.reasons.push_back("arithmetic_saturation");
    for(unsigned ch=0;ch<3;++ch)if(r.raw_endpoint_count[ch])r.reasons.push_back("raw_ADC_endpoint_CH"+std::to_string(ch+1));
    // Speed, source continuity and calibration remain the caller's quality gates.
    r.valid=r.reasons.empty();
    return r;
}
}} // namespace vibration::accelerator
