#include "vibration/core.hpp"
#include "vibration/detection.hpp"

#include <cmath>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
std::vector<std::string> arguments(int argc, char** argv) {
    std::vector<std::string> result;
#ifdef _WIN32
    // The CRT's narrow argv uses the current Windows code page. Read the
    // original UTF-16 command line so Chinese paths reach the UTF-8 API intact.
    (void)argc; (void)argv;
    int count = 0;
    auto* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) throw vibration::AnalysisError("Cannot read Unicode command line");
    try {
        for (int i = 0; i < count; ++i) {
            const auto length = static_cast<int>(std::wcslen(wide[i]));
            const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], length, nullptr, 0, nullptr, nullptr);
            if (bytes == 0 && length != 0) throw vibration::AnalysisError("Invalid Unicode command-line argument");
            std::string utf8(static_cast<std::size_t>(bytes), '\0');
            if (bytes && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], length, utf8.data(), bytes, nullptr, nullptr) != bytes)
                throw vibration::AnalysisError("Cannot convert Unicode command-line argument");
            result.push_back(std::move(utf8));
        }
    } catch (...) { LocalFree(wide); throw; }
    LocalFree(wide);
#else
    for (int i = 0; i < argc; ++i) result.emplace_back(argv[i]);
#endif
    return result;
}

void usage() {
    std::cout <<
        "Usage: vibration_analyze CAPxxxxx.BIN [options]\n"
        "  --ppr NUMBER           Explicit PPR when the file declares zero/unknown\n"
        "  --sensitivity NUMBER   Sensor sensitivity in V/g (default 0.5)\n"
        "  --gain NUMBER          Signed analog voltage gain (default -1)\n"
        "  --signal unknown|test|vibration  Source declaration; synthetic sidecar locks test\n"
        "  --calibrated           Declare that sensitivity and gain were calibrated\n"
        "  --fft-size NUMBER      Power of two 16..262144 (default 65536)\n"
        "  --max-hz NUMBER        Spectrum display limit (default 2000)\n"
        "  --waveform-points N    Display point budget 12..10000 (default 1200)\n"
        "  --max-file-mib NUMBER  Lower the 64 MiB protocol memory limit\n"
        "  --output PATH          Write JSON to PATH instead of standard output\n"
        "  --setup-id ID          Explicit acquisition setup identity for detection/baseline\n"
        "  --detection-profile PATH  Load a draft/experimental all-of rule profile\n"
        "  --detection-output PATH   Write separate unvalidated detection JSON (required with profile)\n"
        "  --write-baseline PATH  Write a single-record baseline draft with no rules\n"
        "  --baseline-id ID       Baseline profile identifier (default single_record_draft)\n"
        "The input capture and its source metadata are never modified. Classification remains unvalidated.\n";
}
double numeric(const std::string& text) {
    std::size_t consumed = 0;
    const auto n = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(n)) throw vibration::AnalysisError("Invalid numeric option: " + text);
    return n;
}
std::size_t integer(const std::string& text) {
    const auto n = numeric(text);
    if (n < 0 || std::floor(n) != n || n > 1000000000) throw vibration::AnalysisError("Invalid integer option: " + text);
    return static_cast<std::size_t>(n);
}

bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
#ifdef _WIN32
    // weakly_canonical preserves the spelling of a nonexistent filename.
    // Windows still treats a differently-cased output as the same filename.
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.native().size()),
                                b.c_str(), static_cast<int>(b.native().size()), TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}

std::filesystem::path resolved_path(std::filesystem::path path) {
    // weakly_canonical may leave a missing relative leaf relative. Anchor it
    // before resolving aliases so reserved missing sidecars are protected.
    path = std::filesystem::absolute(path);
    // weakly_canonical alone leaves a dangling final symlink unresolved. Follow
    // it explicitly so an output alias cannot create a reserved missing sidecar.
    for (unsigned depth = 0; std::filesystem::is_symlink(path); ++depth) {
        if (depth >= 40) throw vibration::AnalysisError("Too many output/source symbolic links");
        const auto target = std::filesystem::read_symlink(path);
        path = target.is_absolute() ? target : path.parent_path() / target;
    }
    return std::filesystem::weakly_canonical(path);
}

bool overwrites(const std::filesystem::path& output, const std::filesystem::path& original) {
    if (same_path(output, resolved_path(original))) return true;
    // Canonical paths handle symlinks; equivalent additionally recognizes hard
    // links whose names and paths differ but whose underlying file is shared.
    return std::filesystem::exists(output) && std::filesystem::exists(original) &&
        std::filesystem::equivalent(output, original);
}
}

