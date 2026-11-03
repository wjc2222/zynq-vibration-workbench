#include "analysis_pane.hpp"
#include "controls.hpp"
#include "plot_canvas.hpp"
#include "single_waveform_view.hpp"
#include "live_preview_model.hpp"
#include "live_render_budget.hpp"
#include "timing_stats.hpp"
#include "../gui/view_math.hpp"

#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStringList>
#include <QVBoxLayout>
#include <QTimer>
#include <QVariant>
#include <QShowEvent>
#include <QHideEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace vibration::workbench {
namespace {
const std::array<QColor, 3> channelColors{QColor("#3769c7"), QColor("#268878"), QColor("#b78032")};
const std::array<QString, 3> channelNames{QStringLiteral("CH1"), QStringLiteral("CH2"), QStringLiteral("CH3")};

QString numeric(double value, int precision = 4) {
    return std::isfinite(value) ? QString::number(value, 'g', precision) : QStringLiteral("—");
}
QString optionalValue(const std::optional<double>& value, int precision = 4) {
    return value ? numeric(*value, precision) : QStringLiteral("—");
}
QString amplitudeUnit(const AnalysisConfig& config) {
    return QString::fromStdString(amplitude_unit_symbol(config));
}
double spectrumScale(std::size_t channel, const AnalysisConfig& config) {
    if (config.amplitude_unit == AmplitudeUnit::Voltage) return 1;
    const auto p = effective_channel_calibration(config, channel);
    const auto scale = 1000.0 / (p.analog_gain * p.sensitivity_mv_per_m_s2);
    return std::abs(config.amplitude_unit == AmplitudeUnit::Gravity ? scale / standard_gravity_m_s2 : scale);
}
QLabel* label(const QString& value, const char* role = nullptr) {
    auto* out = new QLabel(value);
    out->setTextFormat(Qt::PlainText);
    if (role) out->setProperty("role", role);
    out->setMinimumWidth(0);
    return out;
}
QPushButton* button(const QString& value, const char* name, int width) {
    auto* out = new QPushButton(value);
    out->setObjectName(QString::fromLatin1(name));
    // QSS min-height is the content box; reserve both one-pixel borders.
    out->setFixedSize(width, 46);
    return out;
}
void fullWidthLabel(QLabel* value) {
    value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    value->setMinimumWidth(0);
}
}

struct AnalysisPane::Impl {
    explicit Impl(AnalysisPane* owner) : q(owner) {}
    AnalysisPane* q;
    std::shared_ptr<const Capture> capture;
    const AnalysisResult* result = nullptr;
    QStackedWidget* views = nullptr;
    PlotCanvas *wave = nullptr, *spectrum = nullptr, *speed = nullptr;
    SingleWaveformView *singleWave = nullptr, *singleSpectrum = nullptr;
    QLabel *waveCursor = nullptr, *spectrumCursor = nullptr;
    QLabel *spectrumDetail = nullptr, *speedNote = nullptr;
    QLabel *navigationNote = nullptr;
    std::array<QLabel*, 4> speedValues{}, speedTitles{};
    double windowStart = 0, windowEnd = 1;
    std::optional<double> cursor;
    struct Preview { double start, end; QVector<PlotSeries> series; };
    std::vector<Preview> previews;
    std::unique_ptr<LivePreviewModel> live = std::make_unique<LivePreviewModel>();
    LivePreviewView liveView;
    LivePreviewSpectrum liveSpectrum;
    AnalysisConfig liveConfig;
    std::optional<bool> accelerationMode;
    bool orderMarkersVisible = false;
    const AnalysisConfig& waveConfig() const {
        return accelerationMode.has_value() || !result ? liveConfig : result->config;
    }
    AnalysisConfig liveSpectrumConfig() const {
        auto config = liveConfig;
        // Window's acceleration page changes only the time-domain display.
        // A standalone pane retains its configured SI/g spectrum behavior.
        if (accelerationMode.has_value()) config.amplitude_unit = AmplitudeUnit::Voltage;
        return config;
    }
    double displayValue(double value, std::size_t channel) const {
        if(!result || !std::isfinite(value)) return value;
        const auto& source=result->config;const auto& target=waveConfig();
        const double a=voltage_to_amplitude(0,channel,source);
        const double b=voltage_to_amplitude(1,channel,source)-a;
        return voltage_to_amplitude((value-a)/b,channel,target);
    }
    QTimer* liveTimer = nullptr;
    QWidget *liveTools = nullptr;
    QPushButton* liveToggle = nullptr;
    NumberField* liveSpan = nullptr;
    bool liveDisplay = false, following = true;
    quint64 liveDrawRevision = 0, liveSpectrumRevision = 0;
    quint64 liveDrawCount = 0;
    quint64 spectrumCopies = 0, spectrumBuilds = 0;
    const bool demandAnalysis = qgetenv("AX7_LIVE_DEMAND_ANALYSIS") == "1";
    const bool profiling = qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE") != 0;
    TimingSamples refreshTotal, pollTiming, propertyTiming, viewTiming, waveTiming, speedTiming, cursorTiming, spectrumTiming;
    int liveBatchDepth=0;
    quint64 liveBatches=0,renderBudgetDeferrals=0,lastBudgetBuild=0;
    PlotCanvas* budgetPlot=nullptr;
    qint64 lastRefreshWorkNs=0;
    QElapsedTimer lastLivePresentation;
    LiveRenderBudget renderBudget;

    void updateRenderBudget() {
        auto* plot=currentPlot();const auto stats=plot->renderStats();
        if(plot!=budgetPlot){budgetPlot=plot;lastBudgetBuild=0;renderBudget.reset();}
        if(stats.staticBuilds!=lastBudgetBuild){
            lastBudgetBuild=stats.staticBuilds;
            renderBudget.observe(lastRefreshWorkNs,stats.lastPaintNs);
        }
        const int interval=renderBudget.intervalMs();
        if(liveTimer&&liveTimer->interval()!=interval)liveTimer->setInterval(interval);
    }

