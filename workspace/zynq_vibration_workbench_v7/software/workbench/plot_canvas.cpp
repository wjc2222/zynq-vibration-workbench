#include "plot_canvas.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QApplication>
#include <QTouchEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <QPaintEvent>
#include <QCache>
#include <QStaticText>
#include <QTextOption>
#include <QCoreApplication>
#include <QThread>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstring>
#include <list>
#include <utility>

namespace vibration::workbench {
namespace {
// Keep the shared keypad on the bound the user actually taps, including on
// embedded platforms where synthesized mouse presses do not transfer focus.
class RangeLimitEdit final : public QLineEdit {
public: using QLineEdit::QLineEdit;
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if(!isReadOnly()&&event->button()==Qt::LeftButton)setFocus(Qt::MouseFocusReason);
        QLineEdit::mousePressEvent(event);
    }
};
QString number(double value) {
    const double a = std::abs(value);
    if (a > 0.0 && (a < 0.001 || a >= 100000.0))
        return QString::number(value, 'g', 3);
    if (a >= 10.0) return QString::number(value, 'f', 0);
    if (a >= 1.0) return QString::number(value, 'f', 1);
    return QString::number(value, 'g', 2);
}
QString axisNumber(double value, double span) {
    // Remove arithmetic residue only from labels, relative to the displayed
    // span. Original samples and cursor values remain untouched.
    if (std::isfinite(span) && std::abs(value) <= std::abs(span) * 1e-12) value = 0;
    const double magnitude = std::abs(value), step = std::abs(span) / 4;
    if (magnitude && (magnitude < 1e-9 || magnitude >= 1e12)) return QString::number(value, 'g', 8);
    const int places = step > 0 && std::isfinite(step) ? std::clamp(int(std::ceil(-std::log10(step))) + 2, 0, 12) : 6;
    QString label = QString::number(value, 'f', places);
    if (label.contains('.')) { while (label.endsWith('0')) label.chop(1); if (label.endsWith('.')) label.chop(1); }
    return label == "-0" ? QStringLiteral("0") : label;
}
QVector<double> axisTicks(double lower, double upper, int intervals, bool nice) {
    QVector<double> ticks;
    if (nice) {
        const double raw = (upper - lower) / intervals, scale = std::pow(10., std::floor(std::log10(raw)));
        if (std::isfinite(scale) && scale > 0) {
            const double normalized = raw / scale;
            double multiplier = 10;
            for (double candidate : {1., 2., 2.5, 5., 10.}) if (normalized <= candidate * (1 + 1e-12)) { multiplier = candidate; break; }
            const double step = multiplier * scale, first = std::ceil(lower / step - 1e-10) * step;
            for (int i = 0; i < 12; ++i) {
                const double value = first + i * step;
                if (value > upper + step * 1e-10) break;
                if (value >= lower - step * 1e-10) ticks.push_back(value);
            }
        }
    }
    if (ticks.size() < 2) { ticks.clear(); for (int i=0;i<=intervals;++i) ticks.push_back(lower + (upper-lower)*i/intervals); }
    return ticks;
}
const QStaticText& axisText(const QFont& font, const QString& fontKey, const QString& text) {
    // All PlotCanvas painting is on the GUI thread. Sharing a bounded numeric
    // layout cache avoids shaping the same live time labels in three rows.
    static QCache<QString, QStaticText> cache(256);
    const QString key = fontKey + QChar(0x1f) + text;
    if (auto* cached = cache.object(key)) return *cached;
    auto* prepared = new QStaticText(text); prepared->setTextFormat(Qt::PlainText);
    prepared->setTextOption(QTextOption(Qt::AlignLeft)); prepared->setPerformanceHint(QStaticText::AggressiveCaching);
    prepared->prepare(QTransform(), font); cache.insert(key, prepared); return *prepared;
}
void drawAxisText(QPainter& painter, const QRectF& box, Qt::Alignment alignment, const QStaticText& text) {
    const auto size = text.size();
    const double x = box.left() + (alignment & Qt::AlignRight ? box.width() - size.width() : alignment & Qt::AlignHCenter ? (box.width() - size.width()) * .5 : 0);
    const double y = box.top() + (alignment & Qt::AlignVCenter ? (box.height() - size.height()) * .5 : 0);
    painter.drawStaticText(QPointF(x, y), text);
}
void drawVerticalDash(QPainter& painter, double x, double top, double bottom) {
    // Qt's cosmetic DashLine can restart its phase when a scrolled widget has
    // a negative device translation, unlike an intermediate image. Anchor the
    // same 4:2 logical-pixel pattern to the plot top on either paint device.
    painter.setPen(QPen(QColor("#767779"), 1, Qt::SolidLine, Qt::SquareCap));
    for (double y = top; y < bottom; y += 6.0) painter.drawLine(QPointF(x, y), QPointF(x, std::min(y + 4.0, bottom)));
}

// M4-style display envelope: first, minimum, maximum and last original sample
// in each physical pixel column, emitted in source order. No averaging or
// interpolation is applied. Every finite run gets its own buckets so even a
// subpixel missing interval remains a path break.
QVector<QPointF> pixelEnvelope(const QVector<QPointF>& input, double xMin, double xMax, int columns) {
    if (input.size() <= columns * 4 || !(xMax > xMin)) return input;
    QVector<QPointF> output; output.reserve(std::min(input.size(), columns * 4 + 16));
    int first = -1, last = -1, low = -1, high = -1, bucket = -1;
    double priorX = -std::numeric_limits<double>::infinity();
    const auto flush = [&] {
        if (first < 0) return;
        output.push_back(input[first]);
        const int early = std::min(low, high), late = std::max(low, high);
        if (early != first && early != last) output.push_back(input[early]);
        if (late != early && late != first && late != last) output.push_back(input[late]);
        if (last != first) output.push_back(input[last]);
        first = last = low = high = -1;
    };
    for (int index = 0; index < input.size(); ++index) {
        const auto& point = input[index];
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            flush(); bucket = -1;
            if (output.isEmpty() || (std::isfinite(output.last().x()) && std::isfinite(output.last().y()))) output.push_back(point);
            continue;
        }
        // Unordered XY trajectories are not oscilloscope samples. Keep their
        // original path instead of applying a time-column reduction to them.
        if (point.x() < priorX) return input;
        priorX = point.x();
        if (point.x() < xMin || point.x() > xMax) { flush(); bucket = -1; continue; }
        const int column = std::clamp(int((point.x() - xMin) / (xMax - xMin) * columns), 0, columns - 1);
        if (column != bucket) { flush(); bucket = column; }
        if (first < 0) first = last = low = high = index;
        else {
            last = index;
            if (point.y() < input[low].y()) low = index;
            if (point.y() > input[high].y()) high = index;
        }
    }
    flush(); return output;
}