int main(int argc, char** argv) {
    try {
        const auto args = arguments(argc, argv);
        vibration::AnalysisConfig cfg;
        vibration::LoadOptions load;
        std::string input, output;
        std::string profile_path, detection_output, setup_id, baseline_output, baseline_id = "single_record_draft";
        std::set<std::string> detection_options;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (arg == "--help" || arg == "-h") { usage(); return 0; }
            if (arg == "--calibrated") { cfg.calibration_confirmed = true; continue; }
            if (arg.rfind("--", 0) == 0) {
                if (i + 1 >= args.size()) throw vibration::AnalysisError("Missing value for " + arg);
                const std::string& val = args[++i];
                if (arg == "--setup-id" || arg == "--detection-profile" || arg == "--detection-output" ||
                    arg == "--write-baseline" || arg == "--baseline-id") {
                    if (val.empty() || !detection_options.insert(arg).second) throw vibration::AnalysisError("Empty or duplicate detection option: " + arg);
                    if (arg == "--setup-id") setup_id = val;
                    else if (arg == "--detection-profile") profile_path = val;
                    else if (arg == "--detection-output") detection_output = val;
                    else if (arg == "--write-baseline") baseline_output = val;
                    else baseline_id = val;
                }
                else if (arg == "--ppr") cfg.ppr_override = numeric(val);
                else if (arg == "--sensitivity") cfg.sensor_sensitivity_v_per_g = numeric(val);
                else if (arg == "--gain") cfg.analog_gain = numeric(val);
                else if (arg == "--fft-size") cfg.fft_size = integer(val);
                else if (arg == "--max-hz") cfg.spectrum_max_hz = numeric(val);
                else if (arg == "--waveform-points") cfg.waveform_points = integer(val);
                else if (arg == "--max-file-mib") {
                    const auto mib = numeric(val);
                    if (mib <= 0 || mib > 64) throw vibration::AnalysisError("File limit must be in (0,64] MiB");
                    load.max_file_bytes = static_cast<std::uint64_t>(mib * 1024 * 1024);
                } else if (arg == "--output") output = val;
                else if (arg == "--signal") {
                    if (val == "unknown") cfg.signal_kind = vibration::SignalKind::Unknown;
                    else if (val == "test") cfg.signal_kind = vibration::SignalKind::TestSignal;
                    else if (val == "vibration") cfg.signal_kind = vibration::SignalKind::VibrationConfirmed;
                    else throw vibration::AnalysisError("Signal must be unknown, test or vibration");
                } else throw vibration::AnalysisError("Unknown option: " + arg);
            } else {
                if (!input.empty()) throw vibration::AnalysisError("Only one input capture is accepted");
                input = arg;
            }
        }
        if (input.empty()) { usage(); return 2; }
        if (!detection_options.empty()) {
            if (profile_path.empty() && baseline_output.empty()) throw vibration::AnalysisError("Detection options require --detection-profile or --write-baseline");
            if (setup_id.empty()) throw vibration::AnalysisError("Detection and baseline export require explicit --setup-id");
            if (profile_path.empty() != detection_output.empty()) throw vibration::AnalysisError("--detection-profile and --detection-output must be supplied together");
            if (baseline_output.empty() && detection_options.count("--baseline-id")) throw vibration::AnalysisError("--baseline-id requires --write-baseline");
        }
        std::optional<vibration::DetectionProfile> profile;
        if (!profile_path.empty()) profile = vibration::load_detection_profile(profile_path);
        const auto result = vibration::analyze_file(input, cfg, load);
        const auto json = vibration::to_json(result);
        if (!detection_options.empty()) {
            std::vector<std::string> protected_paths{input};
            auto sidecar = std::filesystem::u8path(input); sidecar.replace_extension(".meta.json");
            protected_paths.push_back(sidecar.u8string());
            if (profile) {
                protected_paths.push_back(profile_path);
                protected_paths.push_back(profile->source_path);
                auto baseline_sidecar = std::filesystem::u8path(profile->source_path); baseline_sidecar.replace_extension(".meta.json");
                protected_paths.push_back(baseline_sidecar.u8string());
            }
            std::vector<std::pair<std::string, std::string>> writes;
            if (profile) writes.emplace_back(detection_output, vibration::detection_to_json(vibration::evaluate_detection(result, *profile, setup_id)) + '\n');
            if (!baseline_output.empty()) writes.emplace_back(baseline_output,
                vibration::detection_profile_text(vibration::make_baseline_draft(result, setup_id, baseline_id)));
            if (!output.empty()) writes.emplace_back(output, json + '\n');
            std::vector<std::string> paths; for (const auto& write : writes) paths.push_back(write.first);
            vibration::validate_detection_outputs(paths, protected_paths);
            for (const auto& write : writes) {
                auto guard = protected_paths;
                for (const auto& other : writes) if (other.first != write.first) guard.push_back(other.first);
                vibration::write_detection_text(write.first, write.second, guard);
            }
            if (output.empty()) std::cout << json << '\n';
            return 0;
        }
        if (output.empty()) std::cout << json << '\n';
        else {
            const auto input_path = std::filesystem::u8path(input);
            auto metadata_path = input_path;
            // Match the core's sidecar rule on the input spelling. Resolving an
            // input symlink first would protect the wrong same-stem sidecar.
            metadata_path.replace_extension(".meta.json");
            const auto output_path = resolved_path(std::filesystem::u8path(output));
            if (overwrites(output_path, input_path) || overwrites(output_path, metadata_path))
                throw vibration::AnalysisError("Output must not overwrite the original capture or its source metadata");
            std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
            if (!out) throw vibration::AnalysisError("Cannot create JSON output");
            out << json << '\n';
            if (!out) throw vibration::AnalysisError("Cannot finish writing JSON output");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Analysis failed: " << e.what() << '\n';
        return 1;
    }
}
