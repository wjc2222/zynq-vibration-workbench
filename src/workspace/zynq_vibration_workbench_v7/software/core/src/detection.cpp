#include "vibration/detection.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vibration {
namespace {
constexpr std::size_t kMaxBytes = 65536, kMaxRules = 32, kMaxConditions = 16, kMaxFeatures = 30;
void require(bool ok, const std::string& message) {
    if (!ok) throw AnalysisError("Detection: " + message);
}
bool finite(double v) { return std::isfinite(v) && std::abs(v) <= 1e100; }
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}
bool identifier(const std::string& s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}
void text_field(const std::string& s, std::size_t max_size, bool empty = false) {
    require((empty || !s.empty()) && s.size() <= max_size && s == trim(s), "invalid or oversized text field");
    require(std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; }), "control character in text field");
}
void utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        if (c < 128) { require(c != 0, "NUL is not allowed"); continue; }
        unsigned count; std::uint32_t value, minimum;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7; minimum = 0x10000; }
        else { require(false, "invalid UTF-8"); return; }
        require(s.size() - i >= count, "truncated UTF-8");
        for (unsigned j = 0; j < count; ++j) {
            c = static_cast<unsigned char>(s[i++]);
            require((c & 0xc0) == 0x80, "invalid UTF-8 continuation"); value = (value << 6) | (c & 63);
        }
        require(value >= minimum && value <= 0x10ffff && (value < 0xd800 || value > 0xdfff), "invalid UTF-8 code point");
    }
}
bool valid_feature(const std::string& s) {
    static const std::set<std::string> names{"mean", "rms", "ac_rms", "abs_peak", "peak_to_peak", "crest_factor",
        "pearson_kurtosis", "order1_rms", "order2_rms", "order3_rms"};
    return names.count(s) != 0;
}
bool valid_label(const std::string& s) {
    return s == "normal" || s == "imbalance" || s == "misalignment" || s == "looseness";
}
std::string signal_name(SignalKind kind) {
    return kind == SignalKind::VibrationConfirmed ? "vibration" : kind == SignalKind::TestSignal ? "test" : "unknown";
}
std::optional<double> feature(const AnalysisResult& a, unsigned channel, const std::string& name) {
    require(channel >= 1 && channel <= 3 && valid_feature(name), "invalid feature reference");
    const auto& s = a.time_stats[channel - 1];
    std::optional<double> v;
    if (name == "mean") v = s.mean;
    else if (name == "rms") v = s.rms;
    else if (name == "ac_rms") v = s.ac_rms;
    else if (name == "abs_peak") v = s.abs_peak;
    else if (name == "peak_to_peak") v = s.peak_to_peak;
    else if (name == "crest_factor") v = s.crest_factor;
    else if (name == "pearson_kurtosis") v = s.pearson_kurtosis;
    else {
        const auto& band = a.order_bands[channel - 1][static_cast<unsigned>(name[5] - '1')];
        if (band.status == "measured_band_rms") v = band.rms;
    }
    return v && finite(*v) && (name == "mean" || *v >= 0) ? v : std::nullopt;
}
std::optional<double> baseline(const DetectionProfile& p, unsigned channel, const std::string& name) {
    for (const auto& b : p.baseline) if (b.channel == channel && b.feature == name) return b.value;
    return std::nullopt;
}
AnalysisConfig calibration_config(const DetectionProfile& p) {
    AnalysisConfig cfg;
    cfg.sensor_sensitivity_v_per_g = p.sensor_sensitivity_v_per_g;
    cfg.analog_gain = p.analog_gain;
    cfg.channel_calibration = p.channel_calibration;
    cfg.amplitude_unit = p.amplitude_unit;
    return cfg;
}
void validate(const DetectionProfile& p) {
    require(p.schema == "zynq_vibration.detection_profile.v1", "unsupported profile schema");
    require(identifier(p.id) && identifier(p.setup_id), "profile id and setup_id must be 1..128 ASCII letters, digits, dot, dash or underscore");
    require(p.status == "draft" || p.status == "experimental", "profile status must be draft or experimental; validated profiles are not supported");
    text_field(p.source_path, 4096); text_field(p.source_metadata_status, 128);
    require(p.source_metadata_status == "not_provided" || p.source_metadata_status == "synthetic_sidecar" ||
            p.source_metadata_status == "unconfirmed_sidecar" || p.source_metadata_status == "invalid_or_unreadable_sidecar", "invalid source metadata status");
    require(p.source_kind == "unknown" || p.source_kind == "test" || p.source_kind == "vibration", "invalid source kind");
    require(finite(p.sensor_sensitivity_v_per_g) && p.sensor_sensitivity_v_per_g > 0, "invalid sensor sensitivity");
    require(finite(p.analog_gain) && std::abs(p.analog_gain) > 1e-12, "invalid analog gain");
    validate_calibration_config(calibration_config(p));
    require(p.sample_rate_hz > 0 && p.adc_range_uv > 0, "invalid sample rate or ADC range");
    require(p.fft_size >= 16 && p.fft_size <= 262144 && !(p.fft_size & (p.fft_size - 1)), "invalid requested FFT size");
    require(p.actual_fft_size == 0 || (p.actual_fft_size <= p.fft_size && p.actual_fft_size >= 16 && !(p.actual_fft_size & (p.actual_fft_size - 1))), "invalid actual FFT size");
    require(finite(p.spectrum_max_hz) && p.spectrum_max_hz > 0 && p.min_records > 0, "invalid analysis range or record minimum");
    auto probability = [](double v) { return finite(v) && v >= 0 && v <= 1; };
    require(probability(p.min_speed_valid_fraction) && probability(p.max_speed_timeout_fraction), "invalid speed quality fraction");
    require(!p.pulses_per_rev || (finite(*p.pulses_per_rev) && *p.pulses_per_rev >= 1e-6 && *p.pulses_per_rev <= 1e9), "invalid PPR");
    require(bool(p.speed_rpm_min) == bool(p.speed_rpm_max), "both speed range endpoints are required");
    require(!p.speed_rpm_min || (finite(*p.speed_rpm_min) && finite(*p.speed_rpm_max) && *p.speed_rpm_min > 0 && *p.speed_rpm_min <= *p.speed_rpm_max), "invalid speed interval");
    require(!p.max_speed_cv || (finite(*p.max_speed_cv) && *p.max_speed_cv >= 0), "invalid speed CV limit");
    require(p.baseline.size() <= kMaxFeatures && p.rules.size() <= kMaxRules, "profile entry limit exceeded");
    std::set<std::pair<unsigned, std::string>> unique;
    for (const auto& b : p.baseline) {
        require(b.channel >= 1 && b.channel <= 3 && valid_feature(b.feature) && finite(b.value) &&
                (b.feature == "mean" || b.value >= 0), "invalid baseline feature or negative magnitude/statistic");
        require(unique.emplace(b.channel, b.feature).second, "duplicate baseline feature");
    }
    std::set<std::string> ids;
    for (const auto& r : p.rules) {
        require(identifier(r.id) && ids.insert(r.id).second && valid_label(r.label), "invalid or duplicate rule id/label");
        require(!r.conditions.empty() && r.conditions.size() <= kMaxConditions, "rule must have 1..16 all-of conditions");
        std::set<std::string> conditions;
        for (const auto& c : r.conditions) {
            require(c.channel >= 1 && c.channel <= 3 && valid_feature(c.feature), "invalid rule feature");
            require(c.comparison == "absolute" || c.comparison == "delta" || c.comparison == "ratio", "invalid comparison");
            require(finite(c.minimum) && finite(c.maximum) && c.minimum <= c.maximum, "invalid rule interval");
            require(conditions.insert(std::to_string(c.channel) + ":" + c.feature + ":" + c.comparison).second, "duplicate rule condition");
            require(c.comparison == "absolute" || bool(baseline(p, c.channel, c.feature)), "relative rule requires matching baseline feature");
        }
    }
}
double number(const std::string& s) {
    require(!s.empty() && s.size() <= 128, "invalid numeric field");
    std::istringstream in(s); in.imbue(std::locale::classic()); double v = 0;
    in >> std::noskipws >> v;
    require(!in.fail() && in.peek() == std::char_traits<char>::eof() && finite(v), "invalid finite number: " + s);
    return v;
}
std::uint64_t unsigned_integer(const std::string& s, std::uint64_t maximum) {
    require(!s.empty() && s.size() <= 20 && (s == "0" || s[0] != '0'), "invalid unsigned integer");
    std::uint64_t v = 0;
    for (char c : s) {
        require(c >= '0' && c <= '9', "invalid unsigned integer");
        require(v <= maximum / 10 && (v != maximum / 10 || std::uint64_t(c - '0') <= maximum % 10), "integer exceeds bound");
        v = v * 10 + std::uint64_t(c - '0');
    }
    return v;
}
bool boolean(const std::string& s) { require(s == "true" || s == "false", "boolean must be true or false"); return s == "true"; }
std::optional<double> optional_number(const std::string& s) { return s == "none" ? std::nullopt : std::optional<double>(number(s)); }
std::vector<std::string> fields(const std::string& s, std::size_t count) {
    std::vector<std::string> result; std::istringstream in(s); std::string part;
    while (std::getline(in, part, ',')) result.push_back(trim(part));
    require(result.size() == count && !s.empty() && s.back() != ',', "wrong entry field count");
    return result;
}
std::string quoted(const std::string& s) {
    std::ostringstream out; out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else out << char(c);
    }
    return out.str() + '"';
}
void json_number(std::ostream& out, std::optional<double> v) { if (v && finite(*v)) out << *v; else out << "null"; }
std::filesystem::path resolved(std::filesystem::path p) {
    // A missing relative leaf can remain relative after weakly_canonical.
    // Compare every protected/output name in the same absolute namespace.
    p = std::filesystem::absolute(p);
    for (unsigned depth = 0; std::filesystem::is_symlink(p); ++depth) {
        require(depth < 40, "too many symbolic links");
        const auto target = std::filesystem::read_symlink(p); p = target.is_absolute() ? target : p.parent_path() / target;
    }
    return std::filesystem::weakly_canonical(p);
}
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.native().size()), b.c_str(), static_cast<int>(b.native().size()), TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
} // namespace