quint64 doubleBits(double value) {
    static_assert(sizeof(value) == sizeof(quint64), "M4 range key requires 64-bit doubles");
    quint64 bits; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
struct SharedEnvelopeCache {
    // This numeric cache is used only by QWidget paints on the GUI thread.
    // Owning input copies prevent allocator address reuse from becoming a hit;
    // Qt copy-on-write detaches any later normal QVector mutation.
    struct Entry {
        QVector<QPointF> source, envelope;
        quint64 start = 0, end = 0, dpr = 0, bytes = 0;
        int columns = 0;
    };
    static constexpr int maximumEntries = 6;
    static constexpr quint64 maximumBytes = 8 * 1024 * 1024;
    static constexpr quint64 entryCharge = 256;
    std::list<Entry> entries;
    quint64 hits = 0, misses = 0, evictions = 0, bypasses = 0;
    quint64 retainedBytes = 0, peakBytes = 0, submittedPoints = 0;
    QVector<QPointF> get(const QVector<QPointF>& source, double start, double end, int columns, double dpr,
                         quint64& localHits, quint64& localMisses, quint64& localBypasses) {
        Q_ASSERT(!QCoreApplication::instance() || QThread::currentThread() == QCoreApplication::instance()->thread());
        const auto startBits = doubleBits(start), endBits = doubleBits(end), dprBits = doubleBits(dpr);
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->source.constData() == source.constData() && it->source.size() == source.size() &&
                it->start == startBits && it->end == endBits && it->columns == columns && it->dpr == dprBits) {
                ++hits; ++localHits;
                entries.splice(entries.begin(), entries, it);
                return entries.front().envelope;
            }
        }
        ++misses; ++localMisses; submittedPoints += quint64(source.size());
        auto envelope = pixelEnvelope(source, start, end, columns);
        // Conservative accounting intentionally charges shared/aliased buffers
        // again, and uses capacity rather than size. It bounds retained point
        // storage without relying on allocator internals or unique ownership.
        const auto bytes = entryCharge + (quint64(source.capacity()) + quint64(envelope.capacity())) * sizeof(QPointF);
        if (bytes > maximumBytes) { ++bypasses; ++localBypasses; return envelope; }
        while (!entries.empty() && (int(entries.size()) >= maximumEntries || retainedBytes + bytes > maximumBytes)) {
            retainedBytes -= entries.back().bytes; entries.pop_back(); ++evictions;
        }
        entries.push_front({source, envelope, startBits, endBits, dprBits, bytes, columns});
        retainedBytes += bytes; peakBytes = std::max(peakBytes, retainedBytes);
        return envelope;
    }
    QJsonObject profile() const {
        return {{"scope", "process_gui_thread_cache"}, {"algorithm", "original_M4_v1"},
            {"hits", double(hits)}, {"misses", double(misses)}, {"evictions", double(evictions)}, {"bypasses", double(bypasses)},
            {"entries", int(entries.size())}, {"max_entries", maximumEntries}, {"retained_bytes", double(retainedBytes)},
            {"peak_bytes", double(peakBytes)}, {"max_bytes", double(maximumBytes)}, {"raw_points_submitted", double(submittedPoints)},
            {"accounting", "conservative source+envelope QVector capacities plus 256B entry charge; aliased buffers charged separately"},
            {"counter_scope", "misses includes oversized bypasses; raw_points_submitted counts input points on cache misses, not literal loop iterations; never sum this global snapshot across canvases"}};
    }
};
SharedEnvelopeCache& sharedEnvelopeCache() { static SharedEnvelopeCache cache; return cache; }
}

PlotCanvas::PlotCanvas(QWidget* parent) : QWidget(parent) {
    // ARM r5 measured the cached variants slower than direct drawText. Keep
    // plain as production default; opt-in modes isolate both cache factors.
    const auto requested = QString::fromLatin1(qgetenv("AX7_PLOT_AXES_MODE"));
    if (requested == "text_cache" || requested == "layer_cache" || requested == "both") axesMode_ = requested;
    cacheAxisText_ = axesMode_ == "text_cache" || axesMode_ == "both";
    cacheAxisLayer_ = axesMode_ == "layer_cache" || axesMode_ == "both";
    setMinimumHeight(126);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_AcceptTouchEvents);
    cursorTimer_ = new QTimer(this);
    cursorTimer_->setSingleShot(true);
    cursorTimer_->setInterval(16);
    connect(cursorTimer_, &QTimer::timeout, this, [this] { flushCursor(); });
    touchMenuTimer_=new QTimer(this);touchMenuTimer_->setSingleShot(true);touchMenuTimer_->setInterval(550);
    connect(touchMenuTimer_,&QTimer::timeout,this,[this]{
        if(touchMode_!=TouchMode::Single||navigationBegan_||(!navigationDragging_&&!dragging_))return;
        touchMenuShown_=true;touchMode_=TouchMode::Suppressed;touchTapEligible_=touchFirstTapValid_=false;
        navigationDragging_=dragging_=false;pendingCursor_.reset();cursorTimer_->stop();
        QContextMenuEvent menu(QContextMenuEvent::Other,touchOrigin_.toPoint(),mapToGlobal(touchOrigin_.toPoint()));contextMenuEvent(&menu);
    });
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("单指拖动平移 · 双指横向捏合缩放时间/频率 · 滚轮缩放 · Shift+拖动框选 · 双击/双轻点恢复横轴；固定量程时纵轴保持不变，点击“自动”恢复自动幅值；右键或长按设置精确范围"));
}

void PlotCanvas::invalidateStatic() { staticDirty_ = true; staticValidRegion_ = {}; update(); }

void PlotCanvas::setChart(QString title, QString xLabel, QString yLabel,
                          QVector<PlotSeries> series, QString emptyMessage) {
    if (yLabel_ != yLabel && !fixedYRange_) yRange_.reset();
    title_ = std::move(title);
    xLabel_ = std::move(xLabel);
    yLabel_ = std::move(yLabel);
    series_ = std::move(series);
    ++axesMetadataRevision_; axisGutter_ = 0;
    ++dataRevision_;
    emptyMessage_ = std::move(emptyMessage);
    cursor_.reset();
    dragging_ = false;
    pendingCursor_.reset();
    cursorTimer_->stop();
    invalidateStatic();
}

void PlotCanvas::updateSeries(QVector<PlotSeries> series, bool preserveVisibility) {
    bool metadataChanged = series.size() != series_.size();
    for (int i = 0; !metadataChanged && i < series.size(); ++i) metadataChanged = series[i].name != series_[i].name || series[i].color != series_[i].color;
    if (metadataChanged) ++axesMetadataRevision_;
    if (preserveVisibility) for (int i = 0; i < series.size(); ++i) {
        if (i < series_.size() && series[i].name == series_[i].name) series[i].visible = series_[i].visible;
        else for (const auto& old : series_) if (!series[i].name.isEmpty() && old.name == series[i].name) { series[i].visible = old.visible; break; }
    }
    series_ = std::move(series); ++dataRevision_; invalidateStatic();
}

bool PlotCanvas::appendPoints(int seriesIndex, const QVector<QPointF>& points, int maximumPoints) {
    if (seriesIndex < 0 || seriesIndex >= series_.size() || maximumPoints < 2) return false;
    if (points.isEmpty()) return true;
    auto& current = series_[seriesIndex].points;
    if (points.size() >= maximumPoints) current = points.mid(points.size() - maximumPoints);
    else {
        const int discard = std::max(0, current.size() - (maximumPoints - points.size()));
        if (!discard) current += points;
        else {
            QVector<QPointF> bounded; bounded.reserve(current.size() - discard + points.size());
            for (int i = discard; i < current.size(); ++i) bounded.push_back(current[i]);
            bounded += points; current = std::move(bounded);
        }
    }
    ++dataRevision_; invalidateStatic(); return true;
}

QVector<QPointF> PlotCanvas::displayPoints(int index) const {
    return index >= 0 && index < displayPoints_.size() ? displayPoints_[index] : QVector<QPointF>{};
}
QJsonObject PlotCanvas::sharedM4Profile() const {
    auto result = sharedEnvelopeCache().profile(); result["enabled"] = sharedM4_;
    result["per_canvas_hits"] = double(sharedM4Hits_); result["per_canvas_misses"] = double(sharedM4Misses_);
    result["per_canvas_bypasses"] = double(sharedM4Bypasses_);
    result["local_reduction_counter"] = "renderStats.reductions counts local display-buffer rebuilds, including shared-cache adoption; hits/misses above distinguish actual pixelEnvelope calls";
    return result;
}

void PlotCanvas::rebuildDisplayPoints(double xMin, double xMax, int columns) {
    if (displayRevision_ == dataRevision_ && displayColumns_ == columns && displayXMin_ == xMin && displayXMax_ == xMax) return;
    QElapsedTimer elapsed; elapsed.start();
    displayPoints_.clear(); displayPoints_.reserve(series_.size()); renderStats_.inputPoints = renderStats_.drawnPoints = 0;
    for (const auto& series : series_) {
        displayPoints_.push_back(sharedM4_ ? sharedEnvelopeCache().get(series.points, xMin, xMax, columns, devicePixelRatioF(),
            sharedM4Hits_, sharedM4Misses_, sharedM4Bypasses_) : pixelEnvelope(series.points, xMin, xMax, columns));
        renderStats_.inputPoints += quint64(series.points.size());
    }
    displayRevision_ = dataRevision_; displayColumns_ = columns; displayXMin_ = xMin; displayXMax_ = xMax;
    ++renderStats_.reductions; renderStats_.lastReductionNs = elapsed.nsecsElapsed();
    if (profiling_) { renderStats_.lastEnvelopeNs += renderStats_.lastReductionNs; ++renderStats_.lastEnvelopeBuilds; }
}