    void status(const QString& value) { if (q->statusChanged) q->statusChanged(value); }
    QVector<PlotSeries> amplitudeSeries() const {
        // LivePreviewModel already owns voltage samples. Keep Qt's shared
        // buffers intact when no conversion is requested.
        if (liveConfig.amplitude_unit == AmplitudeUnit::Voltage) return liveView.voltage;
        auto series = liveView.voltage;
        for (int channel=0;channel<series.size()&&channel<3;++channel)
            for (auto& point:series[channel].points)
                if(std::isfinite(point.y()))point.setY(voltage_to_amplitude(point.y(), std::size_t(channel), liveConfig));
        return series;
    }
    PlotCanvas* currentPlot() const { return views->currentIndex()==0 ? wave : views->currentIndex()==1 ? spectrum : speed; }
    void updateNavigation() {
        const int view=views->currentIndex();
        for(auto* plot:{wave,spectrum,speed})plot->setLiveNavigationState(liveDisplay&&live->isLive(),following);
        const auto domain=currentPlot()->navigationBounds();const auto range=currentPlot()->explicitXRange().value_or(domain);
        if(!result&&!liveDisplay){
            navigationNote->clear();
            navigationNote->setToolTip(QStringLiteral("开始采集或打开 BIN 记录后显示实际数据范围。"));
        }else{
            const auto scope=liveDisplay?(following?QStringLiteral("实时跟随"):QStringLiteral("显示已暂停")):
                view==1?QStringLiteral("当前频段"):QStringLiteral("当前窗口");
            auto text=QStringLiteral("%1 · %2–%3 %4").arg(scope,numeric(range.x(),5),numeric(range.y(),5),view==1?QStringLiteral("Hz"):QStringLiteral("s"));
            if(!liveDisplay&&result)text+=QStringLiteral(" · 全记录 %1 s").arg(numeric(result->duration_s,7));
            navigationNote->setText(text);
            navigationNote->setToolTip(QStringLiteral("可用范围 %1–%2；实时预览缓存有界。拖动/滚轮缩放，Shift框选，双击恢复，右键精确设置。").arg(numeric(domain.x(),7),numeric(domain.y(),7)));
        }
        QVector<QPointF> values;QString scope;
        if(liveDisplay&&liveView.voltage.size()==3&&singleWave->sourceSeries().size()==3) {
            QStringList counts;
            // These are the same converted samples adopted by the plot for
            // this revision; avoid recalibrating every value a second time.
            const auto& amplitudes=singleWave->sourceSeries();
            for(int ch=0;ch<3;++ch) {
                long double sumSquares=0;double low=std::numeric_limits<double>::infinity(),high=-low;quint64 count=0;
                for(const auto& point:amplitudes[ch].points)if(std::isfinite(point.x())&&std::isfinite(point.y())&&point.x()>=windowStart&&point.x()<=windowEnd) {
                    const double amplitude=point.y();
                    ++count;low=std::min(low,amplitude);high=std::max(high,amplitude);sumSquares+=static_cast<long double>(amplitude)*amplitude;
                }
                const double missing=std::numeric_limits<double>::quiet_NaN();values.push_back({count?std::sqrt(double(sumSquares/count)):missing,count?high-low:missing});counts<<QStringLiteral("CH%1 %2 点").arg(ch+1).arg(count);
            }
            scope=QStringLiteral("原始预览可见窗 %1–%2 s · %3").arg(numeric(windowStart,7),numeric(windowEnd,7),counts.join(QStringLiteral("； ")));
        } else if(result) {
            for(int ch=0;ch<3;++ch) {
                const auto& stats=result->time_stats[ch];
                const double offset=displayValue(0,ch),scale=displayValue(1,ch)-offset;
                values.push_back({std::hypot(stats.ac_rms*scale,stats.mean*scale+offset),stats.peak_to_peak*std::abs(scale)});
            }
            scope=QStringLiteral("全记录原始采样统计");
        }
        singleWave->setAmplitudeStats(values,scope);
    }
    void pauseNavigation() {
        if(liveDisplay&&following) {refreshLive(true,true);following=false;liveToggle->setText(QStringLiteral("回到最新"));liveSpan->setEnabled(false);syncSpectrumDemand(false);}
        updateNavigation();
    }
    void resumeFollowing() {if(!liveDisplay||!live->isLive())return;following=true;liveToggle->setText(QStringLiteral("暂停显示"));liveSpan->setEnabled(true);refreshLive(true);updateNavigation();}
    void requestWaveRange(double first,double last) {
        if(liveDisplay) {windowStart=first;windowEnd=last;singleWave->setXRange(first,last);updateNavigation();}
        else q->setWindow(first,last);
    }
    void enabled(bool on) {
        // Let users set display limits before opening a file or starting ADC.
        // Disabling the whole view also disabled its Y-range entry point.
        singleWave->setDataAvailable(on);
        singleSpectrum->setDataAvailable(on);
        if (liveToggle) liveToggle->setEnabled(on);
        if (liveSpan) liveSpan->setEnabled(on && following);
    }
    void selectWave(double time) {
        if (liveDisplay) {
            if (!std::isfinite(time) || liveView.voltage.size() != 3 || liveView.voltage[0].points.isEmpty()) return;
            const auto& points = liveView.voltage[0].points;
            auto hit = std::lower_bound(points.begin(), points.end(), time, [](const auto& point, double x) { return point.x() < x; });
            int best = -1; double distance = std::numeric_limits<double>::infinity();
            const int index = int(hit - points.begin());
            for (int i = std::max(0, index - 2); i < std::min(points.size(), index + 3); ++i) if (std::isfinite(points[i].y()) && std::abs(points[i].x() - time) < distance) {
                best = i; distance = std::abs(points[i].x() - time);
            }
            const auto rate = live->info().sampleRate;
            if (best < 0 || !rate || distance > .51 / rate) { waveCursor->setText(QStringLiteral("该时段没有已验证的预览样本")); waveCursor->show(); return; }
            cursor = points[best].x(); wave->setCursor(cursor);
            waveCursor->setText(QStringLiteral("%1 s     CH1  %2     CH2  %3     CH3  %4 %5")
                .arg(numeric(*cursor, 8), numeric(voltage_to_amplitude(liveView.voltage[0].points[best].y(),0,liveConfig), 7),
                     numeric(voltage_to_amplitude(liveView.voltage[1].points[best].y(),1,liveConfig), 7), numeric(voltage_to_amplitude(liveView.voltage[2].points[best].y(),2,liveConfig), 7), amplitudeUnit(liveConfig)));
            waveCursor->show(); return;
        }
        if (!result || !std::isfinite(time)) return;
        try {
            WaveformPoint point;
            if (capture) point = gui::waveform_cursor(*capture, waveConfig(), time);
            else {
                const auto& points = result->waveform;
                if (points.empty()) return;
                const auto found = std::min_element(points.begin(), points.end(), [time](const auto& a, const auto& b) {
                    return std::abs(a.time_s - time) < std::abs(b.time_s - time);
                });
                point = *found;
                for(std::size_t ch=0;ch<3;++ch)point.values[ch]=displayValue(point.values[ch],ch);
            }
            cursor = point.time_s;
            wave->setCursor(cursor);
            waveCursor->setText(QStringLiteral("%1 s     CH1  %2     CH2  %3     CH3  %4 %5")
                .arg(numeric(point.time_s, 7), numeric(point.values[0], 6), numeric(point.values[1], 6), numeric(point.values[2], 6), amplitudeUnit(waveConfig())));
            waveCursor->show();
        } catch (const std::exception& e) { status(QString::fromUtf8(e.what())); }
    }
    QVector<PlotSeries> waveformSeries() {
        auto found = std::find_if(previews.begin(), previews.end(), [this](const Preview& p) {
            return p.start == windowStart && p.end == windowEnd;
        });
        if (found != previews.end()) {
            const auto hit = *found;
            previews.erase(found); previews.push_back(hit);
            return hit.series;
        }
        // Retain every original sample in this viewport. Reduction belongs to
        // the physical-pixel renderer; zoom must never enlarge an old envelope.
        const auto points = capture ? gui::waveform_preview(*capture, waveConfig(), windowStart, windowEnd,
            std::max<std::size_t>(12, capture->records.size())) : result->waveform;
        QVector<PlotSeries> out;
        for (int axis = 0; axis < 3; ++axis) {
            PlotSeries series{channelNames[axis], channelColors[axis], {}};
            series.points.reserve(int(points.size()));
            for (const auto& p : points)
                // Raw-window selection already uses sample-index rounding in
                // view_math. Rechecking floating drag bounds can drop an exact
                // endpoint (for example .105 versus .10500000000000001).
                if (capture || (p.time_s >= windowStart && p.time_s <= windowEnd)) series.points.push_back({p.time_s, capture?p.values[axis]:displayValue(p.values[axis],axis)});
            out.push_back(std::move(series));
        }
        // Six small raw windows are bounded below 6 MiB across three channels.
        // Large full-record views remain in the active plot, not six duplicates.
        if (points.size() <= 20000) {
            if (previews.size() >= 6) previews.erase(previews.begin());
            previews.push_back({windowStart, windowEnd, out});
        }
        return out;
    }
    void renderWindow() {
        if (!result) return;
        try {
            auto series = waveformSeries();
            singleWave->setSourceSeries(series, amplitudeUnit(waveConfig()));
            singleWave->setXRange(windowStart, windowEnd);
            updateNavigation();
            if (cursor && *cursor >= windowStart && *cursor <= windowEnd) selectWave(*cursor);
            else { cursor.reset(); waveCursor->clear(); waveCursor->hide(); }
        } catch (const std::exception& e) { status(QString::fromUtf8(e.what())); }
    }
    void syncSpectrumDemand(bool allowNewWork = true) {
        live->setSpectrumDemand(allowNewWork && liveDisplay && live->isLive() && following && q->isVisible() && views->currentIndex() == 1);
    }
    void selectSpectrum(double hz) {
        const auto* spectra = liveDisplay ? &liveSpectrum.channels : result ? &result->spectra : nullptr;
        if (!spectra || (*spectra)[0].frequency_hz.empty()) return;
        try {
            const auto config = liveDisplay ? liveSpectrumConfig() : result->config;
            const auto k = gui::nearest_frequency_bin((*spectra)[0], hz);
            const double frequency = (*spectra)[0].frequency_hz[k];
            QStringList values;
            for (int axis = 0; axis < 3; ++axis) {
                const auto& spectrum = (*spectra)[axis];
                values << QStringLiteral("%1  %2").arg(channelNames[axis], k < spectrum.peak_amplitude.size() ? numeric(spectrum.peak_amplitude[k] * (liveDisplay ? spectrumScale(std::size_t(axis), config) : 1.0), 6) : QStringLiteral("—"));
            }
            spectrum->setCursor(frequency);
            spectrumCursor->setText(QStringLiteral("%1 Hz     %2 %3").arg(numeric(frequency, 7), values.join(QStringLiteral("     ")), amplitudeUnit(config)));
            spectrumCursor->show();
        } catch (const std::exception& e) { status(QString::fromUtf8(e.what())); }
    }
    void renderOrderMarkers() {
        QVector<QPointF> references;QStringList texts;
        if(orderMarkersVisible&&liveDisplay&&liveView.rpm&&liveView.meanSpeed&&std::isfinite(*liveView.meanSpeed)&&*liveView.meanSpeed>0&&liveSpectrum.available){
            const double base=*liveView.meanSpeed/60.;
            for(int order=1;order<=3;++order){const double frequency=base*order;references.push_back({frequency,double(order)});texts<<QStringLiteral("%1X %2 Hz").arg(order).arg(numeric(frequency,5));}
        }else if(orderMarkersVisible&&!liveDisplay&&result){
            for(const auto& band:result->order_bands[0])if(band.rms&&band.center_hz&&std::isfinite(*band.center_hz)&&*band.center_hz>0){references.push_back({*band.center_hz,double(band.order)});texts<<QStringLiteral("%1X %2 Hz").arg(band.order).arg(numeric(*band.center_hz,5));}
        }
        spectrum->setReferenceLines(references);
        spectrumDetail->setToolTip(!orderMarkersVisible?QStringLiteral("转频标记已关闭；频谱保留原始 FFT 幅值。"):texts.isEmpty()?QStringLiteral("暂无已确认的转速，未显示转频标记。"):texts.join(QStringLiteral("    ")));
    }
    void renderLiveSpectrum() {
        ++spectrumBuilds;
        const auto config = liveSpectrumConfig();
        const auto selectedFrequency = spectrum->cursorX();
        QVector<PlotSeries> series;
        if (liveSpectrum.available) for (int channel = 0; channel < 3; ++channel) {
            const auto& sp = liveSpectrum.channels[channel]; PlotSeries line{channelNames[channel], channelColors[channel], {}};
            const auto count = std::min(sp.frequency_hz.size(), sp.peak_amplitude.size()); line.points.reserve(int(count));
            const auto scale = spectrumScale(std::size_t(channel), config);
            for (std::size_t i = 0; i < count; ++i) line.points.push_back({sp.frequency_hz[i], sp.peak_amplitude[i] * scale});
            series.push_back(std::move(line));
        }
        singleSpectrum->setSourceSeries(series, amplitudeUnit(config));
        renderOrderMarkers();
        if (liveSpectrum.available) spectrumDetail->setText(QStringLiteral("实时幅度谱 · Δf %1 Hz · FFT 窗 %2 s")
            .arg(numeric(liveSpectrum.channels[0].bin_width_hz, 4), numeric(liveSpectrum.channels[0].window_duration_s, 4)));
        else spectrumDetail->setText(liveSpectrum.error);
        if (selectedFrequency) selectSpectrum(*selectedFrequency);
    }
    void updateLiveSpectrumDetail(const LivePreviewInfo& info) {
        if (!demandAnalysis) return;
        if (liveSpectrum.available) spectrumDetail->setText(QStringLiteral("实时幅度谱 · Δf %1 Hz · 快照 %2–%3 s%4")
            .arg(numeric(liveSpectrum.channels[0].bin_width_hz, 4), numeric(liveSpectrum.startTime, 5), numeric(liveSpectrum.endTime, 5),
                 !info.active ? QStringLiteral(" · 预览结束") :
                 (liveSpectrum.inputRevision == info.revision ? QString() : QStringLiteral(" · 更新中"))));
        else if (!info.active) spectrumDetail->setText(QStringLiteral("实时预览已结束"));
        else if (liveSpectrum.error.isEmpty()) spectrumDetail->setText(info.quarantined ? info.error :
            (info.retainedRecords < 16 ? QStringLiteral("等待连续样本") : QStringLiteral("正在计算最新连续样本")));
    }
    void refreshLive(bool force = false, bool allViews = false) {
        if (!liveDisplay) return;
        if(liveBatchDepth>0)return;
        updateRenderBudget();
        if(!force&&lastLivePresentation.isValid()&&lastLivePresentation.elapsed()<renderBudget.intervalMs()){
            ++renderBudgetDeferrals;return;
        }
        struct Measure {
            Impl& owner;QElapsedTimer clock;bool presented=false;
            explicit Measure(Impl& value):owner(value){clock.start();}
            ~Measure(){if(presented){owner.lastRefreshWorkNs=clock.nsecsElapsed();owner.lastLivePresentation.start();}}
        } measured(*this);
        ScopedTiming total(refreshTotal, profiling);
        syncSpectrumDemand(!allViews);
        { ScopedTiming stage(pollTiming, profiling); live->pollSpectrum(); }
        const auto info = live->info();
        {
        ScopedTiming stage(propertyTiming, profiling);
        q->setProperty("liveAcceptedFrames", QVariant::fromValue<qulonglong>(info.acceptedFrames));
        q->setProperty("liveRetainedRecords", QVariant::fromValue<qulonglong>(info.retainedRecords));
        q->setProperty("liveFFTCompleted", QVariant::fromValue<qulonglong>(info.fftCompleted));
        q->setProperty("livePreviewDroppedRecords",QVariant::fromValue<qulonglong>(info.previewDroppedRecords));
        }
        if (!following) return;
        if (demandAnalysis && !q->isVisible()) return;
        const bool wantWave = allViews || views->currentIndex() == 0;
        const bool wantSpeed = allViews || views->currentIndex() == 0 || views->currentIndex() == 2 || (orderMarkersVisible && views->currentIndex() == 1);
        bool navigationPending = false;
        if ((!demandAnalysis || wantWave || wantSpeed) && (force || info.revision != liveDrawRevision)) {
            LivePreviewView view;
            { ScopedTiming stage(viewTiming, profiling); view = live->view(liveSpan->value(), wantWave, wantSpeed); }
            liveDrawRevision = info.revision;
            if ((wantWave && !view.voltage.isEmpty() && !view.voltage[0].points.isEmpty()) || (wantSpeed && !view.speed.points.isEmpty())) {
                liveView = std::move(view); liveDrawRevision = info.revision;
                measured.presented=true;
                q->setProperty("liveRenderedFrames", QVariant::fromValue<qulonglong>(++liveDrawCount));
                windowStart = liveView.startTime; windowEnd = liveView.endTime;
                if (wantWave) { ScopedTiming stage(waveTiming, profiling); singleWave->setSourceSeries(amplitudeSeries(), amplitudeUnit(liveConfig)); singleWave->setNavigationBounds(windowStart,windowEnd);singleWave->setXRange(windowStart, windowEnd); }
                if (wantSpeed) {
                ScopedTiming stage(speedTiming, profiling);
                speed->updateSeries({liveView.speed}, false); speed->setXRange(windowStart, windowEnd);
                singleWave->setSpeedSeries(liveView.speed,liveView.rpm?QStringLiteral("RPM"):QStringLiteral("Hz"));
                singleWave->setSpeedValue(liveView.meanSpeed,QStringLiteral("当前预览窗口平均 · %1–%2 s").arg(numeric(liveView.startTime,5),numeric(liveView.endTime,5)),liveView.rpm);
                singleWave->setSpeedSummary(QStringLiteral("均值 %1").arg(optionalValue(liveView.meanSpeed,4)),QStringLiteral("原始预览窗 · 有效测速 %1% · 最小/最大 %2 / %3").arg(numeric(liveView.validSpeedFraction*100,4),optionalValue(liveView.minimumSpeed,6),optionalValue(liveView.maximumSpeed,6)));
                speedTitles[0]->setText(liveView.rpm ? QStringLiteral("平均转速 / RPM") : QStringLiteral("平均脉冲 / Hz"));
                speedTitles[1]->setText(liveView.rpm ? QStringLiteral("最小 / 最大 / RPM") : QStringLiteral("最小 / 最大 / Hz"));
                speedValues[0]->setText(optionalValue(liveView.meanSpeed, 5));
                speedValues[1]->setText(optionalValue(liveView.minimumSpeed, 4) + QStringLiteral(" / ") + optionalValue(liveView.maximumSpeed, 4));
                speedValues[2]->setText(numeric(liveView.validSpeedFraction * 100, 4) + QStringLiteral("%"));
                speedValues[3]->setText(info.pprConfirmed ? QString::number(info.ppr) : QStringLiteral("未确认"));
                if (liveView.validSpeedFraction <= 0) speedNote->clear();
                else if (!info.pprConfirmed) speedNote->setText(QStringLiteral("PPR 未确认，当前显示脉冲频率"));
                else if (liveView.validSpeedFraction > 0 && liveView.validSpeedFraction < 1) speedNote->setText(QStringLiteral("测速缺测区域保留为空"));
                else speedNote->clear();
                speedNote->setVisible(!speedNote->text().isEmpty());
                }
                if (wantWave) {
                    ScopedTiming stage(cursorTiming, profiling);
                    if (cursor && *cursor >= windowStart && *cursor <= windowEnd) selectWave(*cursor);
                    else { cursor.reset(); waveCursor->clear(); waveCursor->hide(); }
                }
                // The plot adopts a new raw window above. Its visible range
                // label and raw-window RMS/peak-to-peak must adopt that same
                // revision, rather than remaining at the first navigation event.
                // Publish once after all requested buffers have been adopted.
                navigationPending = true;
            }
        }
        // Pausing may freeze all three views once. Normal following never
        // copies or materializes hidden spectrum buffers in the opt-in path.
        if (demandAnalysis && !allViews && views->currentIndex() != 1) {
            if (navigationPending) updateNavigation();
            return;
        }
        ScopedTiming spectrumStage(spectrumTiming, profiling);
        ++spectrumCopies;
        const auto nextSpectrum = live->spectrum();
        if (force || nextSpectrum.revision != liveSpectrumRevision || nextSpectrum.available != liveSpectrum.available) {
            measured.presented=true;
            liveSpectrum = nextSpectrum; liveSpectrumRevision = nextSpectrum.revision;
            renderLiveSpectrum();
        }
        updateLiveSpectrumDetail(info);
        updateNavigation();
    }
};

