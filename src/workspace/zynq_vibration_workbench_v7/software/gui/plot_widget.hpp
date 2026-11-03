#pragma once

#include <QColor>
#include <QImage>
#include <QFont>
#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>
#include <QRectF>
#include <functional>
#include <optional>
class QTimer;
class QPainter;

namespace vibration::gui {

struct PlotSeries {
    QString name;
    QColor color;
    QVector<QPointF> points;
    bool visible = true;
};

// A small software-rendered chart. NaNs split curves instead of connecting
// through invalid samples. No Qt Charts, browser, or GPU is required.
class PlotWidget final : public QWidget {
public:
    explicit PlotWidget(QWidget* parent = nullptr);
    void setChart(QString title, QString xLabel, QString yLabel,
                  QVector<PlotSeries> series, QString emptyMessage = {});
    void setXRange(double minimum, double maximum);
    void clearXRange();
    void setZeroBasedY(bool enabled);
    void setCompact(bool enabled);
    void setEmbedded(bool enabled);
    void setXAxisVisible(bool visible);
    void setLegendVisible(bool visible);
    void setChannelVisible(int index, bool visible);
    void setReferenceLines(QVector<QPointF> lines);
    void setCursor(std::optional<double> x);
    void selectAtX(double x);
    std::function<void(double)> cursorSelected;
    // Small counters make cache/paint costs observable without timing the
    // analysis worker, event-loop startup, or a screenshot encode.
    struct RenderStats {
        quint64 staticBuilds = 0, paints = 0;
        qint64 lastStaticNs = 0, lastPaintNs = 0;
    };
    RenderStats renderStats() const { return renderStats_; }
    QRectF plotArea() const { return plotRect_; }
    const QVector<PlotSeries>& series() const { return series_; }
    std::optional<double> cursorX() const { return cursor_; }
    std::optional<QPointF> explicitXRange() const {
        return hasXRange_ ? std::optional<QPointF>(QPointF(xMinimum_, xMaximum_)) : std::nullopt;
    }
    bool xAxisVisible() const { return xAxisVisible_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    bool event(QEvent*) override;

private:
    QString title_;
    QString xLabel_;
    QString yLabel_;
    QString emptyMessage_;
    QVector<PlotSeries> series_;
    bool hasXRange_ = false;
    bool zeroBasedY_ = false;
    bool compact_ = false;
    bool embedded_ = false;
    bool xAxisVisible_ = true;
    bool legendVisible_ = true;
    double xMinimum_ = 0.0;
    double xMaximum_ = 1.0;
    QRectF plotRect_;
    double drawnXMin_ = 0, drawnXMax_ = 1;
    bool drawnData_ = false;
    std::optional<double> cursor_;
    QVector<QPointF> references_;
    QImage staticLayer_;
    QFont cachedFont_;
    bool staticDirty_ = true;
    bool dragging_ = false;
    QTimer* cursorTimer_ = nullptr;
    std::optional<double> pendingCursor_;
    RenderStats renderStats_;
    void invalidateStatic();
    void drawStatic(QPainter& painter);
    QRect cursorStrip(std::optional<double> x) const;
    void queueCursor(const QPointF& position, bool final);
    void flushCursor();
};

} // namespace vibration::gui
