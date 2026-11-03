#include "analysis_pane.hpp"
#include "controls.hpp"
#include "plot_canvas.hpp"
#include "test_fonts.hpp"
#include "waveform_stack.hpp"
#include "window.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

using vibration::workbench::PlotCanvas;
using vibration::workbench::AnalysisPane;
using vibration::workbench::NumberField;
using vibration::workbench::WaveformStack;
namespace {
template<class T> T* child(QWidget& parent, const char* name) {
    // PlotCanvas/NumberField deliberately have no Q_OBJECT of their own.
    // Searching the inherited meta-object by class would match base widgets.
    auto* out = dynamic_cast<T*>(parent.findChild<QWidget*>(QString::fromLatin1(name)));
    if (!out) throw std::runtime_error(std::string("Missing control: ") + name);
    return out;
}
void mouse(QWidget& widget, QEvent::Type type, const QPointF& point,
           Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}
void paint(QWidget& widget) { QApplication::processEvents(); widget.grab(); }
bool near(double a, double b, double tolerance = 1e-9) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}
bool rangeIs(const PlotCanvas& plot, double start, double end) {
    const auto range = plot.explicitXRange();
    return range && near(range->x(), start) && near(range->y(), end);
}
bool hasSeries(const PlotCanvas& plot, const QString& name) {
    return std::any_of(plot.series().begin(), plot.series().end(), [&](const auto& series) {
        return series.name == name && series.visible && !series.points.empty();
    });
}
int pixelsOf(const QImage& image, const QColor& color) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
        if (image.pixelColor(x, y) == color) ++count;
    return count;
}
int neutralPixels(const QImage& image, QRect area, int minimum, int maximum, int tolerance) {
    int count = 0; area = area.intersected(image.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) for (int x = area.left(); x <= area.right(); ++x) {
        const auto color = image.pixelColor(x, y);
        if (color.red() >= minimum && color.red() <= maximum && std::abs(color.red() - color.green()) <= tolerance &&
            std::abs(color.green() - color.blue()) <= tolerance) ++count;
    }
    return count;
}
QPushButton* namedAction(QDialog& dialog, const QString& title) {
    for (auto* button : dialog.findChildren<QPushButton*>()) if (button->text() == title) return button;
    return nullptr;
}
bool numberPad(NumberField& field, const QString& value, bool confirm) {
    bool opened = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != QStringLiteral("workbenchNumberPad")) return;
        auto* entry = dialog->findChild<QLineEdit*>(QStringLiteral("numberPadEntry"));
        auto* action = namedAction(*dialog, confirm ? QStringLiteral("确定") : QStringLiteral("取消"));
        opened = entry && action;
        if (!opened) { dialog->reject(); return; }
        entry->setText(value); action->click();
        // An unexpected validation rejection must fail the probe, not hang it.
        if (dialog->isVisible()) { opened = false; dialog->reject(); }
    });
    auto* editor = field.findChild<QLineEdit*>();
    if (!editor) return false;
    mouse(*editor, QEvent::MouseButtonPress, editor->rect().center(), Qt::LeftButton, Qt::LeftButton);
    QApplication::processEvents();
    return opened;
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    vibration::workbench::qa::configureFonts(application);
    application.setStyleSheet(vibration::workbench::stylesheet());
    QJsonObject report{{"font_audit", vibration::workbench::qa::fontAudit(application.font())}}; bool passed = true;
    const auto check = [&](const char* name, bool value) { report.insert(QString::fromLatin1(name), value); passed = passed && value; };
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: analysis_probe CAPDDR2_FILE [EVIDENCE_DIRECTORY]\n"); return 2;
    }
    const QString evidence = argc == 3 ? QString::fromLocal8Bit(argv[2]) : QString();
    if (!evidence.isEmpty() && !QDir().mkpath(evidence)) return 2;
    try {
        auto capture = std::make_shared<vibration::Capture>(vibration::load_capture(argv[1]));
        vibration::AnalysisConfig config; config.spectrum_max_hz = 400;
        auto result = std::make_unique<vibration::AnalysisResult>(vibration::analyze(*capture, config));
        const double duration = result->duration_s;
        if (!(duration > 0) || capture->records.empty()) throw std::runtime_error("The fixture must contain a valid nonempty record");
        AnalysisPane pane; pane.setContentsMargins(20, 4, 20, 0); pane.show();
        int statusMessages = 0; pane.statusChanged = [&](const QString&) { ++statusMessages; };
        const std::array<const char*, 4> names{"workbenchWaveCH1", "workbenchWaveCH2", "workbenchWaveCH3", "workbenchWaveOverlay"};
        std::array<PlotCanvas*, 4> plots{};
        for (int i = 0; i < 4; ++i) plots[i] = child<PlotCanvas>(pane, names[i]);
        auto* stack = child<WaveformStack>(pane, "waveformStack");
        auto* speedRow = stack->plot(4); if(!speedRow)throw std::runtime_error("Missing stable fifth time-domain speed row");
        check("default_order_includes_independent_fifth_speed_row",stack->panelOrder()==QVector<int>({0,1,2,3,4})&&stack->panelChannels(4).empty());
        auto* start = child<NumberField>(pane, "waveRangeStart");
        auto* end = child<NumberField>(pane, "waveRangeEnd");
        check("empty_fields_and_range_actions_disabled", !start->isEnabled() && !end->isEnabled() &&
              !child<QPushButton>(pane, "waveRangeApply")->isEnabled() && !child<QPushButton>(pane, "waveFit")->isEnabled());
        pane.setResult(capture, result.get());
        // Window subtracts its 54px rail, 206px record context, 64px header,
        // 53px view tabs and 28px status before presenting this surface.
        for (const auto& size : {QSize(764, 455), QSize(1020, 575)}) {
            pane.resize(size); pane.setView(0); paint(pane);
            bool geometry = pane.size() == size;
            std::array<QRect, 4> bounds{};
            for (int i = 0; i < 4; ++i) {
                bounds[i] = QRect(plots[i]->mapTo(stack->viewport(), QPoint()), plots[i]->size());
                geometry = geometry && plots[i]->isVisible() && plots[i]->width() >= stack->viewport()->width() - 28 &&
                    plots[i]->height() >= 110 && plots[i]->height() <= 240 && plots[i]->xAxisVisible();
            }
            for (int a = 0; a < 4; ++a) for (int b = a + 1; b < 4; ++b) geometry = geometry && !bounds[a].intersects(bounds[b]);
            for (int i = 1; i < 4; ++i) geometry = geometry && bounds[i].top() > bounds[i - 1].bottom();
            const auto speedBounds=QRect(speedRow->mapTo(stack->viewport(),QPoint()),speedRow->size());
            geometry=geometry&&speedRow->isVisible()&&speedRow->width()>=stack->viewport()->width()-28&&speedRow->height()>=110&&speedRow->height()<=240&&speedRow->xAxisVisible()&&speedBounds.top()>bounds[3].bottom();
            for(const auto& waveBounds:bounds)geometry=geometry&&!speedBounds.intersects(waveBounds);
            geometry = geometry && stack->verticalScrollBar()->maximum() > 0;
            check(size.width() == 764 ? "five_full_width_rows_scroll_at_1024x600" : "five_full_width_rows_scroll_at_1280x720", geometry);
            stack->verticalScrollBar()->setValue(0); paint(pane);
            if (!evidence.isEmpty()) pane.grab().save(evidence + QStringLiteral("/wave-top-%1x%2.png").arg(size.width()).arg(size.height()));
            stack->verticalScrollBar()->setValue(stack->verticalScrollBar()->maximum()); paint(pane);
            const auto lastBounds = QRect(speedRow->mapTo(stack->viewport(), QPoint()), speedRow->size());
            check(size.width() == 764 ? "last_row_reachable_at_1024x600" : "last_row_reachable_at_1280x720", stack->viewport()->rect().contains(lastBounds.center()));
            if (!evidence.isEmpty()) pane.grab().save(evidence + QStringLiteral("/wave-bottom-%1x%2.png").arg(size.width()).arg(size.height()));
            stack->verticalScrollBar()->setValue(0); paint(pane);
        }
        pane.resize(764, 455); paint(pane);
        const auto canvasImage = plots[0]->grab().toImage();
        const auto axisArea = QRect(0, 0, int(plots[0]->plotArea().left()) - 3, canvasImage.height());
        const int gridPixels = neutralPixels(canvasImage, plots[0]->plotArea().toAlignedRect(), 237, 253, 0);
        const int axisPixels = neutralPixels(canvasImage, axisArea, 80, 225, 3);
        report.insert(QStringLiteral("neutral_grid_pixels"), gridPixels); report.insert(QStringLiteral("neutral_axis_pixels"), axisPixels);
        check("new_canvas_neutral_grid_and_axis_ink", gridPixels > 100 && axisPixels > 10 && pixelsOf(canvasImage, QColor("#e9eef4")) == 0 &&
              pixelsOf(canvasImage, QColor("#78899c")) == 0);
        check("range_fields_are_new_touch_number_fields", start->height() >= 44 && end->height() >= 44 && start->decimals() == 3);
        check("initial_range_is_first_250ms_or_short_record", std::all_of(plots.begin(), plots.end(), [&](auto* plot) { return rangeIs(*plot, 0, std::min(.25, duration)); }));
        check("fifth_speed_row_uses_linked_initial_time_window",rangeIs(*speedRow,0,std::min(.25,duration)));

        const double first = duration * .1, last = duration * .2;
        pane.setWindow(first, last); paint(pane);
        bool rawRange = std::all_of(plots.begin(), plots.end(), [&](auto* plot) { return rangeIs(*plot, first, last); });
        const double multiplier = (double(capture->header.adc_range_uv) / 1e6 / 32768.0) /
                                  (result->config.analog_gain * result->config.sensor_sensitivity_v_per_g);
        for (int channel = 0; channel < 3; ++channel) {
            const auto& series = plots[channel]->series(); rawRange = rawRange && series.size() == 1 && !series[0].points.empty();
            if (series.empty()) continue;
            for (const auto& point : series[0].points) {
                const auto index = std::size_t(std::llround(point.x() * capture->header.sample_rate_hz));
                rawRange = rawRange && index < capture->records.size() && point.x() >= first - 1e-9 && point.x() <= last + 1e-9;
                if (index < capture->records.size()) rawRange = rawRange && near(point.y(), capture->records[index].raw[channel] * multiplier);
            }
            rawRange = rawRange && series[0].points.constData() == plots[3]->series()[channel].points.constData();
        }
        check("selected_range_uses_original_samples_and_shared_buffers", rawRange);
        const auto cachedPoints = plots[0]->series()[0].points;
        const auto beforeIdentical = plots[0]->renderStats();
        pane.setWindow(first, last); paint(pane);
        const auto afterIdentical = plots[0]->renderStats();
        check("identical_range_reuses_chart_data_cache", (plots[0]->directPaintEnabled() ?
              afterIdentical.reductions == beforeIdentical.reductions && afterIdentical.cachedImageBlits == 0 :
              afterIdentical.staticBuilds == beforeIdentical.staticBuilds) &&
              plots[0]->series()[0].points.constData() == cachedPoints.constData());
        pane.setWindow(duration * .3, duration * .4); pane.setWindow(first, last); paint(pane);
        check("range_revisit_reuses_preview_buffers", plots[0]->series()[0].points.constData() == cachedPoints.constData());
        pane.setWindow(-duration, duration * 2); paint(pane);
        check("range_clamped_to_record", rangeIs(*plots[0], 0, duration));
        const int beforeInvalid = statusMessages;
        pane.setWindow(last, first); pane.setWindow(std::numeric_limits<double>::quiet_NaN(), last);
        check("invalid_ranges_preserve_applied_window", rangeIs(*plots[0], 0, duration) && statusMessages == beforeInvalid + 2);

        std::array<PlotCanvas::RenderStats, 4> cursorStats{};
        std::array<QVector<quintptr>, 4> cursorBuffers;
        // The scroll viewport deliberately leaves some rows unpainted. Warm
        // every canvas before measuring cursor-only cache behavior.
        for (auto* plot : plots) plot->grab();
        for (int i = 0; i < 4; ++i) {
            cursorStats[i] = plots[i]->renderStats();
            for (const auto& series : plots[i]->series()) cursorBuffers[i].push_back(reinterpret_cast<quintptr>(series.points.constData()));
        }
        const auto area = plots[0]->plotArea();
        // Dragging now pans the viewport; a stationary click still selects
        // the original-sample cursor across all four linked time plots.
        mouse(*plots[0], QEvent::MouseButtonPress, {area.left() + area.width() * .8, area.center().y()}, Qt::LeftButton, Qt::LeftButton);
        mouse(*plots[0], QEvent::MouseButtonRelease, {area.left() + area.width() * .8, area.center().y()}, Qt::LeftButton, Qt::NoButton);
        paint(pane);
        const auto cursor = plots[0]->cursorX();
        bool linked = cursor.has_value(), warm = true;
        for (int i = 0; i < 4; ++i) {
            linked = linked && plots[i]->cursorX() && near(*plots[i]->cursorX(), cursor.value_or(-1));
            const auto stats = plots[i]->renderStats();
            QVector<quintptr> buffers; for (const auto& series : plots[i]->series()) buffers.push_back(reinterpret_cast<quintptr>(series.points.constData()));
            warm = warm && (plots[i]->directPaintEnabled() ? stats.reductions == cursorStats[i].reductions && stats.cachedImageBlits == 0 :
                stats.staticBuilds == cursorStats[i].staticBuilds) && buffers == cursorBuffers[i];
        }
        check("pointer_click_links_all_four_cursors", linked && child<QLabel>(pane, "waveCursorReadout")->text().contains(QStringLiteral("CH3")));
        check("linked_cursor_preserves_plot_data_cache", warm);
        const double preciseTime = duration * .1234567;
        plots[0]->selectAtX(preciseTime);
        const double expectedTime = std::llround(preciseTime * capture->header.sample_rate_hz) / double(capture->header.sample_rate_hz);
        check("cursor_preserves_raw_sample_time_beyond_field_precision", plots[0]->cursorX() && near(*plots[0]->cursorX(), expectedTime));
        const auto untouchedPoints = plots[3]->series()[0].points;
        const bool reordered = stack->movePanel(2, 0);
        const auto reorderedIds = stack->panelOrder();
        check("reorder_preserves_stable_plot_identity", reordered && reorderedIds.size() == 5 && reorderedIds.first() == 2 &&
              stack->plot(0) == plots[0] && stack->plot(1) == plots[1] && stack->plot(2) == plots[2] && stack->plot(3) == plots[3]&&stack->plot(4)==speedRow);
        const bool mergedCh1 = stack->moveChannel(0, 1, 0); paint(pane);
        check("merge_ch1_hides_empty_source_row", mergedCh1 && !stack->panelOrder().contains(0) && !plots[0]->isVisible() &&
              stack->panelChannels(0).empty() && hasSeries(*plots[1], QStringLiteral("CH1")) && hasSeries(*plots[1], QStringLiteral("CH2")) &&
              plots[3]->series()[0].points.constData() == untouchedPoints.constData());
        const bool mergedCh3 = stack->moveChannel(2, 1, 2); paint(pane);
        check("merge_ch3_hides_its_empty_source_row", mergedCh3 && !stack->panelOrder().contains(2) && !plots[2]->isVisible() &&
              stack->panelChannels(1) == QVector<int>({0, 1, 2}) && hasSeries(*plots[1], QStringLiteral("CH3")) && stack->panelOrder().size() == 3&&stack->panelOrder().contains(4));
        const bool deduplicated = stack->moveChannel(1, 3, 0); paint(pane);
        check("merge_existing_channel_removes_source_duplicate", deduplicated && !stack->panelChannels(1).contains(0) &&
              stack->panelChannels(3) == QVector<int>({0, 1, 2}) && plots[3]->series().size() == 3 && stack->panelOrder().contains(1));
        const bool split = stack->splitChannel(1, 2); paint(pane);
        const auto activeIds = stack->panelOrder(); int splitId = -1;
        for (const int id : activeIds) if (id != 1 && id != 3 && stack->panelChannels(id) == QVector<int>({2})) splitId = id;
        check("split_channel_restores_an_independent_row", split && activeIds.size() == 4 && splitId >= 0 &&
              stack->plot(splitId)->isVisible() && hasSeries(*stack->plot(splitId), QStringLiteral("CH3")) &&
              stack->panelChannels(1) == QVector<int>({1}));
        const auto selectedChannels = stack->panelChannels(1);
        pane.setResult(capture, result.get()); paint(pane);
        check("record_refresh_preserves_user_order_and_channel_groups", stack->panelOrder() == activeIds && stack->panelChannels(1) == selectedChannels &&
              hasSeries(*plots[1], QStringLiteral("CH2")) && !hasSeries(*plots[1], QStringLiteral("CH1")) && stack->plot(0) == plots[0] &&
              (splitId < 0 || stack->panelChannels(splitId) == QVector<int>({2})));
        pane.setWindow(0, duration);
        child<QPushButton>(pane, "waveZoomIn")->click(); paint(pane);
        const auto zoomed = plots[0]->explicitXRange();
        check("zoom_in_halves_visible_duration", zoomed && near(zoomed->y() - zoomed->x(), duration * .5));
        child<QPushButton>(pane, "waveZoomOut")->click(); child<QPushButton>(pane, "waveFit")->click(); paint(pane);
        check("fit_restores_complete_record", std::all_of(plots.begin(), plots.end(), [&](auto* plot) { return rangeIs(*plot, 0, duration); }));
        child<QPushButton>(pane, "waveReset")->click(); paint(pane);
        check("reset_restores_default_window_not_full_record", std::all_of(plots.begin(), plots.end(), [&](auto* plot) { return rangeIs(*plot, 0, std::min(.25, duration)); }));
        check("reset_restores_default_order_and_channel_groups", stack->panelOrder() == QVector<int>({0, 1, 2, 3,4}) &&
              stack->panelChannels(0) == QVector<int>({0}) && stack->panelChannels(1) == QVector<int>({1}) &&
              stack->panelChannels(2) == QVector<int>({2}) && stack->panelChannels(3) == QVector<int>({0, 1, 2}));
        child<QPushButton>(pane, "waveFit")->click();

        const double oldStart = start->value();
        check("number_pad_cancel_preserves_field", numberPad(*start, QStringLiteral("0"), false) && near(start->value(), oldStart));
        const double editedStart = std::floor(duration * .125 * 1e3) / 1e3;
        check("number_pad_confirm_updates_field", numberPad(*start, QString::number(editedStart, 'f', 3), true) && near(start->value(), editedStart, 1e-6));
        const double editedEnd = end->value();
        child<QPushButton>(pane, "waveRangeApply")->click(); paint(pane);
        check("number_pad_applies_selected_range", rangeIs(*plots[0], editedStart, editedEnd));
        pane.setView(1); paint(pane);
        auto* spectrum = child<PlotCanvas>(pane, "workbenchSpectrum");
        check("spectrum_uses_full_content_width", spectrum->isVisible() && spectrum->width() >= pane.contentsRect().width() - 4 && spectrum->series().size() == 3);
        spectrum->selectAtX(result->spectra[0].frequency_hz.empty() ? 0 : result->spectra[0].frequency_hz.back() * .25); paint(pane);
        check("spectrum_cursor_readout", spectrum->cursorX().has_value() && child<QLabel>(pane, "spectrumCursorReadout")->text().contains(QStringLiteral("CH1")));
        if (!evidence.isEmpty()) pane.grab().save(evidence + QStringLiteral("/spectrum-1024-content.png"));
        pane.setView(2); paint(pane); auto* speed = child<PlotCanvas>(pane, "workbenchSpeed");
        check("speed_uses_full_content_width", speed->isVisible() && speed->width() >= pane.contentsRect().width() - 4 && rangeIs(*speed, 0, duration));
        if (!evidence.isEmpty()) pane.grab().save(evidence + QStringLiteral("/speed-1024-content.png"));

        std::weak_ptr<const vibration::Capture> lifetime = capture; capture.reset();
        check("pane_keeps_capture_alive_for_range_previews", !lifetime.expired());
        pane.clear(); result.reset();
        pane.setWindow(0, 1); pane.setView(0); child<QPushButton>(pane, "waveFit")->click(); paint(pane);
        bool disabled = lifetime.expired() && !start->isEnabled() && !end->isEnabled() && !stack->isEnabled();
        for (const auto* name : {"waveRangeApply", "waveZoomIn", "waveZoomOut", "waveReset", "waveFit"}) disabled = disabled && !child<QPushButton>(pane, name)->isEnabled();
        for (int axis = 1; axis <= 3; ++axis) disabled = disabled && !child<QCheckBox>(pane, QStringLiteral("spectrumChannel%1").arg(axis).toLatin1().constData())->isEnabled();
        check("clear_releases_capture_and_disables_controls", disabled);
        check("cleared_plots_drop_all_data_before_owner_destruction", std::all_of(plots.begin(), plots.end(), [](auto* plot) { return plot->series().empty(); })&&std::all_of(speedRow->series().begin(),speedRow->series().end(),[](const auto& series){return series.points.empty();}));
        if (!evidence.isEmpty()) pane.grab().save(evidence + QStringLiteral("/empty-1024-content.png"));
        if (!evidence.isEmpty()) {
            // Evidence uses the real new application shell, with a file-only
            // analysis. It does not invent a frame around isolated panes.
            pane.hide(); vibration::workbench::Window window; window.show();
            window.openCapture(QString::fromUtf8(argv[1])); QElapsedTimer deadline; deadline.start();
            while (window.isAnalyzing() && deadline.elapsed() < 30000) { QApplication::processEvents(); QThread::msleep(5); }
            auto* actualStack = child<WaveformStack>(window, "waveformStack");
            auto* actualAnalysis = child<AnalysisPane>(window, "analysisPane");
            const bool loaded = !window.isAnalyzing() && hasSeries(*actualStack->plot(0), QStringLiteral("CH1"));
            check("actual_new_window_loads_file_fixture", loaded);
            if (loaded) for (const auto& size : {QSize(1024, 600), QSize(1280, 720)}) {
                window.resize(size); window.setView(0); actualStack->resetLayout(); actualAnalysis->setWindow(0, std::min(.25, duration));
                actualStack->verticalScrollBar()->setValue(0); paint(window);
                const auto suffix = QStringLiteral("-%1x%2.png").arg(size.width()).arg(size.height());
                bool saved = window.grab().save(evidence + QStringLiteral("/window-default") + suffix);
                const bool merged = actualStack->moveChannel(0, 1, 0) && actualStack->moveChannel(2, 1, 2);
                actualStack->verticalScrollBar()->setValue(0); paint(window);
                saved = window.grab().save(evidence + QStringLiteral("/window-merged-ch1-ch2-ch3") + suffix) && saved;
                const bool split = actualStack->splitChannel(1, 2); actualStack->verticalScrollBar()->setValue(0); paint(window);
                saved = window.grab().save(evidence + QStringLiteral("/window-split-ch3") + suffix) && saved;
                check(size.width() == 1024 ? "actual_window_default_merge_split_1024_png" : "actual_window_default_merge_split_1280_png",
                      merged && split && saved && actualStack->panelOrder().size() == 4);
            }
        }
        report.insert(QStringLiteral("font_family"), QFontInfo(application.font()).family());
        report.insert(QStringLiteral("screen_1024_content"), QStringLiteral("764x455 outer; 724x451 after the new shell's pane margins"));
        report.insert(QStringLiteral("screen_1280_content"), QStringLiteral("1020x575 outer; 980x571 after the new shell's pane margins"));
        report.insert(QStringLiteral("measurement_scope"), QStringLiteral("Qt5 host software rendering with a file fixture; no physical display, sensor or hardware acquisition acceptance"));
    } catch (const std::exception& error) { report.insert(QStringLiteral("error"), QString::fromUtf8(error.what())); passed = false; }
    report.insert(QStringLiteral("passed"), passed); const auto json = QJsonDocument(report).toJson();
    if (!evidence.isEmpty()) { QFile file(evidence + QStringLiteral("/analysis-probe.json")); if (file.open(QIODevice::WriteOnly)) file.write(json); }
    std::fwrite(json.constData(), 1, std::size_t(json.size()), stdout); return passed ? 0 : 1;
}