DetectionProfile parse_detection_profile(const std::string& input) {
    require(input.size() <= kMaxBytes, "profile exceeds 64 KiB"); utf8(input);
    std::string text = input;
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    std::map<std::string, std::string> values;
    std::istringstream lines(text); std::string line; std::size_t line_count = 0;
    while (std::getline(lines, line)) {
        require(++line_count <= 2048 && line.size() <= 8192, "profile line limit exceeded");
        line = trim(line); if (line.empty() || line[0] == '#') continue;
        text_field(line, 8192);
        const auto separator = line.find('='); require(separator != std::string::npos, "expected key=value");
        const auto key = trim(line.substr(0, separator)), value = trim(line.substr(separator + 1));
        require(!key.empty() && !value.empty() && key.size() <= 128, "empty or oversized profile key/value");
        require(values.emplace(key, value).second, "duplicate key: " + key);
    }
    auto get = [&](const std::string& key) {
        auto it = values.find(key); require(it != values.end(), "missing key: " + key);
        const auto value = it->second; values.erase(it); return value;
    };
    auto u32 = [&](const std::string& key) { return static_cast<std::uint32_t>(unsigned_integer(get(key), std::numeric_limits<std::uint32_t>::max())); };
    DetectionProfile p;
    p.schema = get("schema"); p.id = get("id"); p.status = get("status"); p.setup_id = get("setup_id");
    p.source_path = get("source_path"); p.source_metadata_status = get("source_metadata_status");
    p.source_header_crc32 = u32("source_header_crc32"); p.source_payload_crc32 = u32("source_payload_crc32");
    p.source_kind = get("source_kind"); p.source_synthetic = boolean(get("source_synthetic"));
    p.calibration_confirmed = boolean(get("calibration_confirmed"));
    p.sensor_sensitivity_v_per_g = number(get("sensor_sensitivity_v_per_g")); p.analog_gain = number(get("analog_gain"));
    if (values.count("amplitude_unit")) {
        const auto unit = get("amplitude_unit");
        require(unit == "V" || unit == "m/s^2" || unit == "g", "invalid amplitude unit");
        p.amplitude_unit = unit == "V" ? AmplitudeUnit::Voltage : unit == "g" ? AmplitudeUnit::Gravity : AmplitudeUnit::AccelerationSI;
    }
    if (values.count("channel_calibration")) {
        const auto mode = get("channel_calibration");
        require(mode == "legacy_global" || mode == "per_channel", "invalid channel calibration mode");
        if (mode == "per_channel") {
            std::array<ChannelCalibration, 3> channels;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto prefix = "channel." + std::to_string(channel + 1) + ".";
                channels[channel] = {number(get(prefix + "sensitivity_mv_per_m_s2")),
                    number(get(prefix + "analog_gain")), number(get(prefix + "voltage_bias_v"))};
            }
            p.channel_calibration = channels;
        }
    }
    p.sample_rate_hz = u32("sample_rate_hz"); p.channel_map = u32("channel_map");
    p.adc_range_uv = static_cast<std::int32_t>(unsigned_integer(get("adc_range_uv"), std::numeric_limits<std::int32_t>::max()));
    p.fft_size = static_cast<std::size_t>(unsigned_integer(get("fft_size"), 262144));
    p.actual_fft_size = static_cast<std::size_t>(unsigned_integer(get("actual_fft_size"), 262144));
    p.spectrum_max_hz = number(get("spectrum_max_hz")); p.pulses_per_rev = optional_number(get("pulses_per_rev"));
    p.speed_rpm_min = optional_number(get("speed_rpm_min")); p.speed_rpm_max = optional_number(get("speed_rpm_max"));
    p.min_records = u32("min_records"); p.min_speed_valid_fraction = number(get("min_speed_valid_fraction"));
    p.max_speed_timeout_fraction = number(get("max_speed_timeout_fraction")); p.max_speed_cv = optional_number(get("max_speed_cv"));
    const auto count = unsigned_integer(get("baseline_count"), kMaxFeatures);
    for (std::size_t i = 0; i < count; ++i) {
        const auto f = fields(get("baseline." + std::to_string(i)), 3);
        p.baseline.push_back({f[1], static_cast<unsigned>(unsigned_integer(f[0], 3)), number(f[2])});
    }
    const auto rule_count = unsigned_integer(get("rule_count"), kMaxRules);
    for (std::size_t i = 0; i < rule_count; ++i) {
        const auto prefix = "rule." + std::to_string(i) + ".";
        DetectionRule rule; rule.id = get(prefix + "id"); rule.label = get(prefix + "label");
        const auto conditions = unsigned_integer(get(prefix + "condition_count"), kMaxConditions);
        for (std::size_t j = 0; j < conditions; ++j) {
            const auto f = fields(get(prefix + "condition." + std::to_string(j)), 5);
            rule.conditions.push_back({f[1], static_cast<unsigned>(unsigned_integer(f[0], 3)), f[2], number(f[3]), number(f[4])});
        }
        p.rules.push_back(std::move(rule));
    }
    require(values.empty(), "unknown profile key: " + (values.empty() ? std::string() : values.begin()->first));
    validate(p); return p;
}