void PlotCanvas::setXRange(double minimum, double maximum) {
    const bool valid = std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum;
    if (!valid || !std::isfinite(maximum-minimum)) return;
    if (hasXRange_ == valid && (!valid || (xMinimum_ == minimum && xMaximum_ == maximum))) return;
    hasXRange_ = valid;
    xMinimum_ = minimum;
    xMaximum_ = maximum;
    invalidateStatic();
}

void PlotCanvas::clearXRange() { if (hasXRange_) { hasXRange_ = false; invalidateStatic(); } }
bool PlotCanvas::setYRange(double minimum, double maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum || !std::isfinite(maximum-minimum)) return false;
    if (zeroCenteredY_) {
        const double extent = std::max(std::abs(minimum), std::abs(maximum));
        if (!std::isfinite(extent * 2)) return false;
        minimum = -extent; maximum = extent;
    }
    yRange_ = QPointF(minimum, maximum); invalidateStatic(); if(yRangeChanged)yRangeChanged();if(viewChanged)viewChanged();return true;
}
bool PlotCanvas::setFixedYRange(double minimum,double maximum) {
    if (!std::isfinite(minimum)||!std::isfinite(maximum)||maximum<=minimum||!std::isfinite(maximum-minimum)||
        (zeroCenteredY_&&!std::isfinite(2*std::max(std::abs(minimum),std::abs(maximum))))) return false;
    if(zeroBasedY_&&!zeroCenteredY_){if(maximum<=0)return false;minimum=0;}
    fixedYRange_=true;return setYRange(minimum,maximum);
}
void PlotCanvas::clearYRange() { if (yRange_ || fixedYRange_) { fixedYRange_=false;yRange_.reset(); invalidateStatic();if(yRangeChanged)yRangeChanged(); if(viewChanged)viewChanged(); } }
void PlotCanvas::setNavigationBounds(double minimum, double maximum) {
    if (std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum && std::isfinite(maximum-minimum)) navigationBounds_ = QPointF(minimum, maximum);
}
void PlotCanvas::clearNavigationBounds() { navigationBounds_.reset(); }
QPointF PlotCanvas::navigationBounds() const {
    if (navigationBounds_) return *navigationBounds_;
    double lower=std::numeric_limits<double>::infinity(), upper=-lower;
    for (const auto& line:series_) for(const auto& point:line.points) if(std::isfinite(point.x())) {lower=std::min(lower,point.x());upper=std::max(upper,point.x());}
    return std::isfinite(lower)&&upper>lower ? QPointF(lower,upper) : QPointF(0,1);
}
void PlotCanvas::setNavigationMode(NavigationMode mode) {
    resetTouchInteraction();touchFirstTapValid_=false;
    navigationMode_=mode; navigationDragging_=dragging_=false;
    setCursor(mode==NavigationMode::BoxZoom ? Qt::CrossCursor : mode==NavigationMode::Pan ? Qt::OpenHandCursor : Qt::ArrowCursor);
}
bool PlotCanvas::requestRange(double minimum,double maximum) {
    if(!std::isfinite(minimum)||!std::isfinite(maximum)||maximum<=minimum||!std::isfinite(maximum-minimum))return false;
    const auto domain=navigationBounds();const double span=domain.y()-domain.x();
    const double minimumSpan=std::min(span,std::max(1e-12,std::max(std::abs(domain.x()),std::abs(domain.y()))*1e-14));
    const double width=std::clamp(maximum-minimum,minimumSpan,span);
    const double lower=std::clamp(minimum,domain.x(),std::max(domain.x(),domain.y()-width));
    if(xRangeRequested)xRangeRequested(lower,lower+width);else setXRange(lower,lower+width);
    return true;
}
bool PlotCanvas::zoomX(double factor,std::optional<double> anchor) {
    if(!std::isfinite(factor)||factor<=0||(anchor&&!std::isfinite(*anchor)))return false;
    if(navigationStarted)navigationStarted();
    const auto range=explicitXRange().value_or(navigationBounds());
    const double at=std::clamp(anchor.value_or((range.x()+range.y())*.5),range.x(),range.y());
    const double ratio=(at-range.x())/(range.y()-range.x());
    const auto domain=navigationBounds();
    const double width=std::min((range.y()-range.x())*factor,domain.y()-domain.x());
    return requestRange(at-ratio*width,at+(1-ratio)*width);
}
bool PlotCanvas::panX(double fraction) {
    if(!std::isfinite(fraction))return false;
    if(navigationStarted)navigationStarted();const auto range=explicitXRange().value_or(navigationBounds());
    const double offset=(range.y()-range.x())*fraction;return requestRange(range.x()+offset,range.y()+offset);
}
void PlotCanvas::restoreView() { if(navigationStarted)navigationStarted();if(!fixedYRange_)clearYRange();const auto bounds=navigationBounds();requestRange(bounds.x(),bounds.y()); }
bool PlotCanvas::zoomY(double factor,std::optional<double> anchor) {
    if(fixedYRange_)return false;
    if(!std::isfinite(factor)||factor<=0||(anchor&&!std::isfinite(*anchor)))return false;
    const auto range=yRange_.value_or(displayedYRange());
    if (zeroCenteredY_) {
        const double extent = std::max(std::abs(range.x()),std::abs(range.y())) * factor;
        if(!std::isfinite(extent)||extent<=0||!std::isfinite(extent*2))return false;
        if(navigationStarted)navigationStarted();return setYRange(-extent,extent);
    }
    const double at=std::clamp(anchor.value_or((range.x()+range.y())*.5),range.x(),range.y());
    const double ratio=(at-range.x())/(range.y()-range.x()),width=(range.y()-range.x())*factor;
    const double lower=at-ratio*width,upper=at+(1-ratio)*width;
    if(!std::isfinite(lower)||!std::isfinite(upper)||upper<=lower||!std::isfinite(upper-lower))return false;
    if(navigationStarted)navigationStarted();return setYRange(lower,upper);
}
bool PlotCanvas::yAxisContains(const QPointF& position) const {return position.x()>=0&&position.x()<plotRect_.left()&&position.y()>=plotRect_.top()&&position.y()<=plotRect_.bottom();}
void PlotCanvas::beginNavigation(const QPointF& position) {
    navigationDragging_=true;navigationBegan_=false;navigationOrigin_=navigationCurrent_=position;
    navigationStartRange_=explicitXRange().value_or(navigationBounds());
    navigationStartYRange_=yRange_.value_or(displayedYRange());
}
void PlotCanvas::moveNavigation(const QPointF& position,bool final) {
    if(navigationY_&&fixedYRange_){if(final)navigationDragging_=false;return;}
    navigationCurrent_=position;
    const double distance=std::abs(position.x()-navigationOrigin_.x())+std::abs(position.y()-navigationOrigin_.y());
    if(!navigationBegan_&&distance<4) {if(final){navigationDragging_=false;if(!navigationY_)queueCursor(position,true);}return;}
    if(!navigationBegan_) {
        navigationBegan_=true;
        if(touchMode_==TouchMode::Single)beginTouchNavigation();else if(navigationStarted)navigationStarted();
        // Pausing live navigation can adopt a newer preview than the one at
        // pointer-down. Apply this first delta to that frozen view and span.
        navigationStartRange_=explicitXRange().value_or(navigationBounds());
        navigationStartYRange_=yRange_.value_or(displayedYRange());
    }
    if(navigationY_) {
        const double delta=(position.y()-navigationOrigin_.y())/std::max(1.,plotRect_.height());
        if (zeroCenteredY_) {
            const double extent=std::max(std::abs(navigationStartYRange_.x()),std::abs(navigationStartYRange_.y()))*std::pow(2.0,delta*2.0);
            setYRange(-extent,extent);
        } else {
            const double offset=delta*(navigationStartYRange_.y()-navigationStartYRange_.x());
            setYRange(navigationStartYRange_.x()+offset,navigationStartYRange_.y()+offset);
        }
    } else if(activeNavigationMode_==NavigationMode::Pan) {
        const double offset=(navigationOrigin_.x()-position.x())/std::max(1.,plotRect_.width())*(navigationStartRange_.y()-navigationStartRange_.x());
        requestRange(navigationStartRange_.x()+offset,navigationStartRange_.y()+offset);
    } else if(final&&std::abs(position.x()-navigationOrigin_.x())>=6) {
        const auto at=[this](double x){return navigationStartRange_.x()+std::clamp((x-plotRect_.left())/std::max(1.,plotRect_.width()),0.,1.)*(navigationStartRange_.y()-navigationStartRange_.x());};
        const double first=at(navigationOrigin_.x()),last=at(position.x());requestRange(std::min(first,last),std::max(first,last));
    }
    if(final)navigationDragging_=false;update();
}
void PlotCanvas::resetTouchInteraction() {
    touchMode_=TouchMode::None;touchPrimaryId_=touchSecondaryId_=-1;
    touchNavigationBegan_=touchTapEligible_=touchPinchReady_=touchMenuShown_=false;
    navigationDragging_=dragging_=false;navigationBegan_=false;
    touchMenuTimer_->stop();pendingCursor_.reset();cursorTimer_->stop();update();
}
void PlotCanvas::beginTouchNavigation() {
    if(touchNavigationBegan_)return;
    touchNavigationBegan_=true;touchTapEligible_=touchFirstTapValid_=false;
    if(navigationStarted)navigationStarted();
}
void PlotCanvas::beginPinch(int secondId,const QPointF& first,const QPointF& second) {
    touchMode_=TouchMode::Pinch;touchSecondaryId_=secondId;touchPinchReady_=false;
    touchTapEligible_=touchFirstTapValid_=false;touchMenuTimer_->stop();
    pendingCursor_.reset();cursorTimer_->stop();navigationDragging_=dragging_=false;update();
    // A second contact outside the plot cancels the single-finger gesture too,
    // but must not accidentally adjust a range or open its long-press menu.
    if(!plotRect_.contains(first)||!plotRect_.contains(second)){touchMode_=TouchMode::Suppressed;return;}
    movePinch(first,second);
}
void PlotCanvas::movePinch(const QPointF& first,const QPointF& second) {
    const double dx=second.x()-first.x(),dy=second.y()-first.y(),distance=std::hypot(dx,dy);
    if(!std::isfinite(distance)||distance<16)return;
    const QPointF center=first*.5+second*.5;
    if(!std::isfinite(center.x())||!std::isfinite(center.y()))return;
    if(!touchPinchReady_) {
        // A vertical pinch is ignored while a user-entered range is fixed.
        // Horizontal gestures retain their existing time/frequency behavior.
        if(fixedYRange_&&std::abs(dy)>1.35*std::abs(dx)){touchMode_=TouchMode::Suppressed;return;}
        // Freeze a following live preview once, before taking the zoom base.
        // A preceding one-finger pan has already issued that same callback.
        beginTouchNavigation();
        touchPinchY_=std::abs(dy)>1.35*std::abs(dx);touchPinchPlot_=plotRect_;
        touchPinchRange_=touchPinchY_?yRange_.value_or(displayedYRange()):explicitXRange().value_or(navigationBounds());
        const double span=touchPinchRange_.y()-touchPinchRange_.x();
        if(!std::isfinite(span)||span<=0)return;
        const double fraction=touchPinchY_?std::clamp((touchPinchPlot_.bottom()-center.y())/std::max(1.,touchPinchPlot_.height()),0.,1.)
            :std::clamp((center.x()-touchPinchPlot_.left())/std::max(1.,touchPinchPlot_.width()),0.,1.);
        touchPinchAnchor_=touchPinchRange_.x()+fraction*span;touchPinchDistance_=distance;touchPinchReady_=true;return;
    }
    // Keep the original data coordinate under the moving finger centroid for
    // time and ordinary axes. Vibration amplitude keeps zero at mid-height.
    // The chosen axis and plot geometry stay fixed throughout this pinch.
    const double width=(touchPinchRange_.y()-touchPinchRange_.x())*(touchPinchDistance_/distance);
    if(touchPinchY_&&fixedYRange_)return;
    if(touchPinchY_&&zeroCenteredY_){if(std::isfinite(width)&&width>0)setYRange(-width*.5,width*.5);return;}
    const double fraction=touchPinchY_?std::clamp((touchPinchPlot_.bottom()-center.y())/std::max(1.,touchPinchPlot_.height()),0.,1.)
        :std::clamp((center.x()-touchPinchPlot_.left())/std::max(1.,touchPinchPlot_.width()),0.,1.);
    const double lower=touchPinchAnchor_-fraction*width,upper=touchPinchAnchor_+(1-fraction)*width;
    if(!std::isfinite(width)||width<=0||!std::isfinite(lower)||!std::isfinite(upper)||upper<=lower)return;
    if(touchPinchY_)setYRange(lower,upper);else requestRange(lower,upper);
}
void PlotCanvas::wheelEvent(QWheelEvent* event) {
#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
    const auto position=event->position();
#else
    const auto position=event->posF();
#endif
    if(yAxisContains(position)&&event->angleDelta().y()) {
        const auto range=yRange_.value_or(displayedYRange());const double fraction=std::clamp((plotRect_.bottom()-position.y())/std::max(1.,plotRect_.height()),0.,1.);
        zoomY(std::pow(.8,event->angleDelta().y()/120.),range.x()+fraction*(range.y()-range.x()));event->accept();
    } else if(plotRect_.contains(position)&&event->angleDelta().y()) {
        const auto range=explicitXRange().value_or(navigationBounds());
        const double fraction=std::clamp((position.x()-plotRect_.left())/std::max(1.,plotRect_.width()),0.,1.);
        zoomX(std::pow(.8,event->angleDelta().y()/120.),range.x()+fraction*(range.y()-range.x()));event->accept();
    } else QWidget::wheelEvent(event);
}
void PlotCanvas::setZeroBasedY(bool enabled) { if (zeroBasedY_ != enabled) { zeroBasedY_ = enabled; invalidateStatic(); } }
void PlotCanvas::setZeroCenteredY(bool enabled) {
    if (zeroCenteredY_ == enabled) return;
    zeroCenteredY_ = enabled;
    if (enabled && yRange_) {
        const double extent=std::max(std::abs(yRange_->x()),std::abs(yRange_->y()));
        if(std::isfinite(extent*2))yRange_=QPointF(-extent,extent);else yRange_.reset();
    }
    invalidateStatic();
}
void PlotCanvas::setCompact(bool enabled) { if (compact_ != enabled) { compact_ = enabled; axisGutter_ = 0; ++axesMetadataRevision_; setMinimumHeight(enabled ? 90 : 126); invalidateStatic(); } }
void PlotCanvas::setEmbedded(bool enabled) { if (embedded_ != enabled) { embedded_ = enabled; axisGutter_ = 0; ++axesMetadataRevision_; invalidateStatic(); } }
void PlotCanvas::setXAxisVisible(bool visible) { if (xAxisVisible_ != visible) { xAxisVisible_ = visible; invalidateStatic(); } }
void PlotCanvas::setLegendVisible(bool visible) { if (legendVisible_ != visible) { legendVisible_ = visible; ++axesMetadataRevision_; invalidateStatic(); } }
void PlotCanvas::setTraceWidth(double width) {
    if (std::isfinite(width) && width >= 1.0 && width <= 2.0 && traceWidth_ != width) { traceWidth_ = width; invalidateStatic(); }
}
void PlotCanvas::setChannelVisible(int i, bool visible) {
    if (i >= 0 && i < series_.size() && series_[i].visible != visible) {
        series_[i].visible = visible;
        invalidateStatic();
    }
}
void PlotCanvas::setReferenceLines(QVector<QPointF> lines) { if (references_ != lines) { references_ = std::move(lines); invalidateStatic(); } }
QRect PlotCanvas::cursorStrip(std::optional<double> value) const {
    if (!value || !std::isfinite(*value) || !drawnData_ || *value < drawnXMin_ || *value > drawnXMax_) return {};
    const double x = plotRect_.left() + (*value - drawnXMin_) / (drawnXMax_ - drawnXMin_) * plotRect_.width();
    return QRect(qFloor(x) - 3, qFloor(plotRect_.top()) - 2, 7, qCeil(plotRect_.height()) + 5).intersected(rect());
}
void PlotCanvas::setCursor(std::optional<double> x) {
    if (x && !std::isfinite(*x)) x.reset();
    if (cursor_ == x) return;
    const auto old = cursorStrip(cursor_);
    cursor_ = x;
    const auto next = cursorStrip(cursor_);
    if (!old.isEmpty()) update(old);
    if (!next.isEmpty()) update(next);
}
void PlotCanvas::selectAtX(double x) { if (cursorSelected && drawnData_ && std::isfinite(x)) cursorSelected(std::clamp(x, drawnXMin_, drawnXMax_)); }
void PlotCanvas::flushCursor() {
    if (!pendingCursor_) return;
    const double value = *pendingCursor_;
    pendingCursor_.reset();
    selectAtX(value);
}
void PlotCanvas::queueCursor(const QPointF& position, bool final) {
    if (!drawnData_ || plotRect_.width() <= 0) return;
    const double fraction = std::clamp((position.x() - plotRect_.left()) / plotRect_.width(), 0.0, 1.0);
    pendingCursor_ = drawnXMin_ + fraction * (drawnXMax_ - drawnXMin_);
    if (final) { cursorTimer_->stop(); flushCursor(); }
    else if (!cursorTimer_->isActive()) cursorTimer_->start();
}
void PlotCanvas::mousePressEvent(QMouseEvent* event) {
    if(event->button()==Qt::LeftButton&&yAxisContains(event->localPos())) {navigationY_=true;activeNavigationMode_=NavigationMode::Pan;beginNavigation(event->localPos());event->accept();return;}
    if (event->button() == Qt::LeftButton && drawnData_ && plotRect_.contains(event->localPos())) {
        navigationY_=false;activeNavigationMode_=(event->modifiers()&Qt::ShiftModifier)?NavigationMode::BoxZoom:navigationMode_;
        if(activeNavigationMode_!=NavigationMode::Cursor){beginNavigation(event->localPos());event->accept();return;}
        dragging_ = true; queueCursor(event->localPos(), true); event->accept();
    } else QWidget::mousePressEvent(event);
}
void PlotCanvas::mouseMoveEvent(QMouseEvent* event) {
    if(navigationDragging_){moveNavigation(event->localPos(),false);event->accept();return;}
    if (dragging_) { queueCursor(event->localPos(), false); event->accept(); }
    else QWidget::mouseMoveEvent(event);
}
void PlotCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if(navigationDragging_&&event->button()==Qt::LeftButton){moveNavigation(event->localPos(),true);event->accept();return;}
    if (dragging_ && event->button() == Qt::LeftButton) {
        queueCursor(event->localPos(), true); dragging_ = false; event->accept();
    } else QWidget::mouseReleaseEvent(event);
}
void PlotCanvas::mouseDoubleClickEvent(QMouseEvent* event) {
    if(event->button()==Qt::LeftButton&&(plotRect_.contains(event->localPos())||yAxisContains(event->localPos()))){navigationDragging_=dragging_=false;restoreView();event->accept();}
    else QWidget::mouseDoubleClickEvent(event);
}
void PlotCanvas::editYRange() {
    QDialog dialog(window());dialog.setObjectName("plotAmplitudeDialog");dialog.setWindowTitle(QStringLiteral("纵轴量程 / %1").arg(yLabel_));dialog.setMinimumWidth(390);
    dialog.setStyleSheet("QLabel{font-size:16px;color:#263345;}QLineEdit{font-size:16px;font-weight:400;background:white;border:1px solid #8d9bad;border-radius:4px;padding:0 8px;color:#263345;}QPushButton{font-size:16px;font-weight:400;min-height:42px;border:1px solid #8d9bad;border-radius:5px;background:white;color:#263345;padding:0 10px;}QPushButton:pressed{background:#dbe6f5;}");
    auto* body=new QVBoxLayout(&dialog);auto* form=new QGridLayout;const auto range=yRange_.value_or(displayedYRange());
    auto* lower=new RangeLimitEdit(QString::number(range.x(),'g',12));auto* upper=new RangeLimitEdit(QString::number(range.y(),'g',12));lower->setObjectName("plotAmplitudeLower");upper->setObjectName("plotAmplitudeUpper");lower->setAccessibleName(QStringLiteral("纵轴下限"));upper->setAccessibleName(QStringLiteral("纵轴上限"));
    lower->setMinimumHeight(44);upper->setMinimumHeight(44);form->addWidget(new QLabel(QStringLiteral("下限 / %1").arg(yLabel_)),0,0);form->addWidget(lower,0,1);form->addWidget(new QLabel(QStringLiteral("上限 / %1").arg(yLabel_)),1,0);form->addWidget(upper,1,1);body->addLayout(form);
    if(zeroBasedY_&&!zeroCenteredY_){lower->setText(QStringLiteral("0"));lower->setReadOnly(true);}
    if(zeroCenteredY_){auto* note=new QLabel(QStringLiteral("振动幅值以 0 为中心，上下限按较大的绝对值对称设置。"));note->setWordWrap(true);body->addWidget(note);}
    auto* fixedNote=new QLabel(QStringLiteral("应用后锁定纵轴；采集、换通道及横轴操作均保持此范围。"));fixedNote->setWordWrap(true);body->addWidget(fixedNote);
    auto* error=new QLabel;error->setObjectName("plotAmplitudeError");error->setWordWrap(true);body->addWidget(error);
    auto* keypad=new QGridLayout;const QStringList keys{"7","8","9",QStringLiteral("退格"),"4","5","6",QStringLiteral("清空"),"1","2","3","-","0",".","e","+"};
    for(int i=0;i<keys.size();++i){auto* key=new QPushButton(keys[i]);key->setObjectName(QStringLiteral("plotAmplitudeKey%1").arg(i));key->setMinimumSize(64,44);key->setFocusPolicy(Qt::NoFocus);keypad->addWidget(key,i/4,i%4);connect(key,&QPushButton::clicked,&dialog,[=]{auto* field=qobject_cast<QLineEdit*>(QApplication::focusWidget());if(field!=lower&&field!=upper)field=lower->isReadOnly()?upper:lower;if(field->isReadOnly())field=upper;if(keys[i]==QStringLiteral("退格"))field->backspace();else if(keys[i]==QStringLiteral("清空"))field->clear();else if(field->text().size()<32)field->insert(keys[i]);error->clear();});}body->addLayout(keypad);
    auto* actions=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);actions->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用"));actions->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));body->addWidget(actions);
    auto* automatic=actions->addButton(QStringLiteral("自动量程"),QDialogButtonBox::ActionRole);automatic->setObjectName("plotAutoRange");connect(automatic,&QPushButton::clicked,&dialog,[&]{clearYRange();dialog.accept();});
    const auto apply=[&]{bool lowOk=false,highOk=false;const double low=lower->text().toDouble(&lowOk),high=upper->text().toDouble(&highOk);if(!lowOk||!highOk||!std::isfinite(low)||!std::isfinite(high)||high<=low||!std::isfinite(high-low)){error->setText(QStringLiteral("上下限须为有限数，且下限小于上限"));return;}if(!setFixedYRange(low,high)){error->setText(QStringLiteral("幅值范围超出数值上限"));return;}dialog.accept();};
    connect(actions,&QDialogButtonBox::accepted,&dialog,apply);connect(actions,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);connect(lower,&QLineEdit::returnPressed,&dialog,apply);connect(upper,&QLineEdit::returnPressed,&dialog,apply);auto* initial=lower->isReadOnly()?upper:lower;initial->setFocus();initial->selectAll();dialog.exec();
}
void PlotCanvas::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);menu.setObjectName("plotContextMenu");menu.setStyleSheet("QMenu{background:white;color:#344255;border:1px solid #ccd4df;}QMenu::item{min-height:44px;padding:0 22px;}QMenu::item:selected{background:#edf3fb;}");
    auto* automatic=menu.addAction(QStringLiteral("自动幅值"));automatic->setCheckable(true);automatic->setChecked(!yRange_);auto* precise=menu.addAction(QStringLiteral("精确幅值范围…"));auto* restore=menu.addAction(fixedYRange_?QStringLiteral("恢复完整横轴"):QStringLiteral("恢复全图"));QAction* latest=nullptr;if(liveActive_&&!liveFollowing_&&resumeLatestRequested){menu.addSeparator();latest=menu.addAction(QStringLiteral("回到最新"));}
    const auto* selected=menu.exec(event->globalPos());if(selected==automatic)clearYRange();else if(selected==precise)editYRange();else if(selected==restore)restoreView();else if(latest&&selected==latest)resumeLatestRequested();event->accept();
}
bool PlotCanvas::event(QEvent* event) {
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
        auto* touch = static_cast<QTouchEvent*>(event);
        struct Contact { int id; QPointF position; bool released; };
        QVector<Contact> contacts,active;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        for(const auto& point:touch->points())contacts.push_back({point.id(),point.position(),point.state()==QEventPoint::State::Released});
#else
        for(const auto& point:touch->touchPoints())contacts.push_back({point.id(),point.pos(),point.state()==Qt::TouchPointReleased});
#endif
        bool invalidContact=false;
        for(const auto& contact:contacts) {
            if(!std::isfinite(contact.position.x())||!std::isfinite(contact.position.y())){invalidContact=true;continue;}
            if(!contact.released)active.push_back(contact);
        }
        const auto findContact=[](const QVector<Contact>& list,int id)->const Contact* {
            for(const auto& contact:list)if(contact.id==id&&std::isfinite(contact.position.x())&&std::isfinite(contact.position.y()))return &contact;
            return nullptr;
        };
        if (event->type() == QEvent::TouchBegin) {
            const Contact* primary=nullptr;
            for(const auto& contact:active)if(plotRect_.contains(contact.position)||yAxisContains(contact.position)){primary=&contact;break;}
            if(!drawnData_||!primary)return QWidget::event(event);
            resetTouchInteraction();touchMode_=TouchMode::Single;touchPrimaryId_=primary->id;
            touchOrigin_=primary->position;touchTapEligible_=true;touchSequenceClock_.start();
            navigationY_=yAxisContains(touchOrigin_);activeNavigationMode_=navigationY_?NavigationMode::Pan:navigationMode_;
            // Defer any single-contact action until the entire begin event is
            // known: some touchscreens deliver both initial fingers together.
            if(active.size()<2) {
                touchMenuTimer_->start();
                if(activeNavigationMode_!=NavigationMode::Cursor)beginNavigation(touchOrigin_);
                else {dragging_=true;queueCursor(touchOrigin_,true);}
            }
        } else if(touchMode_==TouchMode::None)return QWidget::event(event);

        const bool final=event->type()==QEvent::TouchEnd||active.isEmpty();
        if(invalidContact) {
            // Malformed coordinates cannot become zoom factors or a tap. Wait
            // for all contacts to leave before accepting a fresh gesture.
            touchMode_=TouchMode::Suppressed;touchTapEligible_=touchFirstTapValid_=false;
            navigationDragging_=dragging_=false;touchMenuTimer_->stop();pendingCursor_.reset();cursorTimer_->stop();
        }
        if(touchMode_==TouchMode::Single&&active.size()>=2) {
            const auto* primary=findContact(active,touchPrimaryId_);
            const Contact* secondary=nullptr;
            for(const auto& contact:active)if(contact.id!=touchPrimaryId_){secondary=&contact;break;}
            if(primary&&secondary)beginPinch(secondary->id,primary->position,secondary->position);
            else {touchMode_=TouchMode::Suppressed;touchTapEligible_=touchFirstTapValid_=false;navigationDragging_=dragging_=false;touchMenuTimer_->stop();pendingCursor_.reset();cursorTimer_->stop();}
        } else if(touchMode_==TouchMode::Pinch&&!final) {
            const auto* first=findContact(active,touchPrimaryId_);const auto* second=findContact(active,touchSecondaryId_);
            if(first&&second)movePinch(first->position,second->position);
            else {
                // Never reinterpret the remaining finger (or a third finger)
                // as a fresh pan after either tracked pinch contact lifts.
                touchMode_=TouchMode::Suppressed;touchMenuTimer_->stop();
            }
        } else if(touchMode_==TouchMode::Single) {
            const auto* primary=findContact(final?contacts:active,touchPrimaryId_);
            if(!primary) {
                touchMode_=TouchMode::Suppressed;touchTapEligible_=touchFirstTapValid_=false;
                navigationDragging_=dragging_=false;touchMenuTimer_->stop();pendingCursor_.reset();cursorTimer_->stop();
            } else {
                const auto position=primary->position;
                if((position-touchOrigin_).manhattanLength()>=4) {
                    touchTapEligible_=touchFirstTapValid_=false;touchMenuTimer_->stop();
                }
                bool restored=false;
                if(final) {
                    touchMenuTimer_->stop();
                    const bool tap=touchTapEligible_&&!touchNavigationBegan_&&touchSequenceClock_.isValid()
                        &&touchSequenceClock_.elapsed()<=QApplication::doubleClickInterval();
                    const double tapRadius=std::max(24,2*QApplication::startDragDistance());
                    if(tap&&touchFirstTapValid_&&touchTapClock_.isValid()&&touchTapClock_.elapsed()<=QApplication::doubleClickInterval()
                        &&std::hypot(position.x()-touchFirstTapPosition_.x(),position.y()-touchFirstTapPosition_.y())<=tapRadius) {
                        navigationDragging_=dragging_=false;pendingCursor_.reset();cursorTimer_->stop();
                        touchFirstTapValid_=false;restoreView();restored=true;
                    } else if(tap) {touchFirstTapPosition_=position;touchTapClock_.start();touchFirstTapValid_=true;}
                    else touchFirstTapValid_=false;
                }
                if(!restored) {
                    if(navigationDragging_)moveNavigation(position,final);
                    else if(dragging_){queueCursor(position,final);if(final)dragging_=false;}
                }
            }
        }
        if(final)resetTouchInteraction();
        event->accept(); return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        resetTouchInteraction();touchFirstTapValid_=false;event->accept();return true;
    }
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) { axisGutter_ = 0; ++axesMetadataRevision_; invalidateStatic(); }
    return QWidget::event(event);
}

