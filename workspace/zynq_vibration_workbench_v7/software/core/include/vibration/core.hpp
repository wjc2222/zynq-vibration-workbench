#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace vibration {

class AnalysisError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Header {
    std::uint32_t version{}, header_bytes{}, record_bytes{}, state{};
    std::uint32_t sample_rate_hz{}, clock_hz{}, requested_records{}, completed_records{};
    std::uint32_t payload_bytes{}, error_code{}, hw_status{}, dma_status{};
    std::uint32_t dropped_records{}, pulses_per_rev{};
    std::uint64_t first_tick{}, last_tick{};
    std::uint32_t channel_map{};
    std::int32_t adc_range_uv{};
    std::uint32_t speed_filter_cycles{}, speed_timeout_cycles{};
    std::uint32_t payload_crc32{}, header_crc32{};
    // CAPSTRM1 segments preserve session-global indices in their raw records.
    bool stream_segment = false;
    std::uint64_t global_first_index{}, segment_index{}, session_total_records{};
    std::array<std::uint8_t, 16> session_id{};
    std::uint32_t segment_target_records{}, stream_flags{};
};

struct Record {
    std::uint64_t index{}, tick{};
    std::array<std::int16_t, 3> raw{};
    std::uint16_t flags{};
    std::uint32_t speed_period_ticks{}, speed_edge_count{};
};

struct LoadOptions {
    // The delivered firmware owns a 64 MiB capture region including its header.
    std::uint64_t max_file_bytes = 64ULL * 1024 * 1024;
};

struct SourceMetadata {
    bool synthetic = false;
    // A sidecar is a source declaration, never proof of a physical measurement.
    std::string status = "not_provided";
};

struct Capture {
    Header header;
    std::vector<Record> records;
    std::string source_path;
    // SHA256 of the exact loaded header and payload bytes, after CRC passes.
    // Empty for live snapshots or programmatically constructed test captures.
    std::string capture_sha256;
    SourceMetadata source_metadata;
    bool integrity_verified = false;
};

enum class SignalKind { Unknown, TestSignal, VibrationConfirmed };

enum class AmplitudeUnit { Voltage, AccelerationSI, Gravity };
inline constexpr double standard_gravity_m_s2 = 9.80665;

// User-entered conversion parameters are not evidence of physical calibration.
// Bias is an explicitly supplied voltage zero, never an estimated record mean.
struct ChannelCalibration {
    double sensitivity_mv_per_m_s2 = 1000.0 * 0.5 / standard_gravity_m_s2;
    double analog_gain = -1.0;
    double voltage_bias_v = 0.0;
};

struct AnalysisConfig {
    // Only used if the file declares PPR=0; conflicting nonzero values are errors.
    std::optional<double> ppr_override;
    double sensor_sensitivity_v_per_g = 0.5;
    double analog_gain = -1.0;
    // Unset retains the legacy common V/g sensitivity and signed gain above.
    std::optional<std::array<ChannelCalibration, 3>> channel_calibration;
    AmplitudeUnit amplitude_unit = AmplitudeUnit::Gravity;
    bool calibration_confirmed = false;
    SignalKind signal_kind = SignalKind::Unknown;
    std::size_t fft_size = 65536;
    double spectrum_max_hz = 2000.0;
    std::size_t waveform_points = 1200;
    // Execution policy only; does not change analysis definitions or JSON.
    // Small FFTs and single-segment records retain the serial path.
    unsigned execution_threads = 2;
};

// Channel indices are zero-based. All three sensitivities must be positive and
// finite; signed gains must be finite and nonzero. Voltage bypasses conversion.
void validate_calibration_config(const AnalysisConfig& config);
ChannelCalibration effective_channel_calibration(const AnalysisConfig& config, std::size_t channel);
double voltage_to_amplitude(double voltage, std::size_t channel, const AnalysisConfig& config);
double adc_to_amplitude(std::int16_t raw, std::int32_t adc_range_uv,
                        std::size_t channel, const AnalysisConfig& config);
std::string amplitude_unit_symbol(const AnalysisConfig& config);
// Includes the amplitude unit and exact active conversion parameters. Does not
// compare calibration confirmation, source declarations or FFT policy.
bool calibration_config_equal(const AnalysisConfig& a, const AnalysisConfig& b);

struct TimeStats {
    double mean{}, rms{}, ac_rms{}, abs_peak{}, peak_to_peak{};
    std::optional<double> crest_factor, pearson_kurtosis;
    std::uint64_t adc_endpoint_count{};
};

struct Spectrum {
    std::size_t fft_size{}, segment_count{};
    double bin_width_hz{}, window_duration_s{}, enbw_hz{};
    std::vector<double> frequency_hz, peak_amplitude, psd;
};

struct OrderBand {
    int order{};
    std::optional<double> center_hz, half_width_hz, rms;
    std::string status;
};

struct SpeedPoint {
    double time_s{};
    std::optional<double> pulse_frequency_hz, rpm;
};

struct SpeedStats {
    std::optional<double> pulses_per_rev;
    std::string ppr_source;
    double valid_fraction{}, timeout_fraction{};
    std::uint64_t observable_new_edges{};
    std::optional<double> held_mean_pulse_hz, held_mean_rpm, held_std_rpm;
    std::optional<double> fresh_period_mean_rpm, min_rpm, max_rpm;
    // At most 6 * waveform_points display points. Multiple missing runs in a
    // bucket hide the valid fragments between them, never connect over a gap.
    bool trace_gaps_coalesced = false;
    std::vector<SpeedPoint> trace;
};

struct WaveformPoint {
    double time_s{};
    std::array<double, 3> values{};
};

struct Diagnosis {
    std::string status;
    std::string classification;
    std::vector<std::string> evidence;
    std::vector<std::string> limitations;
};

struct AnalysisResult {
    std::string source_path;
    SourceMetadata source_metadata;
    Header header;
    AnalysisConfig config;
    std::string amplitude_unit;
    double duration_s{};
    std::array<TimeStats, 3> time_stats;
    std::array<Spectrum, 3> spectra;
    std::array<std::array<OrderBand, 3>, 3> order_bands;
    SpeedStats speed;
    std::vector<WaveformPoint> waveform;
    Diagnosis diagnosis;
    std::vector<std::string> warnings;
};

// Public path strings are UTF-8 on every platform, including Windows.
Capture load_capture(const std::string& path, const LoadOptions& options = {});
AnalysisResult analyze(const Capture& capture, const AnalysisConfig& config = {});
AnalysisResult analyze_file(const std::string& path, const AnalysisConfig& config = {},
                            const LoadOptions& options = {});
// Numerical ADC-voltage spectra for an already transported raw snapshot. This
// does not establish complete-file integrity, source, calibration or diagnosis.
// Input must be contiguous, <=2 s and <=32768 records; FFT <=16384 points.
std::array<Spectrum, 3> analyze_voltage_spectra(const std::vector<Record>& records,
    std::uint32_t sample_rate_hz, std::uint32_t clock_hz, std::int32_t adc_range_uv,
    std::size_t fft_size = 8192, double max_hz = 2000.0);
std::string to_json(const AnalysisResult& result);

// IEEE reflected CRC32, equivalent to Python zlib.crc32(data).
std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

} // namespace vibration