DetectionProfile load_detection_profile(const std::string& path) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    require(bool(in), "cannot open profile: " + path);
    const auto size = in.tellg(); require(size >= 0 && size <= static_cast<std::streamoff>(kMaxBytes), "profile exceeds 64 KiB or length unavailable");
    in.seekg(0); std::string text(static_cast<std::size_t>(size), '\0');
    require(bool(in.read(text.data(), static_cast<std::streamsize>(text.size()))) && in.peek() == std::char_traits<char>::eof(), "cannot read complete profile");
    return parse_detection_profile(text);
}

std::string detection_profile_text(const DetectionProfile& p) {
    validate(p);
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::boolalpha;
    out << "# Single-record baseline snapshot and experimental all-of rules.\n"
        << "# No normal-state confirmation, trained model, validated thresholds or accuracy claim.\n"
        << "schema=" << p.schema << "\nid=" << p.id << "\nstatus=" << p.status << "\nsetup_id=" << p.setup_id
        << "\nsource_path=" << p.source_path << "\nsource_metadata_status=" << p.source_metadata_status
        << "\nsource_header_crc32=" << p.source_header_crc32 << "\nsource_payload_crc32=" << p.source_payload_crc32
        << "\nsource_kind=" << p.source_kind << "\nsource_synthetic=" << p.source_synthetic
        << "\ncalibration_confirmed=" << p.calibration_confirmed << "\nsensor_sensitivity_v_per_g=" << p.sensor_sensitivity_v_per_g
        << "\nanalog_gain=" << p.analog_gain << "\nsample_rate_hz=" << p.sample_rate_hz << "\nchannel_map=" << p.channel_map
        << "\nadc_range_uv=" << p.adc_range_uv << "\nfft_size=" << p.fft_size << "\nactual_fft_size=" << p.actual_fft_size
        << "\nspectrum_max_hz=" << p.spectrum_max_hz
        << "\namplitude_unit=" << amplitude_unit_symbol(calibration_config(p))
        << "\nchannel_calibration=" << (p.channel_calibration ? "per_channel" : "legacy_global");
    if (p.channel_calibration) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto prefix = "\nchannel." + std::to_string(channel + 1) + ".";
            const auto& c = (*p.channel_calibration)[channel];
            out << prefix << "sensitivity_mv_per_m_s2=" << c.sensitivity_mv_per_m_s2
                << prefix << "analog_gain=" << c.analog_gain << prefix << "voltage_bias_v=" << c.voltage_bias_v;
        }
    }
    auto optional = [&](const char* key, const std::optional<double>& value) {
        out << '\n' << key << '='; if (value) out << *value; else out << "none";
    };
    optional("pulses_per_rev", p.pulses_per_rev); optional("speed_rpm_min", p.speed_rpm_min); optional("speed_rpm_max", p.speed_rpm_max);
    out << "\nmin_records=" << p.min_records << "\nmin_speed_valid_fraction=" << p.min_speed_valid_fraction
        << "\nmax_speed_timeout_fraction=" << p.max_speed_timeout_fraction;
    optional("max_speed_cv", p.max_speed_cv);
    out << "\nbaseline_count=" << p.baseline.size();
    for (std::size_t i = 0; i < p.baseline.size(); ++i) {
        const auto& b = p.baseline[i]; out << "\nbaseline." << i << '=' << b.channel << ',' << b.feature << ',' << b.value;
    }
    out << "\nrule_count=" << p.rules.size();
    for (std::size_t i = 0; i < p.rules.size(); ++i) {
        const auto& rule = p.rules[i]; const auto prefix = "\nrule." + std::to_string(i) + ".";
        out << prefix << "id=" << rule.id << prefix << "label=" << rule.label << prefix << "condition_count=" << rule.conditions.size();
        for (std::size_t j = 0; j < rule.conditions.size(); ++j) {
            const auto& c = rule.conditions[j]; out << prefix << "condition." << j << '=' << c.channel << ',' << c.feature << ',' << c.comparison << ',' << c.minimum << ',' << c.maximum;
        }
    }
    out << '\n'; auto text = out.str(); require(text.size() <= kMaxBytes, "serialized profile exceeds 64 KiB"); utf8(text); return text;
}