void PlotCanvas::paintEvent(QPaintEvent* event) {
    QElapsedTimer total; total.start();
    QElapsedTimer setup; if (profiling_) setup.start();
    const auto dpr = devicePixelRatioF();
    const QSize pixels(qCeil(width() * dpr), qCeil(height() * dpr));
    if (!directPaint_ && (staticLayer_.size() != pixels || !qFuzzyCompare(staticLayer_.devicePixelRatio(), dpr) || cachedFont_ != font())) {
        staticLayer_ = QImage(pixels, QImage::Format_RGB32);
        staticLayer_.setDevicePixelRatio(dpr);
        staticValidRegion_ = {}; staticDirty_ = true;
    }
    // A scroll viewport can expose only part of a row. Rasterize only that
    // actual region; retain its cache and fill newly exposed pixels on scroll.
    const QRegion visible = visibleRegion();
    // QWidget::grab/render may explicitly paint an otherwise hidden widget.
    const QRegion exposed = visible.isEmpty() ? event->region() : visible.intersected(event->region());
    const QRegion missing = exposed.subtracted(staticValidRegion_);
    QPainter p(this);
    // This clip also applies to cursor restoration. Every exposed pixel is
    // covered by opaque white decoration or an opaque axes image, so the
    // QWidget opaque-paint promise holds for both normal and redirected paint.
    p.setClipRegion(exposed);
    if (directPaint_ || staticDirty_ || !missing.isEmpty()) {
        renderStats_.lastAntialiasedTraceRuns = renderStats_.lastIntegerTraceRuns = 0;
        renderStats_.lastTraceVerticalTravelPixels = 0;
        if (profiling_) {
            renderStats_.lastSetupNs = setup.nsecsElapsed();
            renderStats_.lastBoundsNs = renderStats_.lastEnvelopeNs = renderStats_.lastMapNs = renderStats_.lastRasterNs = renderStats_.lastAxesNs = 0;
            renderStats_.lastEnvelopeBuilds = 0;
        }
        QElapsedTimer build; build.start();
        if (directPaint_) {
            // No second canvas-sized image or image-to-widget copy. A cursor
            // move redraws only its old/new exposed strips. Raw-data reduction
            // remains revision/range/physical-column cached in drawStatic.
            drawStatic(p);
            ++renderStats_.directPaints;
        } else {
            QPainter layer(&staticLayer_);
            layer.setClipRegion(exposed);
            drawStatic(layer);
            layer.end();
            staticValidRegion_ += exposed;
        }
        cachedFont_ = font(); staticDirty_ = false;
        ++renderStats_.staticBuilds; renderStats_.lastStaticNs = build.nsecsElapsed();
        if (profiling_) renderStats_.lastOtherNs = std::max(qint64(0), renderStats_.lastStaticNs - renderStats_.lastBoundsNs -
            renderStats_.lastEnvelopeNs - renderStats_.lastMapNs - renderStats_.lastRasterNs - renderStats_.lastAxesNs);
    }
    QElapsedTimer blit; if (profiling_) blit.start();
    if (!directPaint_) {
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(QPoint(0, 0), staticLayer_);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        ++renderStats_.cachedImageBlits;
    }
    if (!cursorStrip(cursor_).isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setClipRect(plotRect_.adjusted(-1, -1, 1, 1), Qt::IntersectClip);
        const auto x = plotRect_.left() + (*cursor_ - drawnXMin_) / (drawnXMax_ - drawnXMin_) * plotRect_.width();
        drawVerticalDash(p, x, plotRect_.top(), plotRect_.bottom());
    }
    if(navigationDragging_&&!navigationY_&&activeNavigationMode_==NavigationMode::BoxZoom) {
        const QRectF selection(QPointF(std::min(navigationOrigin_.x(),navigationCurrent_.x()),plotRect_.top()),QPointF(std::max(navigationOrigin_.x(),navigationCurrent_.x()),plotRect_.bottom()));
        p.setClipRect(plotRect_);p.fillRect(selection,QColor(53,105,200,30));p.setPen(QPen(QColor("#3569c8"),1,Qt::DashLine));p.setBrush(Qt::NoBrush);p.drawRect(selection);
    }
    p.end();
    if (profiling_) renderStats_.lastBlitNs = blit.nsecsElapsed();
    ++renderStats_.paints; renderStats_.lastPaintNs = total.nsecsElapsed();
}

