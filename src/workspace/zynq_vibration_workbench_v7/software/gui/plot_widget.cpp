#include "plot_widget.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <QPaintEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vibration::gui {
namespace {
QString number(double value) {
    const double a = std::abs(value);
    if (a > 0.0 && (a < 0.001 || a >= 100000.0))
        return QString::number(value, 'g', 3);
    if (a >= 10.0) return QString::number(value, 'f', 0);
    if (a >= 1.0) return QString::number(value, 'f', 1);
    return QString::number(value, 'g', 2);
}
}

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(126);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_AcceptTouchEvents);
    cursorTimer_ = new QTimer(this);
    cursorTimer_->setSingleShot(true);
    cursorTimer_->setInterval(16);
    connect(cursorTimer_, &QTimer::timeout, this, [this] { flushCursor(); });
}

void PlotWidget::invalidateStatic() { staticDirty_ = true; update(); }

void PlotWidget::setChart(QString title, QString xLabel, QString yLabel,
                          QVector<PlotSeries> series, QString emptyMessage) {
    title_ = std::move(title);
    xLabel_ = std::move(xLabel);
    yLabel_ = std::move(yLabel);
    series_ = std::move(series);
    emptyMessage_ = std::move(emptyMessage);
    cursor_.reset();
    dragging_ = false;
    pendingCursor_.reset();
    cursorTimer_->stop();
    invalidateStatic();
}

void PlotWidget::setXRange(double minimum, double maximum) {
    const bool valid = std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum;
    if (hasXRange_ == valid && (!valid || (xMinimum_ == minimum && xMaximum_ == maximum))) return;
    hasXRange_ = valid;
    xMinimum_ = minimum;
    xMaximum_ = maximum;
    invalidateStatic();
}

void PlotWidget::clearXRange() { if (hasXRange_) { hasXRange_ = false; invalidateStatic(); } }
void PlotWidget::setZeroBasedY(bool enabled) { if (zeroBasedY_ != enabled) { zeroBasedY_ = enabled; invalidateStatic(); } }
void PlotWidget::setCompact(bool enabled) { if (compact_ != enabled) { compact_ = enabled; setMinimumHeight(enabled ? 90 : 126); invalidateStatic(); } }
void PlotWidget::setEmbedded(bool enabled) { if (embedded_ != enabled) { embedded_ = enabled; invalidateStatic(); } }
void PlotWidget::setXAxisVisible(bool visible) { if (xAxisVisible_ != visible) { xAxisVisible_ = visible; invalidateStatic(); } }
void PlotWidget::setLegendVisible(bool visible) { if (legendVisible_ != visible) { legendVisible_ = visible; invalidateStatic(); } }
void PlotWidget::setChannelVisible(int i, bool visible) {
    if (i >= 0 && i < series_.size() && series_[i].visible != visible) {
        series_[i].visible = visible;
        invalidateStatic();
    }
}
void PlotWidget::setReferenceLines(QVector<QPointF> lines) { if (references_ != lines) { references_ = std::move(lines); invalidateStatic(); } }
QRect PlotWidget::cursorStrip(std::optional<double> value) const {
    if (!value || !std::isfinite(*value) || !drawnData_ || *value < drawnXMin_ || *value > drawnXMax_) return {};
    const double x = plotRect_.left() + (*value - drawnXMin_) / (drawnXMax_ - drawnXMin_) * plotRect_.width();
    return QRect(qFloor(x) - 3, qFloor(plotRect_.top()) - 2, 7, qCeil(plotRect_.height()) + 5).intersected(rect());
}
void PlotWidget::setCursor(std::optional<double> x) {
    if (x && !std::isfinite(*x)) x.reset();
    if (cursor_ == x) return;
    const auto old = cursorStrip(cursor_);
    cursor_ = x;
    const auto next = cursorStrip(cursor_);
    if (!old.isEmpty()) update(old);
    if (!next.isEmpty()) update(next);
}
void PlotWidget::selectAtX(double x) { if (cursorSelected && drawnData_ && std::isfinite(x)) cursorSelected(std::clamp(x, drawnXMin_, drawnXMax_)); }
void PlotWidget::flushCursor() {
    if (!pendingCursor_) return;
    const double value = *pendingCursor_;
    pendingCursor_.reset();
    selectAtX(value);
}
void PlotWidget::queueCursor(const QPointF& position, bool final) {
    if (!drawnData_ || plotRect_.width() <= 0) return;
    const double fraction = std::clamp((position.x() - plotRect_.left()) / plotRect_.width(), 0.0, 1.0);
    pendingCursor_ = drawnXMin_ + fraction * (drawnXMax_ - drawnXMin_);
    if (final) { cursorTimer_->stop(); flushCursor(); }
    else if (!cursorTimer_->isActive()) cursorTimer_->start();
}
void PlotWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && drawnData_ && plotRect_.contains(event->localPos())) {
        dragging_ = true; queueCursor(event->localPos(), true); event->accept();
    } else QWidget::mousePressEvent(event);
}
void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) { queueCursor(event->localPos(), false); event->accept(); }
    else QWidget::mouseMoveEvent(event);
}
void PlotWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && event->button() == Qt::LeftButton) {
        queueCursor(event->localPos(), true); dragging_ = false; event->accept();
    } else QWidget::mouseReleaseEvent(event);
}
bool PlotWidget::event(QEvent* event) {
    if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
        auto* touch = static_cast<QTouchEvent*>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (touch->points().isEmpty()) return QWidget::event(event);
        const auto position = touch->points().first().position();
#else
        if (touch->touchPoints().isEmpty()) return QWidget::event(event);
        const auto position = touch->touchPoints().first().pos();
#endif
        if (event->type() == QEvent::TouchBegin) {
            if (!drawnData_ || !plotRect_.contains(position)) return QWidget::event(event);
            dragging_ = true; queueCursor(position, true);
        } else if (dragging_) {
            const bool final = event->type() == QEvent::TouchEnd;
            queueCursor(position, final);
            if (final) dragging_ = false;
        } else return QWidget::event(event);
        event->accept(); return true;
    }
    if (event->type() == QEvent::TouchCancel) {
        dragging_ = false; pendingCursor_.reset(); cursorTimer_->stop(); event->accept(); return true;
    }
    if (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) invalidateStatic();
    return QWidget::event(event);
}