DetectionProfile make_baseline_draft(const AnalysisResult& a, const std::string& setup_id, const std::string& profile_id) {
    require(a.source_metadata.status != "invalid_or_unreadable_sidecar", "resolve invalid source metadata before baseline export");
    require(!a.source_path.empty(), "baseline source path is empty");
    DetectionProfile p; p.id = profile_id; p.setup_id = setup_id;
    // Preserve the spelling used for same-stem metadata while making its base
    // unambiguous when a profile is later opened from another working directory.
    p.source_path = std::filesystem::absolute(std::filesystem::u8path(a.source_path)).u8string();
    p.source_metadata_status = a.source_metadata.status; p.source_header_crc32 = a.header.header_crc32;
    p.source_payload_crc32 = a.header.payload_crc32; p.source_kind = signal_name(a.config.signal_kind);
    p.source_synthetic = a.source_metadata.synthetic; p.calibration_confirmed = a.config.calibration_confirmed;
    p.sensor_sensitivity_v_per_g = a.config.sensor_sensitivity_v_per_g; p.analog_gain = a.config.analog_gain;
    p.channel_calibration = a.config.channel_calibration; p.amplitude_unit = a.config.amplitude_unit;
    p.sample_rate_hz = a.header.sample_rate_hz; p.channel_map = a.header.channel_map; p.adc_range_uv = a.header.adc_range_uv;
    p.fft_size = a.config.fft_size; p.actual_fft_size = a.spectra[0].fft_size; p.spectrum_max_hz = a.config.spectrum_max_hz;
    p.pulses_per_rev = a.speed.pulses_per_rev; p.speed_rpm_min = a.speed.min_rpm; p.speed_rpm_max = a.speed.max_rpm;
    p.min_records = a.header.completed_records; p.min_speed_valid_fraction = a.speed.valid_fraction;
    p.max_speed_timeout_fraction = a.speed.timeout_fraction;
    if (a.speed.held_mean_rpm && *a.speed.held_mean_rpm > 0 && a.speed.held_std_rpm)
        p.max_speed_cv = *a.speed.held_std_rpm / *a.speed.held_mean_rpm;
    for (unsigned channel = 1; channel <= 3; ++channel)
        for (const auto& name : {"mean", "rms", "ac_rms", "abs_peak", "peak_to_peak", "crest_factor", "pearson_kurtosis", "order1_rms", "order2_rms", "order3_rms"})
            if (const auto v = feature(a, channel, name)) p.baseline.push_back({name, channel, *v});
    validate(p); return p;
}