void PlotCanvas::drawDecoration(QPainter& p) {
    p.fillRect(rect(), QColor("#ffffff"));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    QFont heading = font();
    heading.setPixelSize(16);
    heading.setWeight(QFont::DemiBold);
    p.setFont(heading);
    p.setPen(QColor("#343638"));
    if(!embedded_)p.drawText(QRect(compact_ ? 12 : 16, compact_ ? 3 : 10, width() - (compact_ ? 80 : 32), compact_ ? 22 : 24),
               Qt::AlignLeft | Qt::AlignVCenter, title_);
    QFont small = font();
    small.setPixelSize(14);
    p.setFont(small);

    int legendX = width() - 16;
    for (int i = series_.size() - 1; i >= 0; --i) {
        const auto& s = series_[i];
        if (!legendVisible_ || compact_ || s.name.isEmpty()) continue;
        const int textWidth = p.fontMetrics().horizontalAdvance(s.name);
        legendX -= textWidth;
        p.setPen(QColor("#767779"));
        p.drawText(QRect(legendX, 12, textWidth, 20), Qt::AlignVCenter, s.name);
        legendX -= 21;
        p.setPen(QPen(s.color, 2));
        p.drawLine(legendX, 22, legendX + 14, 22);
        legendX -= 15;
    }
}