AnalysisPane::AnalysisPane(QWidget* parent) : QWidget(parent), impl_(std::make_unique<Impl>(this)) {
    impl_->liveConfig.amplitude_unit = AmplitudeUnit::Voltage;
    setObjectName(QStringLiteral("analysisPane"));
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral(
        "QWidget#analysisPane{background:#ffffff;color:#1a1c1f;}"
        "QLabel{background:transparent;color:#344255;font-size:16px;}"
        "QLabel[role='muted']{color:#344255;font-size:16px;}"
        "QLabel[role='figure']{color:#344255;font-size:16px;}"
        "QLabel[role='metric']{color:#263345;font-size:24px;font-weight:500;}"
        "QPushButton{background:#ffffff;border:1px solid #8d9bad;border-radius:5px;color:#344255;padding:0 9px;font-size:16px;}"
        "QPushButton:hover{background:#f0f5fb;}QPushButton:pressed{background:#e2ebf6;}"
        "QPushButton:disabled{color:#687586;border-color:#ccd4df;}"
        "QDoubleSpinBox{background:#ffffff;border:1px solid #dcdcdc;border-radius:6px;padding:0 9px;min-height:44px;color:#343638;font-size:16px;}"
        "QCheckBox{background:transparent;font-size:16px;}"
        "QFrame[role='figureSurface']{background:#ffffff;border-top:1px solid #ededed;}"
        "QFrame[role='metricStrip']{background:#ffffff;border-top:1px solid #ededed;border-bottom:1px solid #ededed;}"));
    auto& d = *impl_;
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    d.views = new QStackedWidget; d.views->setObjectName(QStringLiteral("analysisViews")); layout->addWidget(d.views);
    auto* waves = new QWidget; auto* waveLayout = new QVBoxLayout(waves);
    waveLayout->setContentsMargins(0, 0, 0, 0); waveLayout->setSpacing(2);
    d.liveTools = new QWidget; auto* liveTools = new QHBoxLayout(d.liveTools); liveTools->setContentsMargins(0, 0, 0, 0); liveTools->setSpacing(8);
    d.liveToggle = button(QStringLiteral("暂停显示"), "liveFollow", 104); d.liveToggle->setToolTip(QStringLiteral("仅冻结显示，采集继续")); liveTools->addWidget(d.liveToggle);
    d.liveSpan = new NumberField; d.liveSpan->setObjectName(QStringLiteral("liveWindow")); d.liveSpan->setAccessibleName(QStringLiteral("实时时间窗"));
    d.liveSpan->setDecimals(3); d.liveSpan->setRange(.02, 2.0); d.liveSpan->setValue(.25); d.liveSpan->setSingleStep(.05);
    d.liveSpan->setPrefix(QStringLiteral("时间窗 ")); d.liveSpan->setSuffix(QStringLiteral(" s")); d.liveSpan->setFixedWidth(160); liveTools->addWidget(d.liveSpan);
    d.singleWave = new SingleWaveformView; d.wave=d.singleWave->plot();
    // Follow/pause and its window share the existing Y-range tools row. They
    // never consume another full button-height row above a small-screen plot.
    d.singleWave->findChild<QWidget*>("plotRangeTools")->layout()->addWidget(d.liveTools);d.liveTools->hide();
    d.wave->cursorSelected = [this](double time) { impl_->selectWave(time); };
    d.wave->navigationStarted = [this] { impl_->pauseNavigation(); };
    d.wave->xRangeRequested = [this](double first,double last) { impl_->requestWaveRange(first,last); };
    d.singleWave->selectionChanged = [this] { if (impl_->cursor) impl_->selectWave(*impl_->cursor);impl_->updateNavigation(); };
    waveLayout->addWidget(d.singleWave, 1); d.waveCursor = label({}, "muted"); d.waveCursor->setObjectName(QStringLiteral("waveCursorReadout"));
    d.waveCursor->setMinimumHeight(24); fullWidthLabel(d.waveCursor); d.views->addWidget(waves);

    auto* frequencies = new QWidget; auto* frequencyLayout = new QVBoxLayout(frequencies);
    frequencyLayout->setContentsMargins(0, 0, 0, 0); frequencyLayout->setSpacing(2);
    d.spectrumDetail = label(QStringLiteral("等待频谱"), "muted"); d.spectrumDetail->setObjectName(QStringLiteral("spectrumDetail"));
    fullWidthLabel(d.spectrumDetail);
    d.singleSpectrum = new SingleWaveformView(frequencies, false, QStringLiteral("频率 / Hz"));
    d.spectrum = d.singleSpectrum->plot(); frequencyLayout->addWidget(d.singleSpectrum, 1);
    d.singleSpectrum->selectionChanged = [this] { impl_->updateNavigation(); };
    d.spectrumCursor = label({}, "muted"); d.spectrumCursor->setObjectName(QStringLiteral("spectrumCursorReadout"));
    d.spectrumCursor->setMinimumHeight(24); fullWidthLabel(d.spectrumCursor); d.views->addWidget(frequencies);
    d.spectrum->cursorSelected = [this](double hz) { impl_->selectSpectrum(hz); };

    auto* speeds = new QWidget; auto* speedLayout = new QVBoxLayout(speeds);
    speedLayout->setContentsMargins(0, 0, 0, 0); speedLayout->setSpacing(12);
    auto* strip = new QFrame; strip->setProperty("role", "metricStrip"); auto* metrics = new QHBoxLayout(strip);
    metrics->setContentsMargins(8, 8, 8, 10); metrics->setSpacing(20);
    const std::array<QString, 4> names{QStringLiteral("平均转速 / RPM"), QStringLiteral("最小 / 最大 / RPM"), QStringLiteral("有效测速"), QStringLiteral("每转脉冲")};
    for (int i = 0; i < 4; ++i) {
        auto* column = new QVBoxLayout; column->setSpacing(3); d.speedTitles[i] = label(names[i], "muted"); column->addWidget(d.speedTitles[i]);
        d.speedValues[i] = label(QStringLiteral("—"), "metric"); d.speedValues[i]->setObjectName(QStringLiteral("speedMetric%1").arg(i));
        fullWidthLabel(d.speedValues[i]); column->addWidget(d.speedValues[i]); metrics->addLayout(column, 1);
    }
    speedLayout->addWidget(strip); d.speed = new PlotCanvas; d.speed->setObjectName(QStringLiteral("workbenchSpeed"));
    d.speed->setEmbedded(true); d.speed->setLegendVisible(false); speedLayout->addWidget(d.speed, 1);
    d.speedNote = label({}, "muted"); d.speedNote->setObjectName(QStringLiteral("speedTraceNote"));
    d.speedNote->setMinimumHeight(28); fullWidthLabel(d.speedNote); speedLayout->addWidget(d.speedNote); d.views->addWidget(speeds);
    auto* navigation=new QWidget;auto* navigationLayout=new QHBoxLayout(navigation);navigationLayout->setContentsMargins(0,0,0,0);navigationLayout->setSpacing(8);
    d.navigationNote=label({},"muted");d.navigationNote->setObjectName("plotNavigationNote");fullWidthLabel(d.navigationNote);navigationLayout->addWidget(d.navigationNote,1);
    d.spectrumDetail->setMaximumWidth(320);navigationLayout->addWidget(d.spectrumDetail,1);d.spectrumDetail->hide();
    // Cursor details stay reachable in each channel sidebar, without reducing
    // the graph's height whenever the operator selects a sample.
    for(const auto& item:{qMakePair(d.singleWave,d.waveCursor),qMakePair(d.singleSpectrum,d.spectrumCursor)}){
        const auto scrollName=item.first==d.singleWave?"waveSelectorScroll":"spectrumSelectorScroll";
        auto* sidebar=item.first->findChild<QScrollArea*>(scrollName)->widget();
        auto* sidebarLayout=qobject_cast<QVBoxLayout*>(sidebar->layout());item.second->setWordWrap(true);item.second->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);sidebarLayout->insertWidget(sidebarLayout->count()-1,item.second);
    }
    d.navigationNote->setToolTip(QStringLiteral("单指拖动 · 双指横向缩放 · 双指纵向调幅 · 双击恢复；鼠标滚轮缩放、Shift+拖动框选；右键或长按设置精确范围"));
    layout->insertWidget(0,navigation);
    for(auto* plot:{d.wave,d.spectrum,d.speed}){plot->resumeLatestRequested=[this]{impl_->resumeFollowing();};plot->viewChanged=[this]{impl_->updateNavigation();};}
    for(auto* plot:{d.spectrum,d.speed}) {plot->navigationStarted=[this]{impl_->pauseNavigation();};plot->xRangeRequested=[this,plot](double first,double last){plot->setXRange(first,last);impl_->updateNavigation();};}
    d.liveTimer = new QTimer(this); d.liveTimer->setTimerType(Qt::PreciseTimer); d.liveTimer->setInterval(40);
    connect(d.liveTimer, &QTimer::timeout, this, [this] { impl_->refreshLive(); });
    connect(d.liveToggle, &QPushButton::clicked, this, [this] {
        auto& state = *impl_;
        // Freeze one complete snapshot so changing pages while paused cannot
        // reveal a newer hidden plot or require rebuilding every hidden view
        // during normal following.
        if (state.following) state.refreshLive(true, true);
        state.following = !state.following; state.liveToggle->setText(state.following ? QStringLiteral("暂停显示") : QStringLiteral("回到最新"));
        state.liveSpan->setEnabled(state.following); if (state.following) state.refreshLive(true);
        state.updateNavigation();
    });
    connect(d.liveSpan, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { if (impl_->following) impl_->refreshLive(true); });
    clear();
}

