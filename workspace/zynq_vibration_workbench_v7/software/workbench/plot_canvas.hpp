#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonObject>
#include <QFont>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <QRectF>
#include <QRegion>
#include <functional>
#include <optional>
class QTimer;
class QPainter;

namespace vibration::workbench {

struct PlotSeries {
    QString name;
    QColor color;
    QVector<QPointF> points;
    bool visible = true;
};

// The new workbench's neutral software renderer. Its sampling, cursor and cache
// behavior are independent of the legacy GUI. NaNs split missing trace spans.
// No Qt Charts, browser, font icon, or GPU is required.
class PlotCanvas final : public QWidget {
public:
    explicit PlotCanvas(QWidget* parent = nullptr);
    void setChart(QString title, QString xLabel, QString yLabel,
                  QVector<PlotSeries> series, QString emptyMessage = {});
    // GUI-thread live preview updates. These preserve chart metadata, explicit
    // ranges, references and cursor; point buffers are adopted by implicit
    // sharing/move. They perform no acquisition, FFT or signal filtering.
    void updateSeries(QVector<PlotSeries> series, bool preserveVisibility = true);
    bool appendPoints(int seriesIndex, const QVector<QPointF>& points, int maximumPoints);
    void setXRange(double minimum, double maximum);
    void clearXRange();
    bool setYRange(double minimum, double maximum);
    void clearYRange();
    // Fixed display limits survive data refresh and X navigation. Vertical
    // gestures cannot change them; only an explicit range/auto action can.
    bool setFixedYRange(double minimum, double maximum);
    bool fixedYRange() const { return fixedYRange_; }
    void editYRange();
    std::function<void()> yRangeChanged;
    std::optional<QPointF> explicitYRange() const { return yRange_; }
    void setNavigationBounds(double minimum, double maximum);
    void clearNavigationBounds();
    enum class NavigationMode { Cursor, Pan, BoxZoom };
    void setNavigationMode(NavigationMode mode);
    NavigationMode navigationMode() const { return navigationMode_; }
    bool zoomX(double factor, std::optional<double> anchor = std::nullopt);
    bool panX(double fraction);
    bool zoomY(double factor, std::optional<double> anchor = std::nullopt);
    QPointF displayedYRange() const { return QPointF(drawnYMin_, drawnYMax_); }
    void restoreView();
    std::function<void()> navigationStarted;
    std::function<void()> viewChanged;
    std::function<void()> resumeLatestRequested;
    void setLiveNavigationState(bool active, bool following) { liveActive_=active; liveFollowing_=following; }
    std::function<void(double, double)> xRangeRequested;
    QPointF navigationBounds() const;
    void setZeroBasedY(bool enabled);
    // Time-domain vibration viewport only: display symmetric limits around
    // zero without subtracting an offset or changing any source sample.
    // Explicit/manual limits and vertical gestures use the same zero anchor.
    void setZeroCenteredY(bool enabled);
    bool zeroCenteredY() const { return zeroCenteredY_; }
    void setCompact(bool enabled);
    void setEmbedded(bool enabled);
    void setXAxisVisible(bool visible);
    void setLegendVisible(bool visible);
    void setTraceWidth(double logicalPixels);
    double traceWidth() const { return traceWidth_; }
    QString axesMode() const { return axesMode_; }
    // Experimental widget raster path. Selected once at construction by the
    // exact opt-in AX7_PLOT_DIRECT_PAINT=1; the cached image path is default.
    bool directPaintEnabled() const { return directPaint_; }
    // Unset/1 keeps legacy AA; 0 disables trace AA; exact "auto" keeps AA for
    // sparse/smooth runs and uses integer raster for costly dense runs. This
    // changes pixel coverage only: samples, M4 vertices and NaN breaks stay.
    bool traceAntialiasEnabled() const { return traceAntialias_; }
    QString traceAntialiasMode() const {
        return adaptiveTraceAntialias_ ? QStringLiteral("auto") : traceAntialias_ ? QStringLiteral("on") : QStringLiteral("off");
    }
    bool sharedM4Enabled() const { return sharedM4_; }
    // GUI-thread snapshot. Global cache counts are process-wide: do not sum
    // them over canvases. per_canvas_* fields belong only to this widget.
    QJsonObject sharedM4Profile() const;
    void setChannelVisible(int index, bool visible);
    void setReferenceLines(QVector<QPointF> lines);
    const QVector<QPointF>& referenceLines() const { return references_; }
    void setCursor(std::optional<double> x);
    void selectAtX(double x);
    std::function<void(double)> cursorSelected;
    // Small counters make cache/paint costs observable without timing the
    // analysis worker, event-loop startup, or a screenshot encode.
    struct RenderStats {
        quint64 staticBuilds = 0, paints = 0;
        qint64 lastStaticNs = 0, lastPaintNs = 0;
        quint64 reductions = 0, inputPoints = 0, drawnPoints = 0;
        qint64 lastReductionNs = 0;
        // Optional AX7_CAPTURE_PROFILE=1 partition of the last static build.
        qint64 lastBoundsNs = 0, lastEnvelopeNs = 0, lastMapNs = 0, lastRasterNs = 0, lastAxesNs = 0;
        qint64 lastSetupNs = 0, lastBlitNs = 0, lastOtherNs = 0;
        quint32 lastEnvelopeBuilds = 0;
        quint64 axesBuilds = 0, axesCacheHits = 0;
        quint64 directPaints = 0, cachedImageBlits = 0;
        // Last static build, independent of AX7_CAPTURE_PROFILE. A run is a
        // finite path between missing intervals; no run is joined or filtered.
        quint64 lastAntialiasedTraceRuns = 0, lastIntegerTraceRuns = 0;
        double lastTraceVerticalTravelPixels = 0;
    };
    RenderStats renderStats() const { return renderStats_; }
    QRectF plotArea() const { return plotRect_; }
    const QVector<PlotSeries>& series() const { return series_; }
    std::optional<double> cursorX() const { return cursor_; }
    std::optional<QPointF> explicitXRange() const {
        return hasXRange_ ? std::optional<QPointF>(QPointF(xMinimum_, xMaximum_)) : std::nullopt;
    }
    bool xAxisVisible() const { return xAxisVisible_; }
    // Actual original-sample envelope used by the last static render. This is
    // read-only diagnostic data, not a replacement for raw analysis samples.
    QVector<QPointF> displayPoints(int seriesIndex) const;
    QStringList yTickLabels() const { return yTickLabels_; }
    QStringList xTickLabels() const { return xTickLabels_; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;

private:
    QString title_;
    QString xLabel_;
    QString yLabel_;
    QString emptyMessage_;
    QVector<PlotSeries> series_;
    bool hasXRange_ = false;
    bool zeroBasedY_ = false;
    bool zeroCenteredY_ = false;
    bool fixedYRange_ = false;
    bool compact_ = false;
    bool embedded_ = false;
    bool xAxisVisible_ = true;
    bool legendVisible_ = true;
    double traceWidth_ = 1.0;
    QString axesMode_ = QStringLiteral("plain");
    bool cacheAxisText_ = false, cacheAxisLayer_ = false;
    const bool directPaint_ = qgetenv("AX7_PLOT_DIRECT_PAINT") == QByteArrayLiteral("1");
    const bool traceAntialias_ = qgetenv("AX7_PLOT_TRACE_AA") != QByteArrayLiteral("0");
    const bool adaptiveTraceAntialias_ = qgetenv("AX7_PLOT_TRACE_AA") == QByteArrayLiteral("auto");
    const bool sharedM4_ = qgetenv("AX7_PLOT_SHARED_M4") == QByteArrayLiteral("1");
    quint64 sharedM4Hits_ = 0, sharedM4Misses_ = 0, sharedM4Bypasses_ = 0;
    double xMinimum_ = 0.0;
    double xMaximum_ = 1.0;
    QRectF plotRect_;
    double drawnXMin_ = 0, drawnXMax_ = 1;
    double drawnYMin_ = -1, drawnYMax_ = 1;
    bool drawnData_ = false;
    std::optional<double> cursor_;
    QVector<QPointF> references_;
    QImage staticLayer_;
    QImage axesLayer_;
    QRectF cachedAxesPlot_;
    QFont cachedAxesFont_;
    QStringList cachedAxesLabels_;
    QVector<double> cachedAxesXGrid_;
    quint64 axesMetadataRevision_ = 0, cachedAxesMetadataRevision_ = ~quint64(0);
    double axisGutter_ = 0;
    QRegion staticValidRegion_;
    QFont cachedFont_;
    bool staticDirty_ = true;
    bool dragging_ = false;
    NavigationMode navigationMode_ = NavigationMode::Pan, activeNavigationMode_ = NavigationMode::Pan;
    std::optional<QPointF> yRange_, navigationBounds_;
    QPointF navigationOrigin_, navigationCurrent_, navigationStartRange_;
    QPointF navigationStartYRange_;
    bool navigationDragging_ = false, navigationY_ = false, navigationBegan_ = false;
    bool liveActive_ = false, liveFollowing_ = true;
    QTimer* cursorTimer_ = nullptr;
    QTimer* touchMenuTimer_ = nullptr;
    QPointF touchOrigin_;
    bool touchMenuShown_ = false;
    enum class TouchMode { None, Single, Pinch, Suppressed };
    TouchMode touchMode_ = TouchMode::None;
    int touchPrimaryId_ = -1, touchSecondaryId_ = -1;
    bool touchNavigationBegan_ = false, touchTapEligible_ = false;
    bool touchFirstTapValid_ = false, touchPinchReady_ = false, touchPinchY_ = false;
    QElapsedTimer touchSequenceClock_, touchTapClock_;
    QPointF touchFirstTapPosition_, touchPinchRange_;
    QRectF touchPinchPlot_;
    double touchPinchDistance_ = 0, touchPinchAnchor_ = 0;
    std::optional<double> pendingCursor_;
    RenderStats renderStats_;
    const bool profiling_ = qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE") != 0;
    quint64 dataRevision_ = 0, displayRevision_ = ~quint64(0);
    int displayColumns_ = 0;
    double displayXMin_ = 0, displayXMax_ = 1;
    QVector<QVector<QPointF>> displayPoints_;
    QStringList yTickLabels_;
    QStringList xTickLabels_;
    void invalidateStatic();
    void rebuildDisplayPoints(double xMin, double xMax, int deviceColumns);
    void drawStatic(QPainter& painter);
    void drawDecoration(QPainter& painter);
    QRect cursorStrip(std::optional<double> x) const;
    void queueCursor(const QPointF& position, bool final);
    void flushCursor();
    bool requestRange(double minimum, double maximum);
    void beginNavigation(const QPointF& position);
    void moveNavigation(const QPointF& position, bool final);
    void resetTouchInteraction();
    void beginTouchNavigation();
    void beginPinch(int secondId, const QPointF& first, const QPointF& second);
    void movePinch(const QPointF& first, const QPointF& second);
    bool yAxisContains(const QPointF& position) const;
};

} // namespace vibration::workbench