void PlotCanvas::drawStatic(QPainter& p) {
    QElapsedTimer axes; if (profiling_) axes.start();
    QFont small = font(); small.setPixelSize(14); p.setFont(small);
    const QString fontKey = cacheAxisText_ ? small.key() : QString();
    const auto drawTickText = [&](QPainter& target, const QRectF& box, Qt::Alignment alignment, const QString& text) {
        if (cacheAxisText_) drawAxisText(target, box, alignment, axisText(small, fontKey, text));
        else target.drawText(box, alignment, text);
    };

    // Keep the top tick's text below the amplitude unit row, including CJK
    // fonts whose ascent is taller than the usual Latin UI font.
    const double left=embedded_?60:compact_?64:72,top=embedded_?12:compact_?44:66;
    const double bottom=xAxisVisible_?(embedded_?32:56):16;
    QRectF plot(left,top,std::max(1.,width()-left-20),std::max(1.,height()-top-bottom));
    // Keep a measured gutter once it grows. Returning to the nominal gutter
    // each frame would reduce the complete raw vectors twice for wider ticks.
    plot.setLeft(std::min(std::max(plot.left(), axisGutter_), std::max(plot.left(), plot.right() - 24)));
    plotRect_ = plot;
    double xMin = std::numeric_limits<double>::infinity();
    double xMax = -xMin;
    double yMin = xMin;
    double yMax = -xMin;
    bool haveData = false;
    if (profiling_) renderStats_.lastAxesNs += axes.nsecsElapsed();
    // An explicit live time interval is already known. Reduce once before
    // measuring bounds: its original min/max samples preserve the exact axis
    // extrema without rescanning every raw point a second time.
    if (hasXRange_) rebuildDisplayPoints(xMinimum_, xMaximum_, std::max(1, qCeil(plot.width() * devicePixelRatioF())));
    QElapsedTimer bounds; if (profiling_) bounds.start();
    for (int seriesIndex = 0; seriesIndex < series_.size(); ++seriesIndex) {
        const auto& s = series_[seriesIndex];
        if (!s.visible) continue;
        const auto& points = hasXRange_ ? displayPoints_[seriesIndex] : s.points;
        for (const auto& point : points) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) continue;
            if (hasXRange_ && (point.x() < xMinimum_ || point.x() > xMaximum_)) continue;
            haveData = true;
            xMin = std::min(xMin, point.x());
            xMax = std::max(xMax, point.x());
            yMin = std::min(yMin, point.y());
            yMax = std::max(yMax, point.y());
        }
    }
    if (hasXRange_) { xMin = xMinimum_; xMax = xMaximum_; }
    drawnData_ = haveData;
    if (profiling_) { renderStats_.lastBoundsNs += bounds.nsecsElapsed(); axes.restart(); }
    if (!haveData && !yRange_) {
        drawDecoration(p);
        yTickLabels_.clear();
        xTickLabels_.clear();
        displayPoints_.clear(); displayRevision_ = ~quint64(0); renderStats_.inputPoints = renderStats_.drawnPoints = 0;
        // A hidden overlay still owns its explicit time interval. Keep cursor
        // interaction available while all of its channel boxes are unchecked.
        if (hasXRange_ && !series_.isEmpty()) {
            drawnData_ = true; drawnXMin_ = xMin; drawnXMax_ = xMax;
        }
        p.setPen(QColor("#767779"));
        p.drawText(plot.adjusted(10, 0, -10, 0), Qt::AlignCenter | Qt::TextWordWrap,
                   emptyMessage_.isEmpty() ? QStringLiteral("暂无数据") : emptyMessage_);
        if (profiling_) renderStats_.lastAxesNs += axes.nsecsElapsed();
        return;
    }
    if(!haveData&&!hasXRange_){const auto domain=navigationBounds();xMin=domain.x();xMax=domain.y();}
    if (!(xMax > xMin)) { xMin -= 0.5; xMax += 0.5; }
    drawnXMin_ = xMin; drawnXMax_ = xMax;
    if (yRange_) {yMin=yRange_->x();yMax=yRange_->y();}
    else if (zeroCenteredY_) {
        double extent=std::max(std::abs(yMin),std::abs(yMax));
        if (!(extent>0)) extent=.001;
        else if(std::isfinite(extent*1.16))extent*=1.16;
        yMin=-extent;yMax=extent;
    } else if (zeroBasedY_ && yMin >= 0.0) yMin = 0.0;
    if (!yRange_ && !zeroCenteredY_ && !(yMax > yMin)) {
        const double pad = std::max(0.001, std::abs(yMin) * 0.1);
        yMin -= pad; yMax += pad;
        if (zeroBasedY_ && yMin < 0.0 && yMax > 0.0) yMin = 0.0;
    } else if (!yRange_ && !zeroCenteredY_) {
        const double pad = (yMax - yMin) * 0.08;
        yMax += pad;
        if (!(zeroBasedY_ && yMin == 0.0)) yMin -= pad;
    }
    drawnYMin_=yMin;drawnYMax_=yMax;

    // Scientific notation, minus signs and the selected UI font can require
    // more room than the nominal axis gutter. Keep full tick labels visible.
    int labelWidth = 0;
    yTickLabels_.clear();
    for (int tick = 0; tick <= 2; ++tick) {
        yTickLabels_ << (yRange_ ? QString::number(yMin+tick*.5*(yMax-yMin),'g',5) : axisNumber(yMin + tick * .5 * (yMax - yMin), yMax - yMin));
        labelWidth = std::max(labelWidth, cacheAxisText_ ? qCeil(axisText(small, fontKey, yTickLabels_.last()).size().width()) : p.fontMetrics().horizontalAdvance(yTickLabels_.last()));
    }
    plot.setLeft(std::min(std::max(plot.left(), double(labelWidth + 11)), std::max(plot.left(), plot.right() - 24)));
    axisGutter_ = plot.left();
    plotRect_ = plot;

    const int xTicks = compact_ ? 2 : 4;
    const auto tickValues = axisTicks(xMin, xMax, xTicks, xLabel_.contains(QStringLiteral("Hz")));
    QVector<double> xGrid; xTickLabels_.clear();
    for (double tick : tickValues) { xGrid.push_back(plot.left()+(tick-xMin)/(xMax-xMin)*plot.width()); xTickLabels_.push_back(axisNumber(tick,xMax-xMin)); }
    const auto drawAxesBackground = [&](QPainter& background) {
        drawDecoration(background); background.setPen(QColor("#344255"));
        if (!embedded_) background.drawText(compact_ ? QRectF(16, 6, width()-36, 24) : QRectF(16, 37, width() - 36, 24),
            compact_ ? Qt::AlignRight : Qt::AlignLeft, yLabel_);
        background.setPen(QPen(QColor("#ededed"), 1));
        for (double x : xGrid) {
            background.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }
        for (int tick = 0; tick <= 2; ++tick) {
            const double y = plot.bottom() - tick * .5 * plot.height();
            background.setPen(QPen(QColor("#ededed"), 1)); background.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            background.setPen(QColor("#344255"));
            drawTickText(background, QRectF(2, y - 12, plot.left() - 9, 24), Qt::AlignRight | Qt::AlignVCenter, yTickLabels_[tick]);
        }
        if (xAxisVisible_ && !embedded_) background.drawText(QRectF(plot.left(), height() - 24, plot.width(), 22), Qt::AlignRight, xLabel_);
    };
    if (!cacheAxisLayer_) { drawAxesBackground(p); ++renderStats_.axesBuilds; }
    else {
        const auto dpr = devicePixelRatioF();
        const QSize axesPixels(qCeil(width() * dpr), qCeil(height() * dpr));
        if (axesLayer_.size() != axesPixels || axesLayer_.devicePixelRatio() != dpr ||
            cachedAxesFont_ != font() || cachedAxesPlot_ != plot || cachedAxesLabels_ != yTickLabels_ || cachedAxesXGrid_ != xGrid || cachedAxesMetadataRevision_ != axesMetadataRevision_) {
            if (axesLayer_.size() != axesPixels || axesLayer_.devicePixelRatio() != dpr) {
                axesLayer_ = QImage(axesPixels, QImage::Format_RGB32); axesLayer_.setDevicePixelRatio(dpr);
            }
            QPainter background(&axesLayer_); drawAxesBackground(background); background.end();
            cachedAxesFont_ = font(); cachedAxesPlot_ = plot; cachedAxesLabels_ = yTickLabels_;
            cachedAxesXGrid_ = xGrid;
            cachedAxesMetadataRevision_ = axesMetadataRevision_; ++renderStats_.axesBuilds;
        } else ++renderStats_.axesCacheHits;
        p.save(); p.setCompositionMode(QPainter::CompositionMode_Source); p.drawImage(QPoint(0, 0), axesLayer_); p.restore();
    }
    p.setPen(QColor("#344255"));
    for (int tick = 0; tick < xGrid.size(); ++tick) {
        const double x = xGrid[tick]; const bool atRight = x >= plot.right() - 1;
        if (xAxisVisible_) drawTickText(p, QRectF(atRight?x-100:x-50, plot.bottom() + 5, 100, 24),
            atRight?Qt::AlignRight|Qt::AlignVCenter:Qt::AlignCenter, xTickLabels_[tick]);
    }

    p.save();
    p.setClipRect(plot.adjusted(-1, -1, 1, 1), Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& reference : references_) {
        if (reference.x() < xMin || reference.x() > xMax) continue;
        const auto x = plot.left() + (reference.x() - xMin) / (xMax - xMin) * plot.width();
        drawVerticalDash(p, x, plot.top(), plot.bottom());
        p.drawText(QPointF(x + 4, plot.top() + 13), QString::number(int(reference.y())) + "X");
    }
    // Cosmetic width is specified in physical pixels; scaling it by DPR keeps
    // the selected logical-pixel weight on normal and high-density displays.
    // Dense traces can cover most vertical pixels even after M4 reduction;
    // raster cost depends on segment length as well as point count. The
    // opt-in auto policy below leaves smooth/zoomed runs antialiased and uses
    // integer raster for dense high-travel runs. Both paths receive exactly
    // the same original-sample vertices and preserve each missing-run break.
    if (profiling_) renderStats_.lastAxesNs += axes.nsecsElapsed();
    rebuildDisplayPoints(xMin, xMax, std::max(1, qCeil(plot.width() * devicePixelRatioF())));
    renderStats_.drawnPoints = 0;
    for (int seriesIndex = 0; seriesIndex < series_.size(); ++seriesIndex) {
        const auto& s = series_[seriesIndex];
        if (!s.visible) continue;
        QElapsedTimer map; if (profiling_) map.start();
        const qint64 rasterBefore = renderStats_.lastRasterNs;
        renderStats_.drawnPoints += quint64(displayPoints_[seriesIndex].size());
        QPen tracePen(s.color, traceWidth_ * devicePixelRatioF(), Qt::SolidLine, Qt::RoundCap, Qt::BevelJoin);
        tracePen.setCosmetic(true); p.setPen(tracePen);
        QVector<QPointF> run; run.reserve(displayPoints_[seriesIndex].size());
        double verticalTravel = 0;
        const auto flush = [&] {
            if (run.isEmpty()) return;
            const double physicalWidth = std::max(1.0, plot.width() * devicePixelRatioF());
            const double physicalTravel = verticalTravel * devicePixelRatioF();
            // More than one vertex and sixteen vertical pixels of travel per
            // physical plot column identifies a filled/dense waveform rather
            // than a sparse curve. Count and travel are measured after the
            // unchanged M4 mapping, with no signal-dependent decimation.
            const bool dense = run.size() > physicalWidth && physicalTravel > physicalWidth * 16.0;
            const bool antialias = traceAntialias_ && !(adaptiveTraceAntialias_ && dense);
            p.setRenderHint(QPainter::Antialiasing, antialias);
            if (antialias) ++renderStats_.lastAntialiasedTraceRuns;
            else ++renderStats_.lastIntegerTraceRuns;
            renderStats_.lastTraceVerticalTravelPixels += physicalTravel;
            QElapsedTimer raster; if (profiling_) raster.start();
            if (run.size() > 1) p.drawPolyline(run.constData(), run.size());
            else if (run.size() == 1) p.drawPoint(run.first());
            if (profiling_) renderStats_.lastRasterNs += raster.nsecsElapsed();
            run.clear(); verticalTravel = 0;
        };
        const double xScale = plot.width() / (xMax - xMin), yScale = plot.height() / (yMax - yMin);
        for (const auto& point : displayPoints_[seriesIndex]) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
                point.x() < xMin || point.x() > xMax) {
                flush();
                continue;
            }
            const QPointF mapped(plot.left() + (point.x() - xMin) * xScale,
                                 plot.bottom() - (point.y() - yMin) * yScale);
            if (!run.isEmpty()) verticalTravel += std::abs(mapped.y() - run.last().y());
            run.push_back(mapped);
        }
        flush();
        if (profiling_) renderStats_.lastMapNs += std::max(qint64(0), map.nsecsElapsed() - (renderStats_.lastRasterNs - rasterBefore));
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.restore();
    if(!haveData){p.setPen(QColor("#767779"));p.drawText(plot,Qt::AlignCenter,QStringLiteral("当前窗口无有效数据"));}
}
} // namespace vibration::workbench