AnalysisPane::~AnalysisPane() = default;

void AnalysisPane::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (impl_->demandAnalysis && impl_->liveDisplay && impl_->following) impl_->refreshLive(true);
}
void AnalysisPane::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (impl_->demandAnalysis) impl_->live->setSpectrumDemand(false);
}

void AnalysisPane::setView(int index) {
    if (index < 0 || index >= 3) return;
    impl_->views->setCurrentIndex(index);
    impl_->spectrumDetail->setVisible(index==1);
    if (impl_->liveDisplay && impl_->following) impl_->refreshLive(true);
    impl_->updateNavigation();
}

void AnalysisPane::clear() {
    auto& d = *impl_; d.result = nullptr; d.capture.reset(); d.previews.clear(); d.cursor.reset(); d.enabled(false);
    d.liveTimer->stop(); d.live->setSpectrumDemand(false); d.live->reset(); d.liveDisplay = false; d.following = true; d.liveView = {}; d.liveSpectrum = {};
    // The enclosing transport batch survives beginLive's clear().
    d.renderBudget.reset();d.budgetPlot=nullptr;d.lastBudgetBuild=0;d.lastRefreshWorkNs=0;d.lastLivePresentation.invalidate();d.liveTimer->setInterval(40);
    d.liveDrawRevision = d.liveSpectrumRevision = 0; d.liveTools->hide();
    d.liveDrawCount = 0;
    for (const char* key : {"liveAcceptedFrames", "liveRetainedRecords", "liveFFTCompleted", "liveRenderedFrames"}) setProperty(key, QVariant::fromValue<qulonglong>(0));
    d.windowStart = 0; d.windowEnd = 1;
    d.singleWave->clearData();
    d.singleSpectrum->clearData(); d.spectrum->setReferenceLines({});
    d.speed->clearXRange(); d.speed->setChart({}, QStringLiteral("时间 / s"), QStringLiteral("RPM"), {}, QStringLiteral("打开记录查看转速"));
    for (auto* value : d.speedValues) value->setText(QStringLiteral("—"));
    d.speedTitles[0]->setText(QStringLiteral("平均转速 / RPM"));
    d.speedTitles[1]->setText(QStringLiteral("最小 / 最大 / RPM"));
    d.waveCursor->clear(); d.waveCursor->hide(); d.spectrumCursor->clear(); d.spectrumCursor->hide();
    d.spectrumDetail->setText(QStringLiteral("等待频谱")); d.spectrumDetail->setToolTip({}); d.speedNote->clear(); d.speedNote->hide();
    d.spectrum->clearNavigationBounds();d.speed->clearYRange();d.speed->clearNavigationBounds();d.updateNavigation();
}