void PlotWidget::paintEvent(QPaintEvent*) {
    QElapsedTimer total; total.start();
    const auto dpr = devicePixelRatioF();
    const QSize pixels(qCeil(width() * dpr), qCeil(height() * dpr));
    if (staticDirty_ || staticLayer_.size() != pixels || !qFuzzyCompare(staticLayer_.devicePixelRatio(), dpr) || cachedFont_ != font()) {
        QElapsedTimer build; build.start();
        staticLayer_ = QImage(pixels, QImage::Format_RGB32);
        staticLayer_.setDevicePixelRatio(dpr);
        QPainter layer(&staticLayer_);
        drawStatic(layer);
        layer.end();
        cachedFont_ = font(); staticDirty_ = false;
        ++renderStats_.staticBuilds; renderStats_.lastStaticNs = build.nsecsElapsed();
    }
    QPainter p(this);
    p.drawImage(QPoint(0, 0), staticLayer_);
    if (!cursorStrip(cursor_).isEmpty()) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setClipRect(plotRect_.adjusted(-1, -1, 1, 1));
        const auto x = plotRect_.left() + (*cursor_ - drawnXMin_) / (drawnXMax_ - drawnXMin_) * plotRect_.width();
        p.setPen(QPen(QColor("#73859c"), 1, Qt::DashLine));
        p.drawLine(QPointF(x, plotRect_.top()), QPointF(x, plotRect_.bottom()));
    }
    p.end();
    ++renderStats_.paints; renderStats_.lastPaintNs = total.nsecsElapsed();
}

