#pragma once
#include "plot_canvas.hpp"
#include <QScrollArea>
#include <memory>

namespace vibration::workbench {
// Four vibration views and one independent speed view share a time axis;
// rearranging or merging only changes
// their presentation. Channel IDs always refer to CH1, CH2, CH3 respectively.
class WaveformStack final : public QScrollArea {
public:
    explicit WaveformStack(QWidget* parent = nullptr);
    ~WaveformStack() override;
    PlotCanvas* plot(int stableId) const;
    void setSourceSeries(const QVector<PlotSeries>&, const QString& amplitudeUnit = "g");
    void setSpeedSeries(const PlotSeries&, const QString& unit = "RPM");
    void setAmplitudeStats(const QVector<QPointF>& rmsAndPeakToPeak, const QString& scope);
    void setSpeedSummary(const QString& summary, const QString& detail = {});
    void setXRange(double start, double end);
    void setNavigationBounds(double start, double end);
    bool setYRange(double minimum, double maximum);
    void clearYRange();
    void setNavigationMode(PlotCanvas::NavigationMode mode);
    void clearData();
    void resetLayout();
    QVector<int> panelOrder() const;
    QVector<int> panelChannels(int stableId) const;
    bool movePanel(int stableId, int destinationIndex);
    bool addChannel(int stableId, int channel);
    bool removeChannel(int stableId, int channel);
    bool moveChannel(int sourcePanel, int targetPanel, int channel);
    bool splitChannel(int sourcePanel, int channel);
    std::function<void(double)> cursorSelected;
    std::function<void()> navigationStarted;
    std::function<void(double,double)> xRangeRequested;
    std::function<void()> layoutChanged;
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
