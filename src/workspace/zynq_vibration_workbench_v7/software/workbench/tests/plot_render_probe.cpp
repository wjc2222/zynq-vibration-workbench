#include "plot_canvas.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QBackingStore>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstring>
#include <vector>

using vibration::workbench::PlotCanvas;
using vibration::workbench::PlotSeries;
namespace {
constexpr double pi = 3.14159265358979323846;
const QColor colors[]{QColor("#3769c7"), QColor("#268878"), QColor("#b78032")};
QVector<PlotSeries> signal(int count, double phase = 0) {
    QVector<PlotSeries> result;
    for (int channel = 0; channel < 3; ++channel) {
        PlotSeries series{QStringLiteral("CH%1").arg(channel + 1), colors[channel], {}}; series.points.reserve(count);
        for (int i = 0; i < count; ++i) {
            const double x = double(i) / std::max(1, count - 1);
            double y = (1.0 - channel * .16) * std::sin(2 * pi * 8.35 * x + channel * .17 + phase);
            if (i == count / 3 + channel * 3) y = 3.5 + channel;
            if (i == count * 2 / 3 + channel * 5) y = -3.0 - channel;
            series.points.push_back({x, y});
        }
        result.push_back(std::move(series));
    }
    return result;
}
qint64 percentile(std::vector<qint64> samples, double fraction) {
    std::sort(samples.begin(), samples.end());
    return samples[std::size_t((samples.size() - 1) * fraction)];
}
void paint(PlotCanvas& canvas) { canvas.repaint(); QApplication::processEvents(); }
void update(PlotCanvas& canvas, const QVector<PlotSeries>& data) {
#ifdef PLOT_RENDER_BASELINE
    canvas.setChart({}, QStringLiteral("s"), QStringLiteral("g"), data);
#else
    canvas.updateSeries(data);
#endif
}
QJsonObject benchmark(PlotCanvas& canvas, int count) {
    const auto a = signal(count, 0), b = signal(count, .04);
    std::vector<qint64> samples; samples.reserve(61);
    for (int frame = 0; frame < 71; ++frame) {
        QElapsedTimer timer; timer.start(); update(canvas, frame % 2 ? a : b); paint(canvas);
        if (frame >= 10) samples.push_back(timer.nsecsElapsed());
    }
    QJsonObject result{{"points_per_channel", count}, {"channels", 3}, {"samples", int(samples.size())},
        {"frame_p50_ns", double(percentile(samples, .5))}, {"frame_p95_ns", double(percentile(samples, .95))},
        {"frame_max_ns", double(*std::max_element(samples.begin(), samples.end()))},
        {"p95_within_20ms", percentile(samples, .95) <= 20000000}};
#ifndef PLOT_RENDER_BASELINE
    result.insert("input_points", double(canvas.renderStats().inputPoints));
    result.insert("drawn_points", double(canvas.renderStats().drawnPoints));
    result.insert("reduction_ns", double(canvas.renderStats().lastReductionNs));
    result.insert("direct_paints", double(canvas.renderStats().directPaints));
    result.insert("cached_image_blits", double(canvas.renderStats().cachedImageBlits));
    if (qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE")) {
        const auto stats = canvas.renderStats();
        result.insert("last_static_ns", double(stats.lastStaticNs)); result.insert("bounds_ns", double(stats.lastBoundsNs));
        result.insert("m4_ns", double(stats.lastEnvelopeNs)); result.insert("map_ns", double(stats.lastMapNs));
        result.insert("raster_ns", double(stats.lastRasterNs)); result.insert("axes_ns", double(stats.lastAxesNs));
        result.insert("setup_ns", double(stats.lastSetupNs)); result.insert("other_ns", double(stats.lastOtherNs));
        result.insert("widget_blit_ns", double(stats.lastBlitNs));
        result.insert("m4_calls", int(stats.lastEnvelopeBuilds));
    }
#endif
    return result;
}
#ifndef PLOT_RENDER_BASELINE
QJsonObject timedFrames(PlotCanvas& canvas, int hz, int frameCount) {
    const auto a = signal(12500, 0), b = signal(12500, .04);
    QEventLoop loop; QTimer timer; timer.setTimerType(Qt::PreciseTimer); timer.setInterval(1000 / hz);
    QElapsedTimer elapsed; std::vector<qint64> rendering, intervals; rendering.reserve(frameCount); intervals.reserve(frameCount);
    qint64 last = 0; int frames = 0;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        const auto now = elapsed.nsecsElapsed(); if (last) intervals.push_back(now - last); last = now;
        QElapsedTimer frame; frame.start(); canvas.updateSeries(frames % 2 ? a : b); canvas.repaint(); rendering.push_back(frame.nsecsElapsed());
        if (++frames == frameCount) { timer.stop(); loop.quit(); }
    });
    elapsed.start(); timer.start(); loop.exec();
    const double actualHz = frames * 1e9 / elapsed.nsecsElapsed();
    return QJsonObject{{"target_hz", hz}, {"frames", frames}, {"delivered_hz", actualHz},
        {"frame_p95_ns", double(percentile(rendering, .95))}, {"interval_p95_ns", double(percentile(intervals, .95))},
        {"keeps_target_within_10_percent", actualHz >= hz * .9 && percentile(rendering, .95) < 20000000}};
}
#endif
bool contains(const QVector<QPointF>& points, const QPointF& expected) {
    return std::any_of(points.begin(), points.end(), [&](const auto& p) { return p == expected; });
}
#ifndef PLOT_RENDER_BASELINE
// Read the existing raster backing store without asking QWidget::grab/render
// to repaint it. This catches stale cursor strips and newly exposed scroll
// pixels which a full screenshot repaint could accidentally repair.
QImage backingPixels(QWidget& widget) {
    auto* store = widget.backingStore();
    const auto* image = store ? dynamic_cast<QImage*>(store->paintDevice()) : nullptr;
    return image ? image->copy() : QImage{};
}
QJsonObject pixelDifference(const QImage& left, const QImage& right) {
    QJsonObject result{{"pixels_identical", left == right}, {"same_geometry", left.size() == right.size() && left.devicePixelRatio() == right.devicePixelRatio()}};
    int maximum = 0, changed = 0;
    if (!left.isNull() && !right.isNull() && left.size() == right.size()) {
        const auto a = left.convertToFormat(QImage::Format_ARGB32), b = right.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < a.height(); ++y) {
            const auto* ap = reinterpret_cast<const QRgb*>(a.constScanLine(y)); const auto* bp = reinterpret_cast<const QRgb*>(b.constScanLine(y));
            for (int x = 0; x < a.width(); ++x) if (ap[x] != bp[x]) {
                ++changed;
                for (int shift : {0, 8, 16, 24}) maximum = std::max(maximum, std::abs(int((ap[x] >> shift) & 255) - int((bp[x] >> shift) & 255)));
            }
        }
    } else maximum = 255;
    result["maximum_channel_difference"] = maximum; result["changed_pixels"] = changed;
    return result;
}
QJsonObject compareDirectPaint(const QString& output) {
    const auto originalDirect = qgetenv("AX7_PLOT_DIRECT_PAINT"), originalAxes = qgetenv("AX7_PLOT_AXES_MODE");
    const bool hadDirect = qEnvironmentVariableIsSet("AX7_PLOT_DIRECT_PAINT"), hadAxes = qEnvironmentVariableIsSet("AX7_PLOT_AXES_MODE");
    QJsonObject result; QJsonArray scenes; bool all = true;
    for (const auto* axes : {"plain", "text_cache", "layer_cache", "both"}) {
        qputenv("AX7_PLOT_AXES_MODE", axes); qputenv("AX7_PLOT_DIRECT_PAINT", "0");
        QScrollArea cached; cached.setFrameShape(QFrame::NoFrame); cached.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn); cached.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* a = new PlotCanvas; cached.setWidget(a); cached.resize(740, 146); a->setFixedSize(724, 220);
        qputenv("AX7_PLOT_DIRECT_PAINT", "1");
        QScrollArea direct; direct.setFrameShape(QFrame::NoFrame); direct.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn); direct.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* b = new PlotCanvas; direct.setWidget(b); direct.resize(740, 146); b->setFixedSize(724, 220);
        all = all && !a->directPaintEnabled() && b->directPaintEnabled();
        auto raw = signal(25000); const double nan = std::numeric_limits<double>::quiet_NaN();
        raw[1].points[13000].setY(nan); raw[1].points[13001].setY(nan); raw[1].points[13002].setY(2.5); raw[1].points[13003].setY(nan);
        for (auto* canvas : {a, b}) {
            canvas->setEmbedded(true); canvas->setLegendVisible(false);
            canvas->setChart(QStringLiteral("实时波形"), QStringLiteral("时间 s"), QStringLiteral("电压 V"), raw);
            canvas->setXRange(.2, .8); canvas->setReferenceLines({{.5, 1}});
        }
        cached.show(); direct.show(); QApplication::processEvents();
        const auto compare = [&](const char* scene) {
            QApplication::processEvents();
            const auto left = backingPixels(cached), right = backingPixels(direct);
            const bool readable = !left.isNull() && !right.isNull();
            auto metrics = pixelDifference(left, right);
            // Qt's raster-device/clip rounding may differ by 1-2 intensity
            // levels per AA draw, reaching 3 where cursor/reference overlaps
            // a trace. Report exact differences, and reject larger changes.
            const bool equivalent = readable && metrics["same_geometry"].toBool() && metrics["maximum_channel_difference"].toInt() <= 3;
            all = all && equivalent; metrics["within_three_channel_levels"] = equivalent;
            metrics["axes_mode"] = QString::fromLatin1(axes); metrics["scene"] = scene; metrics["backing_store_readable"] = readable;
            metrics["cached_dpr"] = a->devicePixelRatioF(); metrics["direct_dpr"] = b->devicePixelRatioF(); scenes.append(metrics);
            if (!output.isEmpty() && (QByteArray(scene) == "cursor_moved_partial_repaint" || QByteArray(scene) == "scroll_bottom_after_refresh")) {
                left.save(output + QStringLiteral("/pair-%1-%2-cached.png").arg(QString::fromLatin1(axes), QString::fromLatin1(scene)));
                right.save(output + QStringLiteral("/pair-%1-%2-direct.png").arg(QString::fromLatin1(axes), QString::fromLatin1(scene)));
            }
        };
        compare("initial_clipped_viewport");
        for (double cursor : {.31, .69, .405}) { a->setCursor(cursor); b->setCursor(cursor); QApplication::processEvents(); }
        compare("cursor_moved_partial_repaint");
        a->setCursor({}); b->setCursor({}); compare("cursor_cleared_partial_repaint");
        cached.verticalScrollBar()->setValue(49); direct.verticalScrollBar()->setValue(49); compare("scroll_newly_exposed_pixels");
        auto changed = raw; changed[0].points[11000].setY(91); changed[2].points[14000].setY(-83);
        a->updateSeries(changed, false); b->updateSeries(changed, false); compare("new_extrema_in_clipped_viewport");
        cached.verticalScrollBar()->setValue(cached.verticalScrollBar()->maximum()); direct.verticalScrollBar()->setValue(direct.verticalScrollBar()->maximum()); compare("scroll_bottom_after_refresh");
        auto alternate = a->font(); alternate.setWeight(QFont::DemiBold); a->setFont(alternate); b->setFont(alternate); compare("font_change_invalidates_axes");
        cached.resize(1040, 178); direct.resize(1040, 178); a->setFixedSize(1024, 250); b->setFixedSize(1024, 250); compare("resize_and_dpr_geometry");
        for (auto* canvas : {a, b}) { canvas->setChannelVisible(0, false); canvas->setChannelVisible(2, false); canvas->setCursor(.43); }
        compare("channel_visibility_and_cursor");
        for (auto* canvas : {a, b}) { canvas->setChart({}, {}, {}, {}, QStringLiteral("等待连续样本")); canvas->clearXRange(); }
        compare("empty_state_is_opaque");
        cached.hide(); direct.hide();
        for (auto* canvas : {a, b}) canvas->setChart({}, QStringLiteral("s"), QStringLiteral("V"), raw);
        cached.show(); direct.show(); compare("hidden_refresh_then_expose");
        const auto sa = a->renderStats(), sb = b->renderStats();
        all = all && sa.directPaints == 0 && sa.cachedImageBlits > 0 && sb.directPaints > 0 && sb.cachedImageBlits == 0;
    }
    if (hadDirect) qputenv("AX7_PLOT_DIRECT_PAINT", originalDirect); else qunsetenv("AX7_PLOT_DIRECT_PAINT");
    if (hadAxes) qputenv("AX7_PLOT_AXES_MODE", originalAxes); else qunsetenv("AX7_PLOT_AXES_MODE");
    result["scenes"] = scenes; result["passed"] = all;
    result["readback_scope"] = "Qt raster backing-store image copied without grab/render/repaint; paired off/on canvases, all four axes modes. Exact equality and changed-pixel counts are recorded; allowable AA composition rounding is at most 3/255 per channel (overlapping AA draws), with identical geometry. This is bounded visual equivalence, not byte identity.";
    return result;
}
bool samePointBytes(const QVector<QPointF>& a, const QVector<QPointF>& b) {
    return a.size() == b.size() && (a.isEmpty() || std::memcmp(a.constData(), b.constData(), std::size_t(a.size()) * sizeof(QPointF)) == 0);
}
QJsonObject testSharedM4() {
    const auto original = qgetenv("AX7_PLOT_SHARED_M4"); const bool wasSet = qEnvironmentVariableIsSet("AX7_PLOT_SHARED_M4");
    QJsonObject result; bool all = true;
    const auto check = [&](const char* name, bool ok) { result[name] = ok; all = all && ok; };
    qputenv("AX7_PLOT_SHARED_M4", "0"); PlotCanvas reference;
    qputenv("AX7_PLOT_SHARED_M4", "1"); PlotCanvas first, second;
    auto raw = signal(25000); const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (int channel = 0; channel < raw.size(); ++channel) {
        for (int index : {10000, 10001, 10003, 10005}) raw[channel].points[index].setY(nan);
        raw[channel].points[10002].setY(13 + channel); raw[channel].points[10004].setY(-12 - channel);
    }
    const auto configure = [&](PlotCanvas& canvas, const QVector<PlotSeries>& data) {
        canvas.resize(724, 220); canvas.setEmbedded(true); canvas.setLegendVisible(false);
        canvas.setChart({}, QStringLiteral("s"), QStringLiteral("V"), data); canvas.setXRange(0, 1); canvas.show(); paint(canvas);
    };
    configure(reference, raw); configure(first, raw);
    const auto beforeSecond = first.sharedM4Profile(); configure(second, raw);
    check("independent_canvases_share_original_buffer_envelopes", second.sharedM4Profile()["hits"].toDouble() >= beforeSecond["hits"].toDouble() + 3 &&
        second.sharedM4Profile()["per_canvas_hits"].toDouble() >= 3);
    const auto identical = [&](PlotCanvas& canvas) {
        for (int i = 0; i < reference.series().size(); ++i) if (!samePointBytes(reference.displayPoints(i), canvas.displayPoints(i))) return false;
        return reference.yTickLabels() == canvas.yTickLabels() && reference.plotArea() == canvas.plotArea();
    };
    check("shared_output_exactly_matches_independent_M4_including_nan_bits", identical(first) && identical(second));
    const auto hitBefore = second.sharedM4Profile(); second.updateSeries(raw, false); paint(second);
    check("new_local_revision_with_same_owned_source_hits", second.sharedM4Profile()["per_canvas_hits"].toDouble() >= hitBefore["per_canvas_hits"].toDouble() + 3 && identical(second));
    auto changed = raw; changed[0].points[1234].setY(123.456);
    const auto cowBefore = second.sharedM4Profile(); second.updateSeries(changed, false); reference.updateSeries(changed, false); paint(reference); paint(second);
    check("copy_on_write_new_buffer_misses_and_keeps_new_peak", changed[0].points.constData() != raw[0].points.constData() &&
        second.sharedM4Profile()["per_canvas_misses"].toDouble() > cowBefore["per_canvas_misses"].toDouble() &&
        contains(second.displayPoints(0), changed[0].points[1234]) && identical(second));
    check("copy_on_write_does_not_modify_other_canvas_raw_samples", first.series()[0].points.constData() == raw[0].points.constData() &&
        first.series()[0].points[1234] == raw[0].points[1234]);
    const auto rangeBefore = second.sharedM4Profile(); second.setXRange(.17, .83); reference.setXRange(.17, .83); paint(reference); paint(second);
    check("different_exact_range_misses_without_changing_M4", second.sharedM4Profile()["per_canvas_misses"].toDouble() > rangeBefore["per_canvas_misses"].toDouble() && identical(second));
    const auto columnsBefore = second.sharedM4Profile(); second.resize(791, 220); reference.resize(791, 220); paint(reference); paint(second);
    check("different_physical_columns_miss_without_changing_M4", second.sharedM4Profile()["per_canvas_misses"].toDouble() > columnsBefore["per_canvas_misses"].toDouble() && identical(second));
    second.hide(); reference.hide(); first.hide();

    // Force the QWidget object itself to reuse exactly the same C++ storage;
    // dataRevision restarts here and must never identify shared source data.
    std::optional<PlotCanvas> slot; slot.emplace(); configure(*slot, raw); const auto* slotAddress = &*slot;
    slot.reset(); slot.emplace(); const auto restart = slot->sharedM4Profile(); configure(*slot, raw);
    check("destroy_recreate_same_source_survives_widget_slot_reuse", &*slot == slotAddress &&
        slot->sharedM4Profile()["per_canvas_hits"].toDouble() >= 3 && slot->sharedM4Profile()["hits"].toDouble() > restart["hits"].toDouble());
    slot.reset(); slot.emplace(); const auto fresh = signal(25000, .321); const auto newSource = slot->sharedM4Profile(); configure(*slot, fresh);
    check("same_widget_slot_and_revision_with_different_source_misses", &*slot == slotAddress && fresh[0].points.constData() != raw[0].points.constData() &&
        slot->sharedM4Profile()["per_canvas_misses"].toDouble() >= 3 && slot->sharedM4Profile()["misses"].toDouble() > newSource["misses"].toDouble());
    slot.reset();

    // Same numeric extent but different IEEE zero bits is a distinct key.
    configure(first, raw); const auto zeroBefore = first.sharedM4Profile();
    second.setChart({}, QStringLiteral("s"), QStringLiteral("V"), raw); second.resize(724, 220); second.setXRange(-0.0, 1); second.show(); paint(second);
    check("range_key_keeps_exact_signed_zero_bits", second.sharedM4Profile()["misses"].toDouble() >= zeroBefore["misses"].toDouble() + 3);

    // Capacity, not only size, must be charged: a tiny view may own a large
    // reserved allocation. It still receives the unchanged original fallback.
    auto reserved = signal(100)[0]; reserved.points.reserve(600000);
    const auto capacityBefore = second.sharedM4Profile(); second.setXRange(0, 1); second.updateSeries({reserved}, false); paint(second);
    check("oversized_capacity_bypasses_but_keeps_original_points", second.sharedM4Profile()["per_canvas_bypasses"].toDouble() > capacityBefore["per_canvas_bypasses"].toDouble() &&
        samePointBytes(second.displayPoints(0), reserved.points));
    auto huge = signal(600000)[0]; const auto hugeBefore = second.sharedM4Profile();
    second.updateSeries({huge}, false); reference.updateSeries({huge}, false); reference.resize(724, 220); reference.setXRange(0, 1); reference.show(); paint(reference); paint(second);
    check("oversized_raw_input_bypasses_and_matches_independent_reduction", second.sharedM4Profile()["per_canvas_bypasses"].toDouble() > hugeBefore["per_canvas_bypasses"].toDouble() && identical(second));
    reference.hide(); first.hide();

    // Six small, distinct owned sources fit the byte cap. Touching entry0
    // before inserting entry6 must protect0 and evict1 (actual LRU, not FIFO).
    QVector<PlotSeries> owners; for (int i = 0; i < 7; ++i) owners.push_back(signal(9000, i * .013)[0]);
    const auto adopt = [&](const PlotSeries& line) { second.updateSeries({line}, false); paint(second); };
    const auto boundedBefore = second.sharedM4Profile(); for (int i = 0; i < 6; ++i) adopt(owners[i]);
    const auto oldest = second.sharedM4Profile(); adopt(owners[0]); const auto touched = second.sharedM4Profile();
    adopt(owners[6]); const auto inserted = second.sharedM4Profile(); adopt(owners[0]); const auto protectedEntry = second.sharedM4Profile(); adopt(owners[1]);
    check("bounded_cache_uses_LRU_not_FIFO", touched["hits"].toDouble() > oldest["hits"].toDouble() &&
        inserted["evictions"].toDouble() > touched["evictions"].toDouble() && protectedEntry["hits"].toDouble() > inserted["hits"].toDouble() &&
        second.sharedM4Profile()["misses"].toDouble() > protectedEntry["misses"].toDouble());
    // Also exercise the byte boundary with >6MiB of live source allocations.
    for (int i = 0; i < 8; ++i) adopt(signal(100000, i * .019)[0]);
    const auto bounded = second.sharedM4Profile();
    check("entries_and_retained_capacity_bytes_are_bounded", bounded["entries"].toInt() <= bounded["max_entries"].toInt() &&
        bounded["retained_bytes"].toDouble() <= bounded["max_bytes"].toDouble() && bounded["peak_bytes"].toDouble() <= bounded["max_bytes"].toDouble() &&
        bounded["evictions"].toDouble() > boundedBefore["evictions"].toDouble());
    auto unordered = signal(25000)[0]; std::swap(unordered.points[500], unordered.points[1500]); adopt(unordered);
    check("unordered_fallback_is_preserved_by_shared_cache", samePointBytes(second.displayPoints(0), unordered.points));
    // Once the cache is the only owner, subsequent equal-sized allocations
    // cannot recycle that live point buffer into a false pointer identity.
    const QPointF* retainedAddress = nullptr;
    { auto transient = signal(25000)[0]; retainedAddress = transient.points.constData(); adopt(transient); }
    second.updateSeries({}, false); paint(second); const auto lifetime = second.sharedM4Profile(); bool reused = false;
    for (int i = 0; i < 64; ++i) { QVector<QPointF> another(25000); reused = reused || another.constData() == retainedAddress; }
    check("cache_ownership_prevents_freed_source_pointer_reuse", !reused && second.sharedM4Profile()["evictions"] == lifetime["evictions"]);
    result["final_global_profile"] = second.sharedM4Profile();
    result["dpr"] = second.devicePixelRatioF();
    result["dpr_scope"] = "Actual widget DPR tested by separate Qt processes at1 and2; key also includes exact DPR bits even when physical columns coincide. No fake screen/DPR setter is used.";
    result["passed"] = all;
    if (wasSet) qputenv("AX7_PLOT_SHARED_M4", original); else qunsetenv("AX7_PLOT_SHARED_M4");
    return result;
}
#endif
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); vibration::workbench::qa::configureFonts(app);
    const QString output = argc > 1 ? QString::fromUtf8(argv[1]) : QString();
    if (!output.isEmpty() && !QDir().mkpath(output)) return 2;
    bool passed = true; QJsonObject report{{"font_audit", vibration::workbench::qa::fontAudit(app.font())}};
    const auto check = [&](const char* name, bool ok) { report.insert(name, ok); passed = passed && ok; };
    PlotCanvas canvas; canvas.resize(1024, 220); canvas.setEmbedded(true); canvas.setLegendVisible(false); canvas.show();
    const auto requestedMode = QString::fromLatin1(qgetenv("AX7_PLOT_AXES_MODE"));
    const QString expectedMode = QStringList{"text_cache", "layer_cache", "both"}.contains(requestedMode) ? requestedMode : QStringLiteral("plain");
    check("requested_axes_mode_or_safe_plain_fallback_is_effective", canvas.axesMode() == expectedMode);
    report["axes_mode_requested"] = requestedMode; report["axes_mode_effective"] = canvas.axesMode();
    report["direct_paint_requested"] = QString::fromLatin1(qgetenv("AX7_PLOT_DIRECT_PAINT"));
    report["direct_paint_effective"] = canvas.directPaintEnabled();
    check("direct_paint_requires_exact_one_opt_in", canvas.directPaintEnabled() == (qgetenv("AX7_PLOT_DIRECT_PAINT") == "1"));
    check("shared_M4_requires_exact_one_opt_in", canvas.sharedM4Enabled() == (qgetenv("AX7_PLOT_SHARED_M4") == "1"));
    canvas.setChart({}, QStringLiteral("s"), QStringLiteral("g"), signal(1800)); canvas.setXRange(0, 1); paint(canvas);
    QJsonArray benchmarks;
    for (const int count : {1800, 12500, 50000, 250000}) benchmarks.push_back(benchmark(canvas, count));
    report.insert("benchmarks_1024x220", benchmarks);