void PlotWidget::drawStatic(QPainter& p) {
    p.fillRect(rect(), QColor("#ffffff"));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#ffffff"));
    p.drawRoundedRect(QRectF(rect()), compact_ ? 0 : 8, compact_ ? 0 : 8);
    p.setBrush(Qt::NoBrush);
    QFont heading = font();
    heading.setPixelSize(14);
    heading.setWeight(QFont::DemiBold);
    p.setFont(heading);
    p.setPen(QColor("#1d2b3b"));
    if(!embedded_)p.drawText(QRect(compact_ ? 12 : 16, compact_ ? 3 : 10, width() - (compact_ ? 80 : 32), compact_ ? 22 : 24),
               Qt::AlignLeft | Qt::AlignVCenter, title_);
    QFont small = font();
    small.setPixelSize(11);
    p.setFont(small);

    int legendX = width() - 16;
    for (int i = series_.size() - 1; i >= 0; --i) {
        const auto& s = series_[i];
        if (!legendVisible_ || compact_ || s.name.isEmpty()) continue;
        const int textWidth = p.fontMetrics().horizontalAdvance(s.name);
        legendX -= textWidth;
        p.setPen(QColor("#89959d"));
        p.drawText(QRect(legendX, 12, textWidth, 20), Qt::AlignVCenter, s.name);
        legendX -= 21;
        p.setPen(QPen(s.color, 2));
        p.drawLine(legendX, 22, legendX + 14, 22);
        legendX -= 15;
    }

    // Keep the top tick's text below the amplitude unit row, including CJK
    // fonts whose ascent is taller than the usual Latin UI font.
    const QRectF plot(embedded_ ? 44 : compact_ ? 52 : 58, embedded_ ? 8 : compact_ ? 36 : 61,
                      std::max(1, width() - (embedded_ ? 58 : compact_ ? 66 : 76)),
                      std::max(1, height() - (embedded_ ? (xAxisVisible_ ? 34 : 16) : compact_ ? (xAxisVisible_ ? 70 : 45) : (xAxisVisible_ ? 95 : 70))));
    plotRect_ = plot;
    double xMin = std::numeric_limits<double>::infinity();
    double xMax = -xMin;
    double yMin = xMin;
    double yMax = -xMin;
    bool haveData = false;
    for (const auto& s : series_) {
        if (!s.visible) continue;
        for (const auto& point : s.points) {
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
    if (!haveData) {
        // A hidden overlay still owns its explicit time interval. Keep cursor
        // interaction available while all of its channel boxes are unchecked.
        if (hasXRange_ && !series_.isEmpty()) {
            drawnData_ = true; drawnXMin_ = xMin; drawnXMax_ = xMax;
        }
        p.setPen(QColor("#78899c"));
        p.drawText(plot.adjusted(10, 0, -10, 0), Qt::AlignCenter | Qt::TextWordWrap,
                   emptyMessage_.isEmpty() ? QStringLiteral("暂无数据") : emptyMessage_);
        return;
    }
    if (!(xMax > xMin)) { xMin -= 0.5; xMax += 0.5; }
    drawnXMin_ = xMin; drawnXMax_ = xMax;
    if (zeroBasedY_ && yMin >= 0.0) yMin = 0.0;
    if (!(yMax > yMin)) {
        const double pad = std::max(0.001, std::abs(yMin) * 0.1);
        yMin -= pad; yMax += pad;
        if (zeroBasedY_ && yMin < 0.0 && yMax > 0.0) yMin = 0.0;
    } else {
        const double pad = (yMax - yMin) * 0.08;
        yMax += pad;
        if (!(zeroBasedY_ && yMin == 0.0)) yMin -= pad;
    }

    p.setPen(QColor("#78899c"));
    if(!embedded_)p.drawText(compact_ ? QRectF(width() - 68, 7, 56, 15) : QRectF(16, 37, width() - 16, 15),
               compact_ ? Qt::AlignRight : Qt::AlignLeft, yLabel_);
    const int xTicks = compact_ ? 2 : 4;
    for (int tick = 0; tick <= xTicks; ++tick) {
        const double fraction = double(tick) / xTicks;
        const double x = plot.left() + fraction * plot.width();
        p.setPen(QPen(QColor("#e9eef4"), 1));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(QColor("#78899c"));
        const double tickValue=xMin + fraction * (xMax - xMin);
        const bool narrow=(xMax-xMin)<std::max(std::abs(xMin),std::abs(xMax))*.2;
        const int places=std::clamp(int(std::ceil(-std::log10((xMax-xMin)/xTicks)))+1,0,8);
        if (xAxisVisible_) p.drawText(QRectF(tick==xTicks?x-72:x-36, plot.bottom() + 3, 72, 17),
                                    tick==xTicks?Qt::AlignRight|Qt::AlignVCenter:Qt::AlignCenter,
                                    narrow?QString::number(tickValue,'f',places):number(tickValue));
    }
    for (int tick = 0; tick <= 2; ++tick) {
        const double fraction = tick / 2.0;
        const double y = plot.bottom() - fraction * plot.height();
        p.setPen(QPen(QColor("#e9eef4"), 1));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(QColor("#78899c"));
        p.drawText(QRectF(2, y - 8, plot.left() - 9, 16), Qt::AlignRight | Qt::AlignVCenter,
                   number(yMin + fraction * (yMax - yMin)));
    }
    if (xAxisVisible_&&!embedded_) p.drawText(QRectF(plot.left(), height() - 15, plot.width(), 14),
                                 Qt::AlignRight, xLabel_);

    p.save();
    p.setClipRect(plot.adjusted(-1, -1, 1, 1));
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const auto& reference : references_) {
        if (reference.x() < xMin || reference.x() > xMax) continue;
        const auto x = plot.left() + (reference.x() - xMin) / (xMax - xMin) * plot.width();
        p.setPen(QPen(QColor("#91a4be"), 1, Qt::DashLine));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.drawText(QPointF(x + 4, plot.top() + 13), QString::number(int(reference.y())) + "X");
    }
    // Dense traces use the raster engine's one-pixel cosmetic stroke on ARM.
    // Keep antialiasing for the surrounding labels, references and cursor.
    p.setRenderHint(QPainter::Antialiasing, false);
    for (const auto& s : series_) {
        if (!s.visible) continue;
        QPainterPath path;
        bool started = false;
        for (const auto& point : s.points) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()) ||
                point.x() < xMin || point.x() > xMax) {
                started = false;
                continue;
            }
            const QPointF q(plot.left() + (point.x() - xMin) / (xMax - xMin) * plot.width(),
                            plot.bottom() - (point.y() - yMin) / (yMax - yMin) * plot.height());
            if (started) path.lineTo(q); else { path.moveTo(q); started = true; }
        }
        QPen tracePen(s.color, 1);
        tracePen.setCosmetic(true);
        p.setPen(tracePen);
        p.drawPath(path);
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.restore();
}
} // namespace vibration::gui