void AnalysisPane::setWindow(double first, double last) {
    auto& d = *impl_; if (!d.result) return;
    if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(d.result->duration_s) || d.result->duration_s <= 0) {
        d.status(QStringLiteral("显示时间范围无效")); return;
    }
    first = std::clamp(first, 0.0, d.result->duration_s); last = std::clamp(last, 0.0, d.result->duration_s);
    if (last <= first) { d.status(QStringLiteral("结束时间须大于开始时间")); return; }
    const bool unchanged = first == d.windowStart && last == d.windowEnd && !d.previews.empty();
    d.windowStart = first; d.windowEnd = last;
    if (!unchanged) d.renderWindow();
}

void AnalysisPane::setResult(std::shared_ptr<const Capture> capture, const AnalysisResult* result) {
    clear(); if (!result) return;
    auto& d = *impl_; d.capture = std::move(capture); d.result = result;
    const auto& r = *result;
    d.enabled(true);
    const double duration = std::isfinite(r.duration_s) && r.duration_s > 0 ? r.duration_s : 1;
    d.singleWave->setNavigationBounds(0,duration);
    QVector<PlotSeries> frequencySeries;
    for (int axis = 0; axis < 3; ++axis) {
        const auto& spectrum = r.spectra[axis]; PlotSeries series{channelNames[axis], channelColors[axis], {}};
        const auto n = std::min(spectrum.frequency_hz.size(), spectrum.peak_amplitude.size()); series.points.reserve(int(n));
        for (std::size_t k = 0; k < n; ++k) series.points.push_back({spectrum.frequency_hz[k], spectrum.peak_amplitude[k]});
        frequencySeries.push_back(std::move(series));
    }
    d.singleSpectrum->setSourceSeries(frequencySeries, amplitudeUnit(r.config));
    const double upper = std::min(r.config.spectrum_max_hz, double(r.header.sample_rate_hz) / 2);
    if (upper > 0) d.spectrum->setXRange(0, upper);
    d.renderOrderMarkers();
    const auto& s = r.spectra[0];
    d.spectrumDetail->setText(QStringLiteral("幅度谱 · Δf %1 Hz · %2 段").arg(numeric(s.bin_width_hz, 4)).arg(s.segment_count));
    const bool havePpr = bool(r.speed.pulses_per_rev); PlotSeries trace{{}, channelColors[0], {}};
    for (const auto& point : r.speed.trace) trace.points.push_back({point.time_s,
        (havePpr ? point.rpm : point.pulse_frequency_hz).value_or(std::numeric_limits<double>::quiet_NaN())});
    d.speed->setChart({}, QStringLiteral("时间 / s"), havePpr ? QStringLiteral("RPM") : QStringLiteral("脉冲 / Hz"), {trace}, QStringLiteral("未收到有效测速脉冲，请检查测速输入"));
    d.speed->setXRange(0, duration);
    d.singleWave->setSpeedSeries(trace,havePpr?QStringLiteral("RPM"):QStringLiteral("Hz"));
    d.singleWave->setSpeedValue(havePpr?r.speed.held_mean_rpm:r.speed.held_mean_pulse_hz,QStringLiteral("全记录平均"),havePpr);
    d.singleWave->setSpeedSummary(QStringLiteral("均值 %1").arg(havePpr?optionalValue(r.speed.held_mean_rpm,4):optionalValue(r.speed.held_mean_pulse_hz,4)),QStringLiteral("全记录测速 · 有效占比 %1%").arg(numeric(r.speed.valid_fraction*100,4)));
    d.speedTitles[0]->setText(havePpr ? QStringLiteral("平均转速 / RPM") : QStringLiteral("平均脉冲 / Hz"));
    d.speedTitles[1]->setText(havePpr ? QStringLiteral("最小 / 最大 / RPM") : QStringLiteral("转速范围 / RPM"));
    d.speedValues[0]->setText(havePpr ? optionalValue(r.speed.held_mean_rpm, 5) : optionalValue(r.speed.held_mean_pulse_hz, 5));
    d.speedValues[1]->setText(havePpr ? optionalValue(r.speed.min_rpm, 4) + QStringLiteral(" / ") + optionalValue(r.speed.max_rpm, 4) : QStringLiteral("— / —"));
    d.speedValues[2]->setText(numeric(r.speed.valid_fraction * 100, 4) + QStringLiteral("%"));
    d.speedValues[3]->setText(optionalValue(r.speed.pulses_per_rev, 5));
    d.speedNote->setText(r.speed.valid_fraction <= 0 ? QString() : !havePpr ? QStringLiteral("PPR 未知，当前显示脉冲频率；RPM 与转频分析暂停") :
        r.speed.trace_gaps_coalesced ? QStringLiteral("缺测区间按显示桶合并；全记录统计保留原值") : QString());
    d.speedNote->setVisible(!d.speedNote->text().isEmpty());
    setWindow(0, std::min(.25, duration));
}