DetectionResult evaluate_detection(const AnalysisResult& a, const DetectionProfile& p, const std::string& setup_id) {
    validate(p);
    DetectionResult r; r.profile_id = p.id; r.profile_status = p.status; r.setup_id = setup_id; r.source_path = a.source_path;
    r.amplitude_unit = amplitude_unit_symbol(a.config);
    r.baseline_amplitude_unit = amplitude_unit_symbol(calibration_config(p));
    const bool conversion_matches = calibration_config_equal(a.config, calibration_config(p));
    r.profile_snapshot = detection_profile_text(p);
    r.limitations = {"All profile rules and candidates are unvalidated; final classification remains not_classified.",
        "A single-record baseline is an observed snapshot, not a confirmed normal state or trained model.",
        "CH1/CH2/CH3 are storage channels (ADC V1/V3/V5); verified physical directions and operating setup are required.",
        "Source and calibration fields are user declarations, not independent proof of a physical measurement or calibration.",
        "No probability, accuracy, or machine fault diagnosis is established by this framework."};
    auto gate = [&](const std::string& id, bool passed, const std::string& detail) { r.gates.push_back({id, passed, detail}); };
    gate("setup", identifier(setup_id) && setup_id == p.setup_id, "Explicit setup_id must exactly equal the profile setup_id.");
    auto source_status_ok = [](const std::string& status) { return status == "not_provided" || status == "unconfirmed_sidecar"; };
    gate("source", a.config.signal_kind == SignalKind::VibrationConfirmed && p.source_kind == "vibration" &&
         !a.source_metadata.synthetic && !p.source_synthetic && source_status_ok(a.source_metadata.status) &&
         source_status_ok(p.source_metadata_status), "Current and baseline must declare physical vibration; synthetic or invalid metadata is blocked.");
    gate("calibration", a.config.calibration_confirmed && p.calibration_confirmed &&
         a.config.amplitude_unit != AmplitudeUnit::Voltage && a.amplitude_unit == amplitude_unit_symbol(a.config),
         "Current and baseline calibration declarations must be confirmed and features must use an acceleration unit.");
    gate("analysis_configuration", conversion_matches &&
         a.config.fft_size == p.fft_size && a.config.spectrum_max_hz == p.spectrum_max_hz &&
         a.header.sample_rate_hz == p.sample_rate_hz && a.header.adc_range_uv == p.adc_range_uv && a.header.channel_map == p.channel_map &&
         std::all_of(a.spectra.begin(), a.spectra.end(), [&](const Spectrum& s) { return s.fft_size == p.actual_fft_size && s.fft_size >= 16; }),
         "Amplitude unit, per-channel sensitivity, signed gain, explicit voltage bias, sample rate, ADC range, channel map and requested/actual FFT configuration must match exactly.");
    gate("ppr", a.speed.pulses_per_rev && p.pulses_per_rev && finite(*a.speed.pulses_per_rev) && *a.speed.pulses_per_rev == *p.pulses_per_rev,
         "Current and baseline must have the same known effective PPR.");
    gate("record_count", a.header.completed_records >= p.min_records, "Current record count must meet the profile minimum.");
    gate("adc_endpoints", std::all_of(a.time_stats.begin(), a.time_stats.end(), [](const TimeStats& s) { return s.adc_endpoint_count == 0; }), "No ADC endpoint code is permitted before rule evaluation.");
    bool measured = finite(a.duration_s) && a.duration_s > 0;
    for (unsigned channel = 1; channel <= 3; ++channel)
        for (const auto& name : {"mean", "rms", "ac_rms", "abs_peak", "peak_to_peak"}) measured = measured && bool(feature(a, channel, name));
    gate("finite_features", measured, "Core time-domain measurements must be finite and duration must be positive.");
    std::set<std::pair<unsigned, std::string>> required;
    for (const auto& b : p.baseline) required.emplace(b.channel, b.feature);
    for (const auto& rule : p.rules) for (const auto& c : rule.conditions) required.emplace(c.channel, c.feature);
    bool available = !required.empty();
    for (const auto& item : required) {
        available = available && bool(feature(a, item.first, item.second));
        if (item.second.compare(0, 5, "order") == 0) {
            const auto& spectrum = a.spectra[item.first - 1];
            const auto& band = a.order_bands[item.first - 1][static_cast<unsigned>(item.second[5] - '1')];
            available = available && spectrum.fft_size >= 16 && spectrum.segment_count > 0 &&
                finite(spectrum.bin_width_hz) && spectrum.bin_width_hz > 0 && !spectrum.frequency_hz.empty() &&
                spectrum.frequency_hz.size() == spectrum.psd.size() && spectrum.psd.size() == spectrum.peak_amplitude.size() &&
                std::all_of(spectrum.frequency_hz.begin(), spectrum.frequency_hz.end(), [](double v) { return finite(v) && v >= 0; }) &&
                std::all_of(spectrum.psd.begin(), spectrum.psd.end(), [](double v) { return finite(v) && v >= 0; }) &&
                std::all_of(spectrum.peak_amplitude.begin(), spectrum.peak_amplitude.end(), [](double v) { return finite(v) && v >= 0; }) &&
                band.center_hz && band.half_width_hz && finite(*band.center_hz) && finite(*band.half_width_hz) &&
                *band.center_hz > 0 && *band.half_width_hz > 0;
        }
    }
    gate("required_features", available, "Every feature referenced by any baseline entry or rule must be available and finite before any candidate is emitted.");
    const auto& s = a.speed;
    const bool known_speed = s.held_mean_rpm && s.held_std_rpm && s.min_rpm && s.max_rpm && p.speed_rpm_min && p.speed_rpm_max && p.max_speed_cv &&
        finite(*s.held_mean_rpm) && finite(*s.held_std_rpm) && finite(*s.min_rpm) && finite(*s.max_rpm) && *s.held_mean_rpm > 0 && *s.held_std_rpm >= 0 &&
        *s.min_rpm > 0 && *s.min_rpm <= *s.max_rpm && *s.held_mean_rpm >= *s.min_rpm && *s.held_mean_rpm <= *s.max_rpm;
    gate("speed", known_speed && finite(s.valid_fraction) && finite(s.timeout_fraction) && s.valid_fraction >= p.min_speed_valid_fraction &&
        s.valid_fraction >= 0.9 && s.valid_fraction <= 1 && s.timeout_fraction >= 0 && s.timeout_fraction <= p.max_speed_timeout_fraction &&
        s.timeout_fraction <= 1 && s.observable_new_edges > 0 &&
        *s.min_rpm >= *p.speed_rpm_min && *s.max_rpm <= *p.speed_rpm_max &&
        *s.held_std_rpm / *s.held_mean_rpm <= *p.max_speed_cv && *s.held_std_rpm / *s.held_mean_rpm <= 0.05,
        "Known RPM range and CV must fit the explicit profile; core speed quality (>=90% valid, <=5% CV) and observable edges are required.");
    auto comparison_value = [&](const DetectionCondition& c) -> std::optional<double> {
        if (!conversion_matches) return std::nullopt;
        const auto current = feature(a, c.channel, c.feature);
        if (!current) return std::nullopt;
        if (c.comparison == "absolute") return current;
        const auto reference = baseline(p, c.channel, c.feature);
        if (!reference || (c.comparison == "ratio" && *reference == 0)) return std::nullopt;
        const auto value = c.comparison == "delta" ? *current - *reference : *current / *reference;
        return finite(value) ? std::optional<double>(value) : std::nullopt;
    };
    bool comparison_domain = true;
    for (const auto& rule : p.rules) for (const auto& c : rule.conditions)
        comparison_domain = bool(comparison_value(c)) && comparison_domain;
    gate("comparison_domain", comparison_domain, "Every rule condition must be computable; missing features, zero ratio denominators and out-of-range arithmetic block all candidates.");
    const bool passed = std::all_of(r.gates.begin(), r.gates.end(), [](const DetectionGate& g) { return g.passed; });
    for (const auto& b : p.baseline) {
        DetectionComparison comparison; comparison.feature = b.feature; comparison.channel = b.channel; comparison.baseline = b.value;
        comparison.current = feature(a, b.channel, b.feature);
        comparison.status = !conversion_matches ? "calibration_configuration_mismatch" : comparison.current ? "measured" : "feature_unavailable";
        if (comparison.current && conversion_matches) {
            const auto delta = *comparison.current - b.value;
            if (finite(delta)) comparison.delta = delta;
            if (b.value != 0) { const auto ratio = *comparison.current / b.value; if (finite(ratio)) comparison.ratio = ratio; }
            if (!comparison.ratio) comparison.status = b.value == 0 ? "zero_baseline_ratio_unavailable" : "ratio_out_of_range";
        }
        r.baseline_comparisons.push_back(std::move(comparison));
    }
    std::size_t matches = 0;
    for (const auto& rule : p.rules) {
        DetectionRuleEvidence evidence; evidence.id = rule.id; evidence.label = rule.label; evidence.matched = passed;
        for (const auto& c : rule.conditions) {
            DetectionConditionEvidence e; e.condition = c; e.current = feature(a, c.channel, c.feature); e.baseline = baseline(p, c.channel, c.feature);
            e.evaluated = comparison_value(c);
            if (!conversion_matches) e.status = "calibration_configuration_mismatch";
            else if (!e.current) e.status = "feature_unavailable";
            else if (!e.evaluated) e.status = "comparison_unavailable";
            else if (!passed) e.status = "not_evaluated_quality_gate";
            else {
                e.passed = e.evaluated && *e.evaluated >= c.minimum && *e.evaluated <= c.maximum;
                e.status = !e.evaluated ? "comparison_unavailable" : e.passed ? "within_interval" : "outside_interval";
            }
            evidence.matched = evidence.matched && e.passed; evidence.conditions.push_back(std::move(e));
        }
        if (evidence.matched) { ++matches; r.candidate = rule.label; }
        r.rules.push_back(std::move(evidence));
    }
    if (!passed) r.status = "quality_gated";
    else if (p.rules.empty()) r.status = "baseline_only";
    else if (matches == 0) r.status = "inconclusive";
    else if (matches == 1) r.status = "candidate";
    else r.status = "conflict";
    if (matches != 1) r.candidate.reset();
    return r;
}

