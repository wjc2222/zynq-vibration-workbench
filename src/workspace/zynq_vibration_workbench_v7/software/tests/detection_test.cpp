#include "vibration/detection.hpp"
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
void rejects(const std::function<void()>& operation, const std::string& name) {
    bool rejected = false;
    try { operation(); } catch (const vibration::AnalysisError&) { rejected = true; }
    check(rejected, name + " was accepted");
}
vibration::AnalysisResult fixture() {
    // An in-memory software fixture deliberately declares measurement conditions
    // to exercise decision paths; it supplies no physical accuracy evidence.
    vibration::Capture capture;
    capture.integrity_verified = true;
    capture.source_path = "software-QA-fixture-not-a-physical-measurement.BIN";
    capture.header.sample_rate_hz = 50000;
    capture.header.clock_hz = 50000000;
    capture.header.adc_range_uv = 5000000;
    capture.header.channel_map = 0x00050301;
    capture.header.pulses_per_rev = 1;
    capture.header.completed_records = 65536;
    capture.records.resize(65536);
    for (std::size_t i=0; i<capture.records.size(); ++i) {
        auto& row = capture.records[i];
        row.index=i; row.tick=i*1000; row.flags=3;
        const auto value=static_cast<std::int16_t>(800*std::sin(i*0.1));
        row.raw={value,static_cast<std::int16_t>(value/2),static_cast<std::int16_t>(value/3)};
        row.speed_period_ticks=1500000; row.speed_edge_count=static_cast<std::uint32_t>(i/1500);
    }
    vibration::AnalysisConfig config;
    config.signal_kind=vibration::SignalKind::VibrationConfirmed;
    config.calibration_confirmed=true; config.fft_size=65536;
    return vibration::analyze(capture,config);
}
vibration::DetectionRule rule(const std::string& label, double lo=0, double hi=1e6) {
    return {"example_"+label,label,{{"ac_rms",1,"absolute",lo,hi}}};
}
void gated(vibration::AnalysisResult analysis, const vibration::DetectionProfile& profile,
           const std::string& reason, const std::string& setup="software-qa-bench") {
    const auto result=vibration::evaluate_detection(analysis,profile,setup);
    check(result.status=="quality_gated"&&!result.candidate&&result.classification=="not_classified",
          "Gate failed: "+reason);
    check(!result.validated,"Gate promoted a fixture to validated");
}
}
int main() {
    try {
        const auto analysis=fixture();
        auto profile=vibration::make_baseline_draft(analysis,"software-qa-bench","example_experimental");
        check(profile.rules.empty()&&profile.status=="draft","Draft invented normal rules/status");
        auto empty=vibration::evaluate_detection(analysis,profile,"software-qa-bench");
        check(empty.status=="baseline_only"&&!empty.candidate,"No rules became normal");
        for (const std::string label:{"normal","imbalance","misalignment","looseness"}) {
            profile.rules={rule(label)};
            const auto result=vibration::evaluate_detection(analysis,profile,"software-qa-bench");
            check(result.status=="candidate"&&result.candidate==label,"Single rule did not yield "+label);
            check(!result.validated&&result.classification=="not_classified","Candidate became confirmed fault");
            const auto json=vibration::detection_to_json(result);
            check(json.find("\"probability\":")==std::string::npos&&json.find("\"confidence\":")==std::string::npos,
                  "Rule output invented probability/confidence");
            const auto text=vibration::detection_profile_text(profile);
            check(result.profile_snapshot==text,"Detection result lost exact canonical profile snapshot");
            const auto parsed=vibration::parse_detection_profile(text);
            check(vibration::detection_profile_text(parsed)==text,"Profile round trip changed fields");
            check(vibration::detection_to_json(vibration::evaluate_detection(analysis,parsed,"software-qa-bench"))==json,
                  "Canonical profile snapshot did not replay identical detection result");
        }
        profile.rules={rule("normal"),rule("imbalance")};
        auto result=vibration::evaluate_detection(analysis,profile,"software-qa-bench");
        check(result.status=="conflict"&&!result.candidate,"Conflicting labels had a winner");
        profile.rules={rule("normal"),rule("normal")};profile.rules[1].id="example_second_normal";
        result=vibration::evaluate_detection(analysis,profile,"software-qa-bench");
        check(result.status=="conflict"&&!result.candidate,"Two matching rules were silently collapsed");
        profile.rules={rule("normal")};
        profile.rules[0].conditions.push_back({"ac_rms",2,"absolute",100,200});
        result=vibration::evaluate_detection(analysis,profile,"software-qa-bench");
        check(result.status=="inconclusive"&&!result.candidate,"All-of rule ignored failed second condition");
        profile.rules={rule("normal",100,200)};
        result=vibration::evaluate_detection(analysis,profile,"software-qa-bench");
        check(result.status=="inconclusive"&&!result.candidate,"No matching rule became normal");
        profile.rules={rule("imbalance")};
        auto modified=analysis; modified.source_metadata.synthetic=true; gated(modified,profile,"synthetic");
        modified=analysis; modified.source_metadata.status="invalid_or_unreadable_sidecar"; gated(modified,profile,"invalid provenance");
        modified=analysis; modified.config.signal_kind=vibration::SignalKind::Unknown; gated(modified,profile,"unknown source");
        modified=analysis; modified.config.signal_kind=vibration::SignalKind::TestSignal; gated(modified,profile,"test source");
        modified=analysis; modified.config.calibration_confirmed=false; gated(modified,profile,"uncalibrated");
        modified=analysis; modified.time_stats[0].adc_endpoint_count=1; gated(modified,profile,"ADC clipping");
        modified=analysis; modified.speed.held_mean_rpm.reset(); gated(modified,profile,"missing speed");
        modified=analysis; modified.speed.pulses_per_rev.reset(); gated(modified,profile,"missing PPR");
        modified=analysis; modified.speed.valid_fraction=0; modified.speed.timeout_fraction=1; gated(modified,profile,"speed timeout");
        modified=analysis; modified.speed.held_mean_rpm=3000; gated(modified,profile,"incompatible RPM");
        modified=analysis; modified.config.analog_gain=-2; gated(modified,profile,"gain mismatch");
        modified=analysis; modified.config.sensor_sensitivity_v_per_g=.7; gated(modified,profile,"sensitivity mismatch");
        modified=analysis; modified.config.fft_size=2048; gated(modified,profile,"FFT config mismatch");
        modified=analysis; modified.config.spectrum_max_hz=300; gated(modified,profile,"spectrum config mismatch");
        modified=analysis; modified.header.channel_map=0x00030201; gated(modified,profile,"channel map mismatch");
        modified=analysis; modified.header.completed_records=1; gated(modified,profile,"insufficient records");
        modified=analysis; modified.duration_s=0; gated(modified,profile,"zero duration");
        modified=analysis; modified.time_stats[0].ac_rms=std::numeric_limits<double>::quiet_NaN(); gated(modified,profile,"nonfinite measurement");
        modified=analysis; modified.time_stats[0].ac_rms=-1; gated(modified,profile,"negative RMS measurement");
        modified=analysis; modified.spectra[0].psd.clear(); gated(modified,profile,"missing spectrum");
        modified=analysis; modified.order_bands[0][0].rms.reset(); gated(modified,profile,"unavailable order band");
        auto timeOnly=profile;
        timeOnly.baseline={{"ac_rms",1,analysis.time_stats[0].ac_rms}};
        modified=analysis; modified.order_bands[0][0].rms.reset();
        result=vibration::evaluate_detection(modified,timeOnly,"software-qa-bench");
        check(result.status=="candidate","Unreferenced missing order incorrectly blocked pure time rule");
        gated(analysis,profile,"setup mismatch","other-setup");
        auto zero=profile;
        for (auto& feature:zero.baseline) if(feature.feature=="ac_rms"&&feature.channel==1) feature.value=0;
        zero.rules[0].conditions[0].comparison="ratio";
        result=vibration::evaluate_detection(analysis,zero,"software-qa-bench");
        check(result.status=="quality_gated"&&!result.candidate,"Zero baseline ratio was not globally gated");
        check(!result.rules[0].conditions[0].evaluated,"Zero baseline generated infinite ratio");
        zero.rules.push_back(rule("normal"));
        result=vibration::evaluate_detection(analysis,zero,"software-qa-bench");
        check(result.status=="quality_gated"&&!result.candidate,"Unavailable ratio let another normal rule win");
        zero.rules.pop_back();
        zero.rules[0].conditions[0].comparison="delta";
        result=vibration::evaluate_detection(analysis,zero,"software-qa-bench");
        check(result.status=="candidate","Finite zero-baseline delta unavailable");
        const auto valid=vibration::detection_profile_text(profile);
        rejects([&]{vibration::parse_detection_profile(valid+"unknown_key=true\n");},"unknown key");
        rejects([&]{vibration::parse_detection_profile(valid+"schema=zynq_vibration.detection_profile.v1\n");},"duplicate key");
        rejects([&]{vibration::parse_detection_profile(std::string(65537,'x'));},"oversized profile");
        rejects([&]{vibration::parse_detection_profile(valid+std::string("\xc0\xaf",2));},"invalid UTF8");
        auto bad=profile; bad.rules[0].conditions[0].minimum=std::numeric_limits<double>::infinity();
        rejects([&]{vibration::detection_profile_text(bad);},"nonfinite condition");
        bad=profile; bad.rules[0].conditions[0].minimum=2; bad.rules[0].conditions[0].maximum=1;
        rejects([&]{vibration::detection_profile_text(bad);},"reversed interval");
        bad=profile; bad.rules[0].conditions[0].channel=4;
        rejects([&]{vibration::evaluate_detection(analysis,bad,"software-qa-bench");},"invalid channel");
        bad=profile; bad.rules[0].label="industrial_certified_fault";
        rejects([&]{vibration::detection_profile_text(bad);},"unknown label");
        bad=profile; bad.status="validated";
        rejects([&]{vibration::detection_profile_text(bad);},"fake validated profile");
        bad=profile;bad.baseline.push_back(bad.baseline.front());
        rejects([&]{vibration::detection_profile_text(bad);},"duplicate baseline feature");
        bad=profile;bad.rules.push_back(bad.rules.front());
        rejects([&]{vibration::detection_profile_text(bad);},"duplicate rule id");
        bad=profile;bad.rules[0].conditions.push_back(bad.rules[0].conditions.front());
        rejects([&]{vibration::detection_profile_text(bad);},"duplicate rule condition");
        bad=profile;
        for(auto& feature:bad.baseline)if(feature.feature=="ac_rms"&&feature.channel==1)feature.value=-1;
        rejects([&]{vibration::detection_profile_text(bad);},"negative RMS baseline");
        std::cout<<"PASS: "<<checks<<" independent experimental detection software checks; no accuracy claim\n";
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