bool AnalysisPane::beginLive(const QJsonObject& firstFrame, bool pprConfirmed, QString* error) {
    clear(); auto& d = *impl_;
    if (!d.live->begin(firstFrame, pprConfirmed, error)) { d.status(error ? *error : d.live->info().error); return false; }
    d.liveDisplay = true; d.following = true; d.enabled(true); d.liveTools->show();
    d.liveToggle->setText(QStringLiteral("暂停显示")); { const QSignalBlocker blocked(d.liveSpan); d.liveSpan->setValue(.25); }
    const auto info = d.live->info();
    d.singleSpectrum->setSourceSeries({}, amplitudeUnit(d.liveSpectrumConfig()));
    d.spectrumDetail->setText(QStringLiteral("等待连续样本，频谱将自动显示"));
    d.spectrum->setXRange(0, std::min(2000.0, info.sampleRate / 2.0)); d.spectrum->setReferenceLines({});
    d.speed->setChart({}, QStringLiteral("时间 / s"), pprConfirmed ? QStringLiteral("RPM") : QStringLiteral("脉冲 / Hz"), {}, QStringLiteral("未收到有效测速脉冲，请检查测速输入"));
    d.refreshLive(true); d.liveTimer->start(); return true;
}
bool AnalysisPane::acceptLiveFrame(const QJsonObject& frame, QString* error) {
    auto& d = *impl_; d.syncSpectrumDemand(); const bool accepted = d.live->accept(frame, error);
    if (!accepted) d.status(error ? *error : d.live->info().error);
    return accepted;
}
void AnalysisPane::beginLiveBatch(){++impl_->liveBatchDepth;}
void AnalysisPane::endLiveBatch(){
    auto& d=*impl_;if(d.liveBatchDepth<=0)return;
    if(--d.liveBatchDepth==0){++d.liveBatches;d.refreshLive();}
}
void AnalysisPane::endLive(const QString& reason) {
    auto& d = *impl_; if (d.liveDisplay && d.following) d.refreshLive(true);
    d.live->end(reason); d.liveTimer->stop(); d.liveToggle->setEnabled(false); d.liveSpan->setEnabled(false);
    if (d.liveDisplay) d.updateLiveSpectrumDetail(d.live->info());
    if (!reason.isEmpty()) d.status(reason);
}
bool AnalysisPane::isLive() const { return impl_->live->isLive(); }
void AnalysisPane::setAnalysisConfig(const AnalysisConfig& config) {
    validate_calibration_config(config);
    impl_->liveConfig=config;
    if(impl_->accelerationMode.has_value())impl_->liveConfig.amplitude_unit=*impl_->accelerationMode?AmplitudeUnit::AccelerationSI:AmplitudeUnit::Voltage;
    impl_->previews.clear();
    setProperty("analysisAmplitudeUnit",amplitudeUnit(impl_->liveConfig));
    if(impl_->liveDisplay&&impl_->following)impl_->refreshLive(true,true);
    else if(impl_->liveDisplay){
        // Reconvert the frozen raw snapshot without polling a newer live frame.
        impl_->singleWave->setSourceSeries(impl_->amplitudeSeries(),amplitudeUnit(impl_->liveConfig));
        if(impl_->cursor)impl_->selectWave(*impl_->cursor);
        impl_->renderLiveSpectrum();impl_->updateLiveSpectrumDetail(impl_->live->info());impl_->updateNavigation();
    }
    else if(impl_->result)impl_->renderWindow();
    else {
        impl_->singleWave->setSourceSeries({},amplitudeUnit(impl_->liveConfig));
        impl_->singleSpectrum->setSourceSeries({},amplitudeUnit(impl_->liveSpectrumConfig()));
    }
}
void AnalysisPane::setAccelerationMode(bool enabled) {
    impl_->singleWave->setSpeedControlsVisible(!enabled);
    if(impl_->accelerationMode==enabled)return;
    impl_->accelerationMode=enabled;
    setAnalysisConfig(impl_->liveConfig);
}
void AnalysisPane::setOrderMarkersVisible(bool visible){impl_->orderMarkersVisible=visible;if(visible&&impl_->liveDisplay&&impl_->following)impl_->refreshLive(true);else impl_->renderOrderMarkers();}
bool AnalysisPane::orderMarkersVisible()const{return impl_->orderMarkersVisible;}
QJsonObject AnalysisPane::performanceProfile() const {
    const auto& d = *impl_;
    return {{"enabled", d.profiling}, {"demand_analysis", d.demandAnalysis},
        {"spectrum_policy", d.demandAnalysis ? "visible_following_only" : "automatic_5hz"},
        {"spectrum_copies", double(d.spectrumCopies)}, {"spectrum_ui_builds", double(d.spectrumBuilds)},
        {"preview_batches",double(d.liveBatches)},{"preview_batch_depth",d.liveBatchDepth},
        {"preview_dropped_records",double(d.live->info().previewDroppedRecords)},
        {"render_interval_ms",d.renderBudget.intervalMs()},{"render_budget_deferrals",double(d.renderBudgetDeferrals)},
        {"render_pacing","presentation only; target equal frame/idle time within 40..2000ms, nonpreemptive painting"},
        {"scope", "lifetime wall timings; model stages are nested in analysis; recent percentiles bounded to 256 observations"},
        {"refresh_total", d.refreshTotal.json()}, {"poll_spectrum", d.pollTiming.json()}, {"properties", d.propertyTiming.json()},
        {"build_view", d.viewTiming.json()}, {"adopt_wave", d.waveTiming.json()}, {"adopt_speed_and_labels", d.speedTiming.json()},
        {"cursor", d.cursorTiming.json()}, {"spectrum_copy_and_ui", d.spectrumTiming.json()}, {"model", d.live->performanceProfile()}};
}

} // namespace vibration::workbench