std::string detection_to_json(const DetectionResult& r) {
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::boolalpha;
    out << "{\"schema\":" << quoted(r.schema) << ",\"profile_id\":" << quoted(r.profile_id) << ",\"profile_status\":" << quoted(r.profile_status)
        << ",\"profile_snapshot\":" << quoted(r.profile_snapshot)
        << ",\"amplitude_unit\":" << quoted(r.amplitude_unit) << ",\"baseline_amplitude_unit\":" << quoted(r.baseline_amplitude_unit)
        << ",\"setup_id\":" << quoted(r.setup_id) << ",\"source_path\":" << quoted(r.source_path) << ",\"status\":" << quoted(r.status)
        << ",\"classification\":\"not_classified\",\"validated\":false,\"candidate\":";
    if (r.candidate) out << quoted(*r.candidate); else out << "null";
    out << ",\"gates\":[";
    for (std::size_t i = 0; i < r.gates.size(); ++i) {
        const auto& g = r.gates[i]; if (i) out << ',';
        out << "{\"id\":" << quoted(g.id) << ",\"passed\":" << g.passed << ",\"detail\":" << quoted(g.detail) << '}';
    }
    out << "],\"baseline_comparisons\":[";
    for (std::size_t i = 0; i < r.baseline_comparisons.size(); ++i) {
        const auto& c = r.baseline_comparisons[i]; if (i) out << ',';
        out << "{\"channel\":" << c.channel << ",\"feature\":" << quoted(c.feature) << ",\"baseline\":"; json_number(out, c.baseline);
        out << ",\"current\":"; json_number(out, c.current); out << ",\"delta\":"; json_number(out, c.delta);
        out << ",\"ratio\":"; json_number(out, c.ratio); out << ",\"status\":" << quoted(c.status) << '}';
    }
    out << "],\"rules\":[";
    for (std::size_t i = 0; i < r.rules.size(); ++i) {
        const auto& rule = r.rules[i]; if (i) out << ',';
        out << "{\"id\":" << quoted(rule.id) << ",\"label\":" << quoted(rule.label) << ",\"matched\":" << rule.matched << ",\"conditions\":[";
        for (std::size_t j = 0; j < rule.conditions.size(); ++j) {
            const auto& e = rule.conditions[j]; if (j) out << ',';
            out << "{\"channel\":" << e.condition.channel << ",\"feature\":" << quoted(e.condition.feature) << ",\"comparison\":" << quoted(e.condition.comparison)
                << ",\"minimum\":"; json_number(out, e.condition.minimum); out << ",\"maximum\":"; json_number(out, e.condition.maximum);
            out << ",\"current\":"; json_number(out, e.current); out << ",\"baseline\":"; json_number(out, e.baseline);
            out << ",\"evaluated\":"; json_number(out, e.evaluated); out << ",\"passed\":" << e.passed << ",\"status\":" << quoted(e.status) << '}';
        }
        out << "]}";
    }
    out << "],\"limitations\":[";
    for (std::size_t i = 0; i < r.limitations.size(); ++i) { if (i) out << ','; out << quoted(r.limitations[i]); }
    out << "]}"; return out.str();
}

void validate_detection_outputs(const std::vector<std::string>& paths, const std::vector<std::string>& protected_paths) {
    auto protected_set = protected_paths;
    for (const auto& path : paths) {
        require(!path.empty(), "output path is empty"); const auto output = resolved(std::filesystem::u8path(path));
        for (const auto& original : protected_set) {
            require(!original.empty(), "empty protected path"); const auto source = std::filesystem::u8path(original);
            require(!same_path(output, resolved(source)) && !(std::filesystem::exists(output) && std::filesystem::exists(source) && std::filesystem::equivalent(output, source)),
                    "output must not overwrite capture, metadata, profile or another output: " + original);
        }
        protected_set.push_back(path);
    }
}

void write_detection_text(const std::string& path, const std::string& text, const std::vector<std::string>& protected_paths) {
    validate_detection_outputs({path}, protected_paths); const auto output = resolved(std::filesystem::u8path(path));
    std::ofstream out(output, std::ios::binary | std::ios::trunc); require(bool(out), "cannot create output: " + path);
    out << text; out.close(); require(bool(out), "cannot finish output: " + path);
}
} // namespace vibration
