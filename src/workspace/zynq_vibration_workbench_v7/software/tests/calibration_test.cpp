#include "vibration/core.hpp"
#include "vibration/detection.hpp"
#include "../gui/view_math.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>

namespace {
unsigned checks = 0;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); ++checks; }
void near(double actual, double expected, const char* message, double tolerance = 1e-9) {
    check(std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected)), message);
}
void rejects(const std::function<void()>& action, const char* message) {
    bool rejected = false; try { action(); } catch (const vibration::AnalysisError&) { rejected = true; } check(rejected, message);
}
vibration::Capture fixture() {
    vibration::Capture capture; capture.integrity_verified = true; capture.source_path = "software-calibration-QA.BIN";
    capture.header.sample_rate_hz = 1024; capture.header.clock_hz = 1024000;
    capture.header.adc_range_uv = 3276800; capture.header.channel_map = 0x00050301;
    capture.header.pulses_per_rev = 1; capture.header.completed_records = 1024; capture.records.resize(1024);
    for (std::size_t i = 0; i < capture.records.size(); ++i) {
        auto& record = capture.records[i]; record.index = i; record.tick = i * 1000;
        const auto raw = std::int16_t(std::lround(1000 + 400 * std::sin(2 * 3.141592653589793 * 32 * i / 1024)));
        record.raw = {raw, raw, raw}; record.flags = 3; record.speed_period_ticks = 32000; record.speed_edge_count = std::uint32_t(i / 32);
    }
    return capture;
}
} // namespace
int main() {
    using namespace vibration;
    try {
        AnalysisConfig config; config.fft_size = 1024; config.spectrum_max_hz = 512; config.execution_threads = 1;
        config.amplitude_unit = AmplitudeUnit::AccelerationSI;
        config.channel_calibration = std::array<ChannelCalibration, 3>{{{2, 1, .025}, {4, 2, .05}, {8, -1, .075}}};
        near(voltage_to_amplitude(.012, 0, AnalysisConfig{config}), -6.5, "Explicit voltage zero conversion");
        auto zeroBias = config; (*zeroBias.channel_calibration)[0].voltage_bias_v = 0;
        near(voltage_to_amplitude(.012, 0, zeroBias), 6, "2 mV/(m/s2) converts millivolts correctly");
        near(voltage_to_amplitude(.012, 1, zeroBias), -4.75, "CH2 uses independent gain/sensitivity/zero");
        near(voltage_to_amplitude(.012, 2, zeroBias), 7.875, "CH3 negative gain preserves polarity");
        const auto input = fixture(); const auto original = input.records;
        auto voltageConfig = config; voltageConfig.amplitude_unit = AmplitudeUnit::Voltage;
        const auto volts = analyze(input, voltageConfig); const auto si = analyze(input, config);
        near(si.time_stats[0].mean, 37.5, "SI time domain retains DC except explicit bias");
        near(si.time_stats[1].mean, 6.25, "CH2 DC conversion"); near(si.time_stats[2].mean, -3.125, "CH3 DC conversion");
        near(volts.time_stats[0].mean, .1, "Voltage bypasses configured bias and gain");
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double slope = channel == 0 ? 500 : 125;
            near(si.time_stats[channel].ac_rms, volts.time_stats[channel].ac_rms * slope, "Time RMS independent-channel scaling");
            near(si.spectra[channel].peak_amplitude[32], volts.spectra[channel].peak_amplitude[32] * slope, "FFT peak independent-channel scaling");
            near(si.spectra[channel].psd[32], volts.spectra[channel].psd[32] * slope * slope, "PSD squared-unit scaling");
            check(si.order_bands[channel][0].rms.has_value(), "Order band available");
            near(*si.order_bands[channel][0].rms, *volts.order_bands[channel][0].rms * slope, "Order RMS uses same physical unit");
        }
        auto gravityConfig = config; gravityConfig.amplitude_unit = AmplitudeUnit::Gravity;
        const auto gravity = analyze(input, gravityConfig);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            near(gravity.time_stats[channel].rms * standard_gravity_m_s2, si.time_stats[channel].rms, "g-to-SI time RMS consistency");
            near(gravity.spectra[channel].peak_amplitude[32] * standard_gravity_m_s2, si.spectra[channel].peak_amplitude[32], "g-to-SI FFT consistency");
            near(gravity.spectra[channel].psd[32] * standard_gravity_m_s2 * standard_gravity_m_s2, si.spectra[channel].psd[32], "g-to-SI PSD consistency");
        }
        const auto preview = gui::waveform_preview(input, config, 0, .2, 1200);
        const auto cursor = gui::waveform_cursor(input, config, 0);
        near(cursor.values[0], 37.5, "Waveform cursor SI conversion");
        near(preview.front().values[2], -3.125, "Preview CH3 conversion matches statistics");
        check(!si.config.calibration_confirmed && si.amplitude_unit == "m/s^2_nominal_uncalibrated", "User parameters are not physical calibration");
        const auto json = to_json(si); check(json.find("\"sensitivity_mv_per_m_s2\":2") != std::string::npos, "Result serializes per-channel calibration");
        check(json.find("user_configuration_not_measurement") != std::string::npos, "Result explains calibration provenance");
        for (std::size_t i = 0; i < input.records.size(); ++i) check(input.records[i].raw == original[i].raw, "Analysis mutated original ADC records");
        auto bad = config; (*bad.channel_calibration)[1].analog_gain = 0;
        rejects([&]{ analyze(input, bad); }, "Zero channel gain accepted");
        bad = config; (*bad.channel_calibration)[2].sensitivity_mv_per_m_s2 = -2;
        rejects([&]{ analyze(input, bad); }, "Negative channel sensitivity accepted");
        bad = config; (*bad.channel_calibration)[0].sensitivity_mv_per_m_s2 = std::numeric_limits<double>::quiet_NaN();
        rejects([&]{ analyze(input, bad); }, "NaN channel sensitivity accepted");
        bad = config; (*bad.channel_calibration)[0].analog_gain = std::numeric_limits<double>::infinity();
        rejects([&]{ analyze(input, bad); }, "Infinite channel gain accepted");
        bad = config; (*bad.channel_calibration)[2].voltage_bias_v = std::numeric_limits<double>::infinity();
        rejects([&]{ analyze(input, bad); }, "Infinite explicit bias accepted");
        rejects([&]{ voltage_to_amplitude(1, 3, config); }, "Invalid channel accepted");
        AnalysisConfig legacy; legacy.fft_size = 1024; legacy.spectrum_max_hz = 512;
        near(voltage_to_amplitude(.012, 1, legacy), -.024, "Legacy common V/g gain remains compatible");
        check(analyze(input, legacy).amplitude_unit == "g_nominal_uncalibrated", "Legacy nominal unit changed");
        auto confirmed = config; confirmed.calibration_confirmed = true; confirmed.signal_kind = SignalKind::VibrationConfirmed;
        const auto measured = analyze(input, confirmed);
        auto profile = make_baseline_draft(measured, "software-qa", "si-test");
        profile.rules = {{"test-rule", "normal", {{"ac_rms", 1, "absolute", measured.time_stats[0].ac_rms * .99, measured.time_stats[0].ac_rms * 1.01}}}};
        const auto text = detection_profile_text(profile); const auto parsed = parse_detection_profile(text);
        check(text == detection_profile_text(parsed), "SI/channel profile round trip changed configuration");
        check(evaluate_detection(measured, parsed, "software-qa").status == "candidate", "Same-unit SI threshold failed");
        auto changed = confirmed; changed.amplitude_unit = AmplitudeUnit::Gravity;
        const auto mismatch = evaluate_detection(analyze(input, changed), profile, "software-qa");
        check(mismatch.status == "quality_gated", "SI threshold applied to g feature");
        check(mismatch.amplitude_unit == "g" && mismatch.baseline_amplitude_unit == "m/s^2", "Detection lost current/baseline physical units");
        for (const auto& comparison : mismatch.baseline_comparisons)
            check(!comparison.delta && !comparison.ratio && comparison.status == "calibration_configuration_mismatch", "Mismatched-unit delta/ratio was computed");
        check(!mismatch.rules[0].conditions[0].evaluated, "Mismatched-unit threshold was evaluated");
        changed = confirmed; (*changed.channel_calibration)[2].sensitivity_mv_per_m_s2 = 9;
        check(evaluate_detection(analyze(input, changed), profile, "software-qa").status == "quality_gated", "CH3 sensitivity mismatch did not gate");
        changed = confirmed; changed.amplitude_unit = AmplitudeUnit::Voltage;
        const auto voltageAnalysis = analyze(input, changed); const auto voltageProfile = make_baseline_draft(voltageAnalysis, "software-qa", "v-test");
        check(evaluate_detection(voltageAnalysis, voltageProfile, "software-qa").status == "quality_gated", "Voltage feature promoted to calibrated acceleration");
        auto legacyAnalysis = analyze(input, legacy); auto legacyProfile = make_baseline_draft(legacyAnalysis, "software-qa", "old-test");
        auto oldText = detection_profile_text(legacyProfile);
        for (const std::string line : {"amplitude_unit=g\n", "channel_calibration=legacy_global\n"}) {
            const auto position = oldText.find(line); check(position != std::string::npos, "Legacy extension missing"); oldText.erase(position, line.size());
        }
        const auto oldProfile = parse_detection_profile(oldText);
        check(!oldProfile.channel_calibration && oldProfile.amplitude_unit == AmplitudeUnit::Gravity, "Old profile did not migrate to legacy global g");
        std::cout << "PASS " << checks << " calibration and unit-consistency checks; software synthetic fixture only\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
