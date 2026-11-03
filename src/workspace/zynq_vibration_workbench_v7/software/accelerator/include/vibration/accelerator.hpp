#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace vibration { namespace accelerator {
constexpr unsigned kPoints = 8192;
constexpr unsigned kBins = kPoints / 2 + 1;
constexpr unsigned kOrders = 13;
constexpr unsigned kAnalysisRateHz = 5000;
enum class Backend { Hardware, Reference };
class Error : public std::runtime_error { public: using std::runtime_error::runtime_error; };
struct Band { std::uint32_t first = 0, last = 0; };
struct Request {
    unsigned sample_rate_hz = 50000;
    // Exactly required_raw_frames(fs), chronological, CH1/CH2/CH3 signed ADC codes.
    // Input includes complete FIR history. Each transaction starts fresh.
    std::vector<std::array<std::int16_t, 3>> raw;
    std::array<Band, kOrders> order_bands{};
    bool bands_valid = false;
    unsigned timeout_ms = 10000;
    std::uint64_t window_seq = 0;
};
struct Statistics {
    std::int64_t sum_q8 = 0;
    std::uint64_t sumsq_q16 = 0;
    std::int32_t minimum_q8 = 0, maximum_q8 = 0;
};
struct Channel {
    // PL FIR has Q7 resolution; the API expands it exactly by one bit to Q8.
    std::vector<std::int32_t> lowband_q8;
    Statistics statistics;
    // One-sided per-bin mean-square ADC-code power. Sum over bins is the
    // Hann-energy-normalized mean-square of this window, not peak amplitude.
    // P[k] = (k==0 || k==N/2 ? 1 : 2)*|FFT((x-mean)*hann)[k]|^2/(N*sum(hann^2)).
    std::vector<double> power_adc2;
    std::array<double, kOrders> order_power_adc2{};
    double reference_power_adc2 = 0, nonharmonic_power_adc2 = 0;
    double orders_4_to_10_power_adc2 = 0, fullband_power_adc2 = 0;
    unsigned fft_exponent = 0;
    unsigned fft_pre_shift = 0;
};
struct Result {
    Backend backend = Backend::Reference;
    std::string source, algorithm_version, coefficients_sha256;
    unsigned hardware_id = 0, revision = 0;
    unsigned sample_rate_hz = 0, analysis_sample_rate_hz = kAnalysisRateHz;
    unsigned fir_taps = 0, decimation = 0;
    std::uint64_t window_seq = 0;
    std::array<Channel, 3> channels;
    std::array<unsigned, 3> raw_endpoint_count{};
    std::uint64_t saturation_count = 0;
    bool bands_valid = false;
    double elapsed_ms = 0, cpu_ms = 0;
    std::uint64_t pl_cycles = 0;
    // No silent fallback: Hardware errors throw Error. Reference is explicit.
    bool valid = false;
    std::vector<std::string> reasons;
};
struct Capabilities {
    bool available = false;
    unsigned hardware_id = 0, revision = 0, clock_hz = 0;
    std::string device, reason;
};
unsigned required_raw_frames(unsigned sample_rate_hz);
unsigned fir_taps(unsigned sample_rate_hz);
void validate_request(const Request& request);
Capabilities probe(const std::string& device = "/dev/vibration-accel");
Result run(const Request& request, Backend backend,
           const std::string& device = "/dev/vibration-accel");
const char* backend_name(Backend backend);
}} // namespace vibration::accelerator
