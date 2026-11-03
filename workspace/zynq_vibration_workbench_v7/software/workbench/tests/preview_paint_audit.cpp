#include "live_preview_model.hpp"
#include "plot_canvas.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QBackingStore>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace vibration;
using namespace vibration::workbench;
namespace {
constexpr double pi = 3.14159265358979323846;
void put(QByteArray& bytes, int at, quint64 value, int count) {
    for (int i = 0; i < count; ++i) bytes[at + i] = char(value >> (8 * i));
}
QJsonObject frame(int first, int count, int sequence, double frequency, bool noise) {
    QByteArray bytes(count * 32, 0);
    for (int i = 0; i < count; ++i) {
        const int index = first + i, at = i * 32;
        put(bytes, at, index, 8); put(bytes, at + 8, quint64(index) * 1000, 8);
        for (int channel = 0; channel < 3; ++channel) {
            quint32 mixed = quint32(index) + quint32(channel + 1) * 0x9e3779b9u;
            mixed ^= mixed >> 16; mixed *= 0x7feb352du; mixed ^= mixed >> 15; mixed *= 0x846ca68bu; mixed ^= mixed >> 16;
            const double signal = noise ? double(mixed >> 8) / 8388608.0 - 1 :
                std::sin(2 * pi * frequency * index / 50000.0 + channel * .2);
            put(bytes, at + 16 + channel * 2, quint16(qint16(std::lround(signal * 12000))), 2);
        }
        put(bytes, at + 22, 1, 2);
    }
    return {{"schema", "vibration.capture.v2"}, {"type", "event"}, {"event", "preview"}, {"job_id", "synthetic_paint_audit"},
        {"generation", "1"}, {"frame_seq", sequence}, {"first_record", first}, {"record_count", count},
        {"committed_records", first + count}, {"requested_records", 1000000}, {"sample_rate_hz", 50000},
        {"clock_hz", 50000000}, {"ppr", 1}, {"adc_range_uv", 5000000}, {"channel_map", 328449}, {"record_bytes", 32},
        {"source", "test_fixture"}, {"hardware_confirmed", false}, {"synthetic", true}, {"source_flags", 1}, {"validity_flags", 27},
        {"raw_base64", QString::fromLatin1(bytes.toBase64())},
        {"crc32", double(crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()), bytes.size()))}, {"dropped_preview_records", 0}};
}
double percentile(std::vector<qint64> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end()); return values[std::size_t((values.size() - 1) * p)] / 1e6;
}
QJsonObject distribution(const std::vector<qint64>& values) {
    return {{"p50_ms", percentile(values, .5)}, {"p95_ms", percentile(values, .95)}, {"maximum_ms", percentile(values, 1)}};
}
QImage backingPixels(PlotCanvas& canvas) {
    auto* image = canvas.backingStore() ? dynamic_cast<QImage*>(canvas.backingStore()->paintDevice()) : nullptr;
    return image ? image->copy() : QImage{};
}
bool originalVertices(const QVector<QPointF>& displayed, const QVector<QPointF>& raw) {
    int cursor = 0;
    for (const auto& point : displayed) {
        if (!std::isfinite(point.y())) continue;
        while (cursor < raw.size() && raw[cursor].x() < point.x()) ++cursor;
        if (cursor == raw.size() || raw[cursor] != point) return false;
    }
    return true;
}
QJsonObject scenario(double seconds, double frequency, bool noise, int frameCount) {
    LivePreviewModel model; QString error; bool passed = true;
    int next = 0, sequence = 0;
    const int initial = int(seconds * 50000) + 4000;
    while (next < initial) {
        const int count = std::min(4000, initial - next); const auto event = frame(next, count, ++sequence, frequency, noise);
        const bool accepted = next == 0 ? model.begin(event, false, &error) : model.accept(event, &error);
        if (!accepted) return {{"passed", false}, {"error", error}, {"phase", "initial_fixture_accept"}};
        next += count;
    }
    PlotCanvas canvas; canvas.setEmbedded(true); canvas.setLegendVisible(false); canvas.setTraceWidth(1.0); canvas.resize(1020, 410);
    canvas.show(); QApplication::processEvents();
    std::vector<qint64> materialize, painting, frames, m4, raster, bounds;
    quint64 gaps = 0, antialiasedRuns = 0, integerRuns = 0; int colored = 0;
    double travel = 0;
    int backingFormat = QImage::Format_Invalid, backingDepth = 0, backingStride = 0;
    for (int iteration = 0; iteration < frameCount + 5; ++iteration) {
        const auto event = frame(next, 2000, ++sequence, frequency, noise); next += 2000;
        if (!model.accept(event, &error)) return {{"passed", false}, {"error", error}, {"phase", "fresh_fixture_accept"}};
        QElapsedTimer total; total.start(); QElapsedTimer stage; stage.start();
        auto view = model.view(seconds, true, true);
        const auto viewNs = stage.nsecsElapsed(); stage.restart();
        canvas.updateSeries(std::move(view.voltage), false); canvas.setXRange(view.startTime, view.endTime);
        canvas.repaint(); QApplication::processEvents();
        const auto paintNs = stage.nsecsElapsed(), frameNs = total.nsecsElapsed();
        const auto stats = canvas.renderStats();
        if (iteration >= 5) {
            materialize.push_back(viewNs); painting.push_back(paintNs); frames.push_back(frameNs);
            m4.push_back(stats.lastEnvelopeNs); raster.push_back(stats.lastRasterNs); bounds.push_back(stats.lastBoundsNs);
            antialiasedRuns += stats.lastAntialiasedTraceRuns; integerRuns += stats.lastIntegerTraceRuns;
            travel += stats.lastTraceVerticalTravelPixels;
        }
        for (const auto& point : canvas.displayPoints(0)) if (!std::isfinite(point.y())) ++gaps;
        // Read already-painted RAM backing pixels. Do not use grab/render,
        // which could repaint and hide a partial-update error.
        if (iteration == frameCount + 4) {
            auto* device = canvas.backingStore() ? dynamic_cast<QImage*>(canvas.backingStore()->paintDevice()) : nullptr;
            passed &= device && !device->isNull();
            if (device) {
                backingFormat = device->format(); backingDepth = device->depth(); backingStride = device->bytesPerLine();
                const auto rgb = device->convertToFormat(QImage::Format_RGB32);
                for (int y = 0; y < rgb.height(); ++y) {
                const auto* row = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
                for (int x = 0; x < rgb.width(); ++x)
                    if (std::abs(qRed(row[x]) - qGreen(row[x])) > 25 || std::abs(qGreen(row[x]) - qBlue(row[x])) > 25) ++colored;
                }
            }
        }
    }
    bool originals = true, partialExact = true;
    for (int channel = 0; channel < canvas.series().size(); ++channel)
        originals &= originalVertices(canvas.displayPoints(channel), canvas.series()[channel].points);
    const auto range = canvas.explicitXRange().value();
    for (double fraction : {.37, .63, .41}) {
        canvas.setCursor(range.x() + (range.y() - range.x()) * fraction); QApplication::processEvents();
        const auto partial = backingPixels(canvas);
        canvas.repaint(); QApplication::processEvents(); const auto full = backingPixels(canvas);
        partialExact &= !partial.isNull() && partial == full;
    }
    return {{"scope", "synthetic_fresh_frames_qt_ram_backing_no_sensor_no_scanout"}, {"seconds", seconds}, {"frequency_hz", frequency}, {"noise", noise},
        {"frames", frameCount}, {"passed", passed && gaps == 0 && colored > 500 && originals && partialExact}, {"error", error},
        {"trace_antialias_mode", canvas.traceAntialiasMode()}, {"antialiased_trace_runs", double(antialiasedRuns)}, {"integer_trace_runs", double(integerRuns)},
        {"mean_vertical_travel_physical_pixels", travel / frameCount},
        {"backing_image_format_enum", backingFormat}, {"backing_image_depth", backingDepth}, {"backing_bytes_per_line", backingStride},
        {"device_pixel_ratio", canvas.devicePixelRatioF()},
        {"original_vertices_only", originals}, {"partial_cursor_pixels_equal_full_redraw", partialExact}, {"continuous_stream_nan_breaks", double(gaps)},
        {"colored_backing_pixels", colored}, {"view_materialize", distribution(materialize)}, {"paint_and_events", distribution(painting)},
        {"view_plus_paint", distribution(frames)}, {"envelope", distribution(m4)}, {"raster", distribution(raster)}, {"bounds", distribution(bounds)},
        {"preview_dropped_records", double(model.info().previewDroppedRecords)}, {"model_profile", model.performanceProfile()}};
}
QJsonObject gapScenario() {
    LivePreviewModel model; QString error;
    const bool accepted = model.begin(frame(0, 4000, 1, 1000, false), false, &error) && model.accept(frame(8000, 4000, 2, 1000, false), &error);
    auto view = model.view(.25, true, false);
    PlotCanvas canvas; canvas.setEmbedded(true); canvas.resize(1020, 410); canvas.show();
    canvas.updateSeries(view.voltage, false); canvas.setXRange(view.startTime, view.endTime); QApplication::processEvents();
    int breaks = 0; for (const auto& point : canvas.displayPoints(0)) if (!std::isfinite(point.y())) ++breaks;
    bool originals = true; for (int channel = 0; channel < canvas.series().size(); ++channel)
        originals &= originalVertices(canvas.displayPoints(channel), view.voltage[channel].points);
    return {{"scope", "deliberately_missing_4000_preview_records_synthetic_only"}, {"passed", accepted && breaks == 1 && originals && model.info().previewDroppedRecords == 4000},
        {"error", error}, {"preview_dropped_records", double(model.info().previewDroppedRecords)}, {"display_nan_breaks", breaks}, {"original_vertices_only", originals},
        {"missing_interval_start_seconds", .08}, {"missing_interval_end_seconds", .16},
        {"interpretation", "The renderer deliberately disconnects this 80ms missing preview interval; no interpolation or invented samples."}};
}
QJsonObject adaptivePolicyScenario() {
    // Independently render the same vertices using each policy. Sparse auto
    // must match forced-AA exactly; dense auto must match forced-integer
    // exactly. This checks a raster-policy choice, not a looser pixel metric.
    const bool hadMode = qEnvironmentVariableIsSet("AX7_PLOT_TRACE_AA");
    const QByteArray originalMode = qgetenv("AX7_PLOT_TRACE_AA");
    struct Result { QImage image; QVector<QPointF> points; PlotCanvas::RenderStats stats; };
    const auto draw = [&](const QByteArray& mode, const QVector<QPointF>& points, double end) {
        qputenv("AX7_PLOT_TRACE_AA", mode);
        PlotCanvas canvas; canvas.setEmbedded(true); canvas.setLegendVisible(false); canvas.resize(1020, 410);
        canvas.setChart({}, {}, {}, {{"fixture", QColor("#3769c7"), points}}); canvas.setXRange(0, end);
        canvas.show(); QApplication::processEvents(); canvas.repaint(); QApplication::processEvents();
        return Result{backingPixels(canvas), canvas.displayPoints(0), canvas.renderStats()};
    };
    QVector<QPointF> sparse, dense;
    for (int i = 0; i <= 100; ++i) sparse.push_back({i / 100.0, std::sin(2 * pi * i / 100.0)});
    for (int i = 0; i <= 20000; ++i) dense.push_back({i / 50000.0, std::sin(2 * pi * 1000 * i / 50000.0)});
    const auto sparseAuto = draw("auto", sparse, 1), sparseAa = draw("1", sparse, 1);
    const auto denseAuto = draw("auto", dense, .4), denseInteger = draw("0", dense, .4);
    const auto denseAa = draw("1", dense, .4);
    if (hadMode) qputenv("AX7_PLOT_TRACE_AA", originalMode); else qunsetenv("AX7_PLOT_TRACE_AA");
    const bool sparseExact = !sparseAuto.image.isNull() && sparseAuto.image == sparseAa.image;
    const bool denseExact = !denseAuto.image.isNull() && denseAuto.image == denseInteger.image;
    const bool vertices = sparseAuto.points == sparseAa.points && denseAuto.points == denseInteger.points && denseAuto.points == denseAa.points &&
        originalVertices(sparseAuto.points, sparse) && originalVertices(denseAuto.points, dense);
    const bool modes = sparseAuto.stats.lastAntialiasedTraceRuns == 1 && sparseAuto.stats.lastIntegerTraceRuns == 0 &&
        denseAuto.stats.lastAntialiasedTraceRuns == 0 && denseAuto.stats.lastIntegerTraceRuns == 1;
    return {{"passed", sparseExact && denseExact && vertices && modes}, {"sparse_auto_pixels_equal_forced_aa", sparseExact},
        {"dense_auto_pixels_equal_forced_integer", denseExact}, {"identical_original_vertices_across_policies", vertices},
        {"sparse_uses_aa_dense_uses_integer", modes}, {"dense_aa_and_integer_pixels_differ", denseAuto.image != denseAa.image},
        {"threshold", "finite run >1 vertex and >16 vertical-travel physical pixels per physical plot column"}};
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); qa::configureFonts(app);
    int frameCount = 41;
    if (qEnvironmentVariableIsSet("AX7_PAINT_AUDIT_FRAMES")) {
        bool ok = false; frameCount = qEnvironmentVariableIntValue("AX7_PAINT_AUDIT_FRAMES", &ok);
        if (!ok || frameCount < 1 || frameCount > 201) {
            std::fputs("AX7_PAINT_AUDIT_FRAMES must be an integer in [1,201]\n", stderr); return 2;
        }
    }
    QJsonArray scenarios; bool passed = true;
    for (double seconds : {.25, 2.0}) for (bool noise : {false, true}) {
        auto row = scenario(seconds, 1000, noise, frameCount); passed &= row["passed"].toBool(); scenarios.append(row);
    }
    const auto gap = gapScenario(); passed &= gap["passed"].toBool();
    const auto adaptive = adaptivePolicyScenario(); passed &= adaptive["passed"].toBool();
    QJsonObject report{{"passed", passed}, {"direct_widget_paint", qgetenv("AX7_PLOT_DIRECT_PAINT") == "1"},
        {"trace_antialias", qgetenv("AX7_PLOT_TRACE_AA") != "0"},
        {"trace_antialias_requested", QString::fromLatin1(qgetenv("AX7_PLOT_TRACE_AA"))},
        {"qt_build_version", QT_VERSION_STR}, {"qt_runtime_version", qVersion()}, {"build_cpu_architecture", QSysInfo::buildCpuArchitecture()},
        {"runtime_cpu_architecture", QSysInfo::currentCpuArchitecture()}, {"build_abi", QSysInfo::buildAbi()},
        {"runtime_kernel_type", QSysInfo::kernelType()}, {"runtime_product_type", QSysInfo::productType()},
        {"platform_plugin", QApplication::platformName()}, {"warmup_frames_per_scenario", 5},
        {"scenarios", scenarios}, {"deliberate_gap", gap}, {"adaptive_trace_policy", adaptive},
        {"limitations", "Synthetic Qt RAM backing-store timing on the reported runtime only; no physical HDMI flicker, driver PAN, touch device or sensor capture verification."}};
    const auto bytes = QJsonDocument(report).toJson();
    if (argc > 1) { QFile out(QString::fromUtf8(argv[1])); if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size()) return 2; }
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stdout); return passed ? 0 : 1;
}