#ifndef PLOT_RENDER_BASELINE
    QJsonArray widths;
    for (const double width : {1.0, 1.2, 1.35}) {
        canvas.setTraceWidth(width); auto result = benchmark(canvas, 12500);
        result["logical_width"] = width; widths.push_back(result);
        if (!output.isEmpty()) canvas.grab().save(output + QStringLiteral("/trace-width-%1.png").arg(width));
    }
    canvas.setTraceWidth(1.0); report.insert("polyline_width_comparison", widths);
    if (qFuzzyCompare(canvas.devicePixelRatioF(), 1.0)) {
        bool budget = true; for (const auto& row : benchmarks) budget = budget && row.toObject().value("p95_within_20ms").toBool();
        check("all_tested_dpr1_host_frame_p95_within_20ms", budget);
    }
    QJsonArray sustained;
    for (const int hz : {25, 50}) {
        const auto result = timedFrames(canvas, hz, 60); sustained.push_back(result);
        if (qFuzzyCompare(canvas.devicePixelRatioF(), 1.0)) check(hz == 25 ? "sustained_25hz_host_updates" : "sustained_50hz_host_updates",
            result.value("keeps_target_within_10_percent").toBool());
    }
    report.insert("timed_12500_points_per_channel", sustained);
    const auto raw = signal(250000); canvas.updateSeries(raw); paint(canvas);
    bool peaks = true, ordered = true, originals = true;
    for (int channel = 0; channel < 3; ++channel) {
        const auto reduced = canvas.displayPoints(channel);
        const auto& input = raw[channel].points;
        const auto peak = std::max_element(input.begin(), input.end(), [](auto a, auto b) { return a.y() < b.y(); });
        const auto trough = std::min_element(input.begin(), input.end(), [](auto a, auto b) { return a.y() < b.y(); });
        peaks = peaks && contains(reduced, *peak) && contains(reduced, *trough) && contains(reduced, input.first()) && contains(reduced, input.last());
        double last = -std::numeric_limits<double>::infinity();
        for (const auto& point : reduced) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) continue;
            ordered = ordered && point.x() >= last; last = point.x();
            const auto index = std::size_t(std::llround(point.x() * (input.size() - 1)));
            originals = originals && index < std::size_t(input.size()) && input[int(index)] == point;
        }
    }
    check("single_sample_positive_negative_peaks_and_endpoints_preserved", peaks);
    check("reduced_points_keep_source_time_order", ordered);
    check("display_points_are_original_samples_not_interpolation", originals);
    check("dense_paths_bounded_by_physical_pixel_columns", canvas.renderStats().drawnPoints <=
        quint64(3 * (qCeil(canvas.plotArea().width() * canvas.devicePixelRatioF()) + 1) * 4));
    const auto before = canvas.renderStats();
    for (int i = 0; i < 80; ++i) { canvas.setCursor(.2 + i * .005); paint(canvas); }
    check("cursor_does_not_resample_data", canvas.renderStats().reductions == before.reductions);
    check("cursor_redraw_uses_selected_presentation_path", canvas.directPaintEnabled() ?
        canvas.renderStats().directPaints > before.directPaints && canvas.renderStats().cachedImageBlits == 0 :
        canvas.renderStats().staticBuilds == before.staticBuilds && canvas.renderStats().directPaints == 0);
    const auto sourcePointer = canvas.series()[0].points.constData();
    canvas.resize(1280, 250); paint(canvas);
    check("resize_preserves_source_samples", canvas.series()[0].points.constData() == sourcePointer && canvas.series()[0].points == raw[0].points);
    canvas.setCursor(.45); canvas.setChannelVisible(1, false); canvas.setReferenceLines({{.5, 1}}); paint(canvas);
    const auto selected = canvas.cursorX();
    canvas.updateSeries(signal(12500, .1)); paint(canvas);
    check("fast_update_keeps_cursor_visibility_and_window", canvas.cursorX() == selected && !canvas.series()[1].visible &&
        canvas.explicitXRange() && canvas.explicitXRange()->x() == 0 && canvas.explicitXRange()->y() == 1);
    const auto prefix = canvas.series()[0].points;
    check("bounded_append_accepts_valid_series", canvas.appendPoints(0, {{1.1, 9}, {1.2, -8}}, 100));
    check("bounded_append_discards_oldest_only", canvas.series()[0].points.size() == 100 && canvas.series()[0].points.last() == QPointF(1.2, -8) &&
        canvas.series()[0].points[97] == prefix.last());
    check("append_rejects_invalid_index_or_budget", !canvas.appendPoints(-1, {{2, 1}}, 10) && !canvas.appendPoints(0, {{2, 1}}, 1));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    canvas.updateSeries({{"gap", colors[0], {{0, 0}, {.2, .2}, {.3, nan}, {.7, nan}, {.8, -.2}, {1, 0}}}}, false); paint(canvas);
    int gapCount = 0; for (const auto& p : canvas.displayPoints(0)) if (!std::isfinite(p.y())) ++gapCount;
    check("nan_missing_runs_remain_disconnected", gapCount > 0);
    if (!output.isEmpty()) canvas.grab().save(output + "/gap.png");
    auto fragmented = signal(25000)[0];
    fragmented.points[10000].setY(nan); fragmented.points[10001].setY(nan);
    fragmented.points[10002].setY(1.25); fragmented.points[10003].setY(nan);
    canvas.updateSeries({fragmented}, false); paint(canvas);
    const auto pieces = canvas.displayPoints(0); int breaks = 0;
    for (const auto& point : pieces) if (!std::isfinite(point.y())) ++breaks;
    check("subpixel_gaps_and_isolated_valid_sample_survive_reduction", breaks >= 2 && contains(pieces, fragmented.points[10002]));
    auto unordered = signal(25000)[0]; std::swap(unordered.points[500], unordered.points[1500]);
    canvas.updateSeries({unordered}, false); paint(canvas);
    check("unordered_xy_falls_back_to_original_path", canvas.displayPoints(0) == unordered.points);
    QJsonArray labels, axisM4Calls; bool axisFit = true, zeroLabels = true, sourceUnchanged = true;
    int axisCase = 0;
    for (const double scale : {1.0, 1e-15, 1e100, -1e100}) {
        const auto points = scale > 0 ? QVector<QPointF>{{0, -scale}, {.5, scale * 6.78e-17}, {1, scale}} :
            QVector<QPointF>{{0, scale * 3}, {.5, scale * 2}, {1, scale}};
        canvas.updateSeries({{"axis", colors[1], points}}, false); canvas.setCursor({}); canvas.setReferenceLines({}); paint(canvas);
        QFont axisFont = canvas.font(); axisFont.setPixelSize(11); const QFontMetrics metrics(axisFont);
        for (const auto& text : canvas.yTickLabels()) axisFit = axisFit && metrics.horizontalAdvance(text) <= canvas.plotArea().left() - 11 + .01;
        if (scale > 0) zeroLabels = zeroLabels && canvas.yTickLabels().value(1) == QStringLiteral("0");
        sourceUnchanged = sourceUnchanged && canvas.series()[0].points == points;
        labels.push_back(QJsonArray::fromStringList(canvas.yTickLabels()));
        if (qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE")) axisM4Calls.push_back(int(canvas.renderStats().lastEnvelopeBuilds));
        if (!output.isEmpty()) canvas.grab().save(output + QStringLiteral("/axis-case-%1.png").arg(axisCase++));
    }
    check("negative_tiny_huge_axis_labels_fit_font_metrics", axisFit);
    check("near_zero_axis_residue_shows_zero_without_changing_samples", zeroLabels && sourceUnchanged);
    report.insert("axis_labels_by_case", labels);
    if (qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE")) report.insert("axis_m4_calls_by_case", axisM4Calls);
    auto stable = signal(12500); canvas.setChart({}, QStringLiteral("s"), QStringLiteral("g"), stable); canvas.setXRange(0, 1); paint(canvas);
    const auto uncachedImage = canvas.grab().toImage(); const auto cacheBefore = canvas.renderStats();
    canvas.updateSeries(stable, false); paint(canvas);
    check("unchanged_axis_render_is_pixel_equivalent_to_first_render", canvas.grab().toImage() == uncachedImage);
    const bool layerMode = canvas.axesMode() == "layer_cache" || canvas.axesMode() == "both";
    check("axis_layer_cache_is_used_only_in_requested_modes", layerMode ?
        canvas.renderStats().axesBuilds == cacheBefore.axesBuilds && canvas.renderStats().axesCacheHits > cacheBefore.axesCacheHits :
        canvas.renderStats().axesBuilds > cacheBefore.axesBuilds && canvas.renderStats().axesCacheHits == cacheBefore.axesCacheHits);
    auto changedAxis = stable; changedAxis[0].points[6000].setY(90);
    const auto labelsBefore = canvas.yTickLabels(); canvas.updateSeries(changedAxis, false); paint(canvas);
    check("new_raw_peak_invalidates_y_axis_cache_immediately", canvas.renderStats().axesBuilds > cacheBefore.axesBuilds && canvas.yTickLabels() != labelsBefore && contains(canvas.displayPoints(0), changedAxis[0].points[6000]));
    auto wideAxis = stable; for (auto& line : wideAxis) for (auto& point : line.points) point.setY(point.y() * 1e100);
    canvas.setChart({}, QStringLiteral("s"), QStringLiteral("g"), wideAxis); paint(canvas);
    const auto stableGutter = canvas.plotArea().left(); const auto reductionsBefore = canvas.renderStats().reductions;
    canvas.updateSeries(wideAxis, false); paint(canvas);
    check("wide_tick_gutter_is_stable_and_reduces_raw_data_once", canvas.plotArea().left() == stableGutter && canvas.renderStats().reductions == reductionsBefore + 1);
    canvas.setChart({}, QStringLiteral("s"), QStringLiteral("g"), stable); paint(canvas);
    auto ranged = signal(25000), trimmed = ranged;
    for (int channel = 0; channel < ranged.size(); ++channel) {
        ranged[channel].points[100].setY(1e9); ranged[channel].points[24800].setY(-1e9);
        trimmed[channel].points.clear();
        for (const auto& point : ranged[channel].points) if (point.x() >= .25 && point.x() <= .75) trimmed[channel].points.push_back(point);
    }
    canvas.setXRange(.25, .75); canvas.updateSeries(ranged, false); paint(canvas);
    const auto rangeLabels = canvas.yTickLabels(); const auto rangeArea = canvas.plotArea(); bool rangeExtrema = true;
    for (int channel = 0; channel < ranged.size(); ++channel) {
        const auto& expected = trimmed[channel].points; const auto shown = canvas.displayPoints(channel);
        const auto bounds = std::minmax_element(expected.begin(), expected.end(), [](auto a, auto b) { return a.y() < b.y(); });
        rangeExtrema = rangeExtrema && contains(shown, *bounds.first) && contains(shown, *bounds.second);
    }
    if (!output.isEmpty()) canvas.grab().save(output + "/explicit-range-full-source.png");
    canvas.updateSeries(trimmed, false); paint(canvas);
    check("explicit_range_bounds_equal_independently_trimmed_raw_input", canvas.yTickLabels() == rangeLabels && canvas.plotArea() == rangeArea);
    check("explicit_range_keeps_all_visible_extrema_and_excludes_outside_spikes", rangeExtrema && !rangeLabels.join(' ').contains("e+09"));
    if (!output.isEmpty()) canvas.grab().save(output + "/explicit-range-trimmed-source.png");
    canvas.setXRange(0, 1);
    canvas.updateSeries(signal(12500), false); canvas.resize(1024, 220); paint(canvas);
    if (!output.isEmpty()) canvas.grab().save(output + "/three-channel-1024.png");
    canvas.resize(724, 172); paint(canvas);
    if (!output.isEmpty()) canvas.grab().save(output + "/three-channel-724.png");
    const auto visibleImage = canvas.grab().toImage(); canvas.hide();
    check("explicit_hidden_widget_render_retains_complete_chart", canvas.grab().toImage() == visibleImage);
    if (qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE")) {
        const auto stats = canvas.renderStats();
        check("profile_parts_partition_static_render_without_double_counting", stats.lastBoundsNs + stats.lastEnvelopeNs + stats.lastMapNs + stats.lastRasterNs + stats.lastAxesNs + stats.lastOtherNs <= stats.lastStaticNs + 2000);
    }
    report.insert("curve_logical_width", canvas.traceWidth());
    const auto equivalence = compareDirectPaint(output); report["direct_paint_pixel_equivalence"] = equivalence;
    check("direct_and_cached_pixels_within_three_levels_without_screenshot_repaint", equivalence.value("passed").toBool());
    report["selected_canvas_shared_M4_profile"] = canvas.sharedM4Profile();
    const auto shared = testSharedM4(); report["shared_M4_cache_regressions"] = shared;
    check("shared_M4_cache_all_regressions", shared.value("passed").toBool());
#endif
    report.insert("device_pixel_ratio", canvas.devicePixelRatioF());
    report.insert("measurement_scope", "Linux x86_64 host Qt raster rendering, three display series; no sensor, ARM, physical refresh, FFT or acquisition benchmark");
    report.insert("includes", "series adoption, min/max scan, path rasterization, image presentation and pending GUI events; excludes signal generation and file I/O");
    report.insert("passed", passed);
    const auto json = QJsonDocument(report).toJson();
    if (!output.isEmpty()) { QFile file(output + "/plot-render.json"); if (file.open(QIODevice::WriteOnly)) file.write(json); }
    std::fwrite(json.constData(), 1, std::size_t(json.size()), stdout); return passed ? 0 : 1;
}
