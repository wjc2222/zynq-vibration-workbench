#include "live_preview_model.hpp"
#include "timing_stats.hpp"
#include "test_fonts.hpp"
#ifndef LIVE_MODEL_ONLY
#include "analysis_pane.hpp"
#include "controls.hpp"
#include "waveform_stack.hpp"
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QLabel>
#endif
#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace vibration;
using namespace vibration::workbench;
namespace {
void put(QByteArray& bytes, int offset, quint64 value, int count) {
    for (int byte = 0; byte < count; ++byte) bytes[offset + byte] = char((value >> (byte * 8)) & 255);
}
QByteArray raw(const Capture& capture, std::size_t first, std::size_t count) {
    QByteArray out(int(count * 32), 0);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& r = capture.records.at(first + i); const int offset = int(i * 32);
        put(out, offset, r.index, 8); put(out, offset + 8, r.tick, 8);
        for (int channel = 0; channel < 3; ++channel) put(out, offset + 16 + channel * 2, quint16(r.raw[channel]), 2);
        put(out, offset + 22, r.flags, 2); put(out, offset + 24, r.speed_period_ticks, 4); put(out, offset + 28, r.speed_edge_count, 4);
    }
    return out;
}
quint32 checksum(const QByteArray& bytes) { return crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size())); }
QJsonObject frame(const Capture& capture, std::size_t first, std::size_t count, quint64 sequence, int dropped = 0) {
    const auto bytes = raw(capture, first, count);
    return QJsonObject{{"schema", "vibration.capture.v2"}, {"type", "event"}, {"event", "preview"}, {"job_id", "model_probe"},
        {"generation", "1"}, {"frame_seq", double(sequence)}, {"first_record", double(first)}, {"record_count", double(count)},
        {"committed_records", double(first + count)}, {"requested_records", double(capture.records.size())},
        {"sample_rate_hz", double(capture.header.sample_rate_hz)}, {"clock_hz", 50000000}, {"ppr", 1}, {"adc_range_uv", 5000000},
        {"channel_map", 328449}, {"record_bytes", 32}, {"source", "test_fixture"}, {"hardware_confirmed", false},
        {"synthetic", true}, {"source_flags", 1}, {"validity_flags", 27}, {"raw_base64", QString::fromLatin1(bytes.toBase64())},
        {"crc32", double(checksum(bytes))}, {"dropped_preview_records", dropped}};
}
QJsonObject replaceBytes(QJsonObject object, const QByteArray& bytes) {
    object["raw_base64"] = QString::fromLatin1(bytes.toBase64()); object["crc32"] = double(checksum(bytes)); return object;
}
bool close(double a, double b) { return std::abs(a - b) <= 1e-12 * std::max({1.0, std::abs(a), std::abs(b)}); }
void waitSpectrum(LivePreviewModel& model, int timeout = 2000) {
    QElapsedTimer timer; timer.start();
    while (!model.spectrum().available && timer.elapsed() < timeout) { model.pollSpectrum(); QApplication::processEvents(); QThread::msleep(1); }
}
void events(int milliseconds) {
    QElapsedTimer timer; timer.start(); while (timer.elapsed() < milliseconds) { QApplication::processEvents(); QThread::msleep(1); }
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); qa::configureFonts(app); QJsonObject report{{"font_audit", qa::fontAudit(app.font())}}; bool passed = true;
    const auto check = [&](const char* name, bool value) { report[name] = value; passed = passed && value; };
    if (argc < 2 || argc > 3) { std::fprintf(stderr, "usage: live_preview_probe SYNTHETIC_CAPDDR2 [EVIDENCE_DIR]\n"); return 2; }
    const QString output = argc > 2 ? QString::fromUtf8(argv[2]) : QString(); if (!output.isEmpty() && !QDir().mkpath(output)) return 2;
    try {
        const auto capture = load_capture(argv[1]);
        if (!capture.source_metadata.synthetic || capture.records.size() < 20000) throw std::runtime_error("Use a synthetic fixture with at least 20000 records");
        const auto first = frame(capture, 0, 2000, 1);
        LivePreviewModel model; QString error;
        check("first_frame_consumed_and_source_explicit", model.begin(first, true, &error) && model.info().acceptedFrames == 1 && model.info().synthetic && !model.info().hardwareConfirmed);
        auto view = model.view();
        check("voltage_is_direct_adc_scale_not_assumed_g", view.voltage.size() == 3 &&
            close(view.voltage[0].points.first().y(), capture.records[0].raw[0] * (5.0 / 32768.0)));
        const auto period = capture.records[0].speed_period_ticks;
        check("explicit_ppr1_enables_measured_rpm", view.rpm && view.meanSpeed && period && close(*view.meanSpeed, 50000000.0 / period * 60));
        LivePreviewModel unconfirmed; unconfirmed.begin(first, false);
        check("unconfirmed_ppr_keeps_pulse_hz", !unconfirmed.view().rpm && unconfirmed.view().meanSpeed && close(*unconfirmed.view().meanSpeed, 50000000.0 / period));
        const auto beforeBad = model.info().acceptedRecords;
        check("duplicate_frame_rejected_without_double_append", !model.accept(first) && model.info().acceptedRecords == beforeBad);
        auto corrupt = frame(capture, 2000, 2000, 2); corrupt["crc32"] = 0;
        check("bad_crc_rejected_without_partial_append", !model.accept(corrupt) && model.info().acceptedRecords == beforeBad);
        auto noncanonical = frame(capture, 2000, 2000, 2); auto encoded = noncanonical.value("raw_base64").toString();
        const QString alphabet = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
        const int paddingIndex = encoded.size() - 3; const int sextet = alphabet.indexOf(encoded[paddingIndex]);
        encoded[paddingIndex] = alphabet[sextet ^ 1]; noncanonical["raw_base64"] = encoded;
        check("nonzero_base64_pad_bits_rejected_before_append", !model.accept(noncanonical) && model.info().acceptedRecords == beforeBad);
        auto bytes = raw(capture, 2000, 2000); put(bytes, 22, 0, 2);
        check("bad_flags_rejected_after_correct_crc", !model.accept(replaceBytes(frame(capture, 2000, 2000, 2), bytes)) && model.info().acceptedRecords == beforeBad);
        bytes = raw(capture, 2000, 2000); put(bytes, 8, capture.records[2000].tick + 1, 8);
        check("bad_timestamp_rejected_after_correct_crc", !model.accept(replaceBytes(frame(capture, 2000, 2000, 2), bytes)) && model.info().acceptedRecords == beforeBad);
        check("later_frame_marks_observed_gap_once", model.accept(frame(capture, 6000, 2000, 4, 4000)) && model.info().previewDroppedRecords == 4000);
        view = model.view(2); bool gap = false;
        for (const auto& point : view.voltage[0].points) gap = gap || !std::isfinite(point.y());
        check("missing_preview_interval_is_nan_not_a_joined_curve", gap);
        auto changed = frame(capture, 8000, 2000, 5); changed["generation"] = "2";
        check("generation_change_quarantines_without_restarting_job", !model.accept(changed) && model.isLive() && model.info().quarantined && !model.accept(frame(capture, 8000, 2000, 5)));
        LivePreviewModel metadata;
        metadata.begin(first); changed = frame(capture, 2000, 2000, 2); changed["source"] = "hardware_test"; changed["hardware_confirmed"] = true;
        check("valid_but_changed_source_quarantines", !metadata.accept(changed) && metadata.info().quarantined);
        check("fresh_job_can_be_explicitly_started", model.begin(first, true) && !model.info().quarantined);
        for (std::size_t index = 2000, seq = 2; index + 2000 <= capture.records.size(); index += 2000, ++seq)
            if (!model.accept(frame(capture, index, 2000, seq))) throw std::runtime_error("Valid bounded-ring stream rejected");
        check("raw_ring_bounded_to_two_seconds", model.info().retainedRecords <= std::size_t(capture.header.sample_rate_hz) * 2 && model.info().acceptedRecords > model.info().retainedRecords);
        view = model.view(.25);
        check("latest_quarter_second_view_is_bounded", view.endTime - view.startTime <= .250000001 && view.voltage[0].points.size() <= int(capture.header.sample_rate_hz / 4 + 2));
        waitSpectrum(model);
        check("worker_produces_voltage_spectrum_from_contiguous_snapshot", model.spectrum().available && model.spectrum().channels[0].fft_size <= 8192);
        QElapsedTimer latest; latest.start();
        while (model.spectrum().endTime < view.endTime - 1.0 / capture.header.sample_rate_hz && latest.elapsed() < 2000) {
            model.pollSpectrum(); QApplication::processEvents(); QThread::msleep(1);
        }
        check("worker_catches_up_to_latest_contiguous_window", close(model.spectrum().endTime, view.endTime));
        const auto submitted = model.info().fftSubmitted; QElapsedTimer unchanged; unchanged.start();
        while (unchanged.elapsed() < 250) { model.pollSpectrum(); QThread::msleep(1); }
        check("unchanged_data_does_not_recompute_fft", model.info().fftSubmitted == submitted);
        report["fft_submitted_after_fast_replay"] = double(model.info().fftSubmitted);
        report["model_profile"] = model.performanceProfile();
        TimingSamples knownTimings; for (int i = 1; i <= 1024; ++i) knownTimings.add(i);
        const auto known = knownTimings.json();
        check("timing_percentiles_are_bounded_and_totals_retain_all_samples", known.value("count").toDouble() == 1024 &&
            known.value("recent_samples").toDouble() == 256 && close(known.value("total_ms").toDouble(), .5248) &&
            close(known.value("recent_p50_ms").toDouble(), .000896) && close(known.value("recent_p95_ms").toDouble(), .001011));

        // Repackage exactly the same first 8192 fixture records as a valid
        // temporary CAPDDR2, so the numerical comparator uses normal loading.
        QTemporaryDir temporary; if (!temporary.isValid()) throw std::runtime_error("Cannot allocate private comparator directory");
        const auto payload = raw(capture, 0, 8192); QByteArray header(4096, 0); header.replace(0, 8, QByteArray("CAPDDR2\0", 8));
        for (const auto field : {std::pair<int, quint32>{8, 2}, {12, 4096}, {16, 32}, {20, 2}, {24, capture.header.sample_rate_hz}, {28, 50000000},
                               {32, 8192}, {36, 8192}, {40, quint32(payload.size())}, {80, 328449}, {84, 5000000}}) put(header, field.first, field.second, 4);
        put(header, 64, capture.records[0].tick, 8); put(header, 72, capture.records[8191].tick, 8); put(header, 96, checksum(payload), 4); put(header, 100, checksum(header.left(128)), 4);
        const QString path = temporary.path() + "/synthetic-spectrum.BIN"; QFile file(path); if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write comparator fixture");
        file.write(header); file.write(payload); file.close(); QFile sidecar(temporary.path() + "/synthetic-spectrum.meta.json"); sidecar.open(QIODevice::WriteOnly); sidecar.write("{\"synthetic\":true}"); sidecar.close();
        const auto loaded = load_capture(path.toUtf8().toStdString()); AnalysisConfig config; config.analog_gain = 1; config.sensor_sensitivity_v_per_g = 1;
        config.fft_size = 8192; config.spectrum_max_hz = 2000; config.execution_threads = 1; const auto reference = analyze(loaded, config);
        const auto actual = analyze_voltage_spectra(loaded.records, loaded.header.sample_rate_hz, loaded.header.clock_hz, loaded.header.adc_range_uv, 8192, 2000);
        bool equivalent = true;
        for (int channel = 0; channel < 3; ++channel) {
            equivalent = equivalent && actual[channel].frequency_hz == reference.spectra[channel].frequency_hz && actual[channel].segment_count == reference.spectra[channel].segment_count;
            for (std::size_t i = 0; i < actual[channel].peak_amplitude.size(); ++i)
                equivalent = equivalent && close(actual[channel].peak_amplitude[i], reference.spectra[channel].peak_amplitude[i]) && close(actual[channel].psd[i], reference.spectra[channel].psd[i]);
        }
        check("dedicated_voltage_fft_matches_normally_loaded_core_spectrum", equivalent);
        auto invalidRecords = loaded.records; ++invalidRecords[10].tick; bool rejected = false;
        try { analyze_voltage_spectra(invalidRecords, 50000, 50000000, 5000000); } catch (const AnalysisError&) { rejected = true; }
        check("voltage_fft_rejects_noncontiguous_snapshot", rejected);
        rejected = false; try { analyze_voltage_spectra(loaded.records, 50000, 50000000, 5000000, 32768); } catch (const AnalysisError&) { rejected = true; }
        check("voltage_fft_enforces_cpu_bound", rejected);

#ifndef LIVE_MODEL_ONLY
        AnalysisPane pane; pane.resize(764, 455); pane.setContentsMargins(20, 4, 20, 0); pane.show();
        check("pane_begins_real_frame_preview", pane.beginLive(first, true)); events(80);
        auto* stack = dynamic_cast<WaveformStack*>(pane.findChild<QWidget*>("waveformStack"));
        auto* follow = pane.findChild<QPushButton*>("liveFollow"); auto* span = pane.findChild<QDoubleSpinBox*>("liveWindow");
        if (!stack || !follow || !span) throw std::runtime_error("Live pane controls missing");
        stack->moveChannel(0, 1, 0); const auto selectedOrder = stack->panelOrder();
        const auto oldEnd = stack->plot(1)->explicitXRange();
        pane.acceptLiveFrame(frame(capture, 2000, 2000, 2)); events(80);
        check("live_25hz_display_follows_new_raw_samples", stack->plot(1)->explicitXRange() && oldEnd && stack->plot(1)->explicitXRange()->y() > oldEnd->y());
        check("live_refresh_preserves_user_merge_and_order", stack->panelOrder() == selectedOrder && stack->panelChannels(1) == QVector<int>({0, 1}));
        follow->click(); const auto frozen = stack->plot(1)->series()[0].points;
        pane.acceptLiveFrame(frame(capture, 4000, 2000, 3)); events(100);
        check("pause_freezes_display_without_stopping_live_ingestion", pane.isLive() && stack->plot(1)->series()[0].points == frozen && !span->isEnabled());
        pane.setView(2); events(20);
        auto* pausedSpeed = dynamic_cast<PlotCanvas*>(pane.findChild<QWidget*>("workbenchSpeed"));
        check("paused_page_switch_uses_the_same_frozen_raw_snapshot", pausedSpeed && !pausedSpeed->series().isEmpty() &&
            !pausedSpeed->series()[0].points.isEmpty() && close(pausedSpeed->series()[0].points.last().x(), frozen.last().x()));
        pane.setView(0); events(20);
        follow->click(); events(80);
        check("follow_resumes_latest_live_data", stack->plot(1)->series()[0].points != frozen && span->isEnabled());
        const auto acceptedBefore = pane.property("liveAcceptedFrames").toULongLong(), drawnBefore = pane.property("liveRenderedFrames").toULongLong();
        QEventLoop liveLoop; QTimer producer; producer.setTimerType(Qt::PreciseTimer); producer.setInterval(40); int delivered = 0;
        QElapsedTimer liveElapsed; liveElapsed.start();
        QObject::connect(&producer, &QTimer::timeout, &liveLoop, [&] {
            if (!pane.acceptLiveFrame(frame(capture, 6000 + std::size_t(delivered) * 2000, 2000, 4 + delivered))) passed = false;
            if (++delivered == 50) { producer.stop(); liveLoop.quit(); }
        });
        producer.start(); liveLoop.exec(); events(85);
        const auto acceptedNow = pane.property("liveAcceptedFrames").toULongLong(), drawnNow = pane.property("liveRenderedFrames").toULongLong();
        check("sustained_true_raw_frame_25hz_gui_updates", acceptedNow == acceptedBefore + 50 && drawnNow >= drawnBefore + 45 && liveElapsed.elapsed() <= 2500);
        report["live_raw_frames_delivered"] = delivered; report["live_gui_refreshes"] = double(drawnNow - drawnBefore);
        report["live_gui_elapsed_ms"] = double(liveElapsed.elapsed());
        const auto visibleRange = stack->plot(1)->explicitXRange();
        auto* cursorText = pane.findChild<QLabel*>("waveCursorReadout"); stack->plot(1)->selectAtX(.1); events(5);
        if (visibleRange) stack->plot(1)->selectAtX((visibleRange->x() + visibleRange->y()) * .5);
        check("live_cursor_uses_voltage_units", cursorText && cursorText->text().contains(" V"));
        pane.setView(2); events(40);
        auto* speedPlot = dynamic_cast<PlotCanvas*>(pane.findChild<QWidget*>("workbenchSpeed"));
        check("opening_speed_page_adopts_current_raw_snapshot", speedPlot && !speedPlot->series().isEmpty() && !speedPlot->series()[0].points.isEmpty());
        const auto hiddenWave = stack->plot(1)->series()[0].points;
        const auto speedEnd = speedPlot->series()[0].points.last().x();
        pane.acceptLiveFrame(frame(capture, 106000, 2000, 54)); events(85);
        check("hidden_wave_buffers_are_not_rebuilt_during_speed_updates", stack->plot(1)->series()[0].points == hiddenWave && speedPlot->series()[0].points.last().x() > speedEnd);
        pane.setView(0); events(40);
        check("returning_to_wave_page_immediately_adopts_latest_snapshot", stack->plot(1)->series()[0].points.last().x() > hiddenWave.last().x());
        if (!output.isEmpty()) for (const auto size : {QSize(764, 455), QSize(1020, 575)}) {
            pane.resize(size); events(60); pane.grab().save(output + QStringLiteral("/live-wave-%1x%2.png").arg(size.width()).arg(size.height()));
            pane.setView(1); events(250); pane.grab().save(output + QStringLiteral("/live-spectrum-%1x%2.png").arg(size.width()).arg(size.height()));
            pane.setView(2); events(30); pane.grab().save(output + QStringLiteral("/live-speed-%1x%2.png").arg(size.width()).arg(size.height())); pane.setView(0);
        }
        pane.endLive(QStringLiteral("测试结束")); check("ending_preview_stops_live_state", !pane.isLive());
        report["analysis_profile"] = pane.performanceProfile();
        if (qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE")) check("enabled_profile_records_model_and_analysis_stages",
            pane.performanceProfile().value("refresh_total").toObject().value("count").toDouble() >= 50 &&
            pane.performanceProfile().value("model").toObject().value("accept_total").toObject().value("count").toDouble() >= 54);
        pane.clear(); check("clear_drops_live_data", stack->plot(1)->series().isEmpty());
        auto noSpeedBytes = raw(capture, 0, 2000);
        for (int i = 0; i < 2000; ++i) { put(noSpeedBytes, i * 32 + 22, 1, 2); put(noSpeedBytes, i * 32 + 24, 0, 4); }
        pane.beginLive(replaceBytes(first, noSpeedBytes), true);
        bool duplicateWaiting = false; for (auto* value : pane.findChildren<QLabel*>()) duplicateWaiting = duplicateWaiting || value->text() == QStringLiteral("等待连续样本");
        check("empty_live_spectrum_hint_is_not_duplicated_in_toolbar", !duplicateWaiting);
        pane.setView(2); events(40); auto* note = pane.findChild<QLabel*>("speedTraceNote");
        check("entirely_missing_speed_hides_redundant_footer_note", note && note->isHidden() && note->text().isEmpty());
        pane.endLive(); pane.clear();
#endif
        report["preview_integrity_scope"] = "CRC and raw continuity of accepted preview frames only; never complete-file verification or diagnosis";
        report["source_scope"] = "Explicit synthetic fixture only; no hardware, physical sensor or accuracy claim";
    } catch (const std::exception& error) { report["error"] = QString::fromUtf8(error.what()); passed = false; }
    report["passed"] = passed; const auto json = QJsonDocument(report).toJson();
    if (!output.isEmpty()) { QFile file(output + "/live-preview.json"); if (file.open(QIODevice::WriteOnly)) file.write(json); }
    std::fwrite(json.constData(), 1, std::size_t(json.size()), stdout); return passed ? 0 : 1;
}
