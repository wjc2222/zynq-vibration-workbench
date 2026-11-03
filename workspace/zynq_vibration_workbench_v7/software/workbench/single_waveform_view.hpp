#pragma once
#include "plot_canvas.hpp"
#include <array>
class QAbstractButton;
class QLabel;
class QPushButton;
class QDialog;
namespace vibration::workbench {
// One plot owns presentation state; hidden channel samples stay in sources_.
class SingleWaveformView final : public QWidget {
public:
    // Spectrum uses the same canvas and channel selector without time-domain
    // statistics or speed controls; samples and navigation stay in PlotCanvas.
    explicit SingleWaveformView(QWidget* parent=nullptr,bool allowSpeed=true,const QString& xLabel=QStringLiteral("时间 / s"));
    PlotCanvas* plot() const { return plot_; }
    void setSourceSeries(const QVector<PlotSeries>&,const QString& unit="V");
    void setSpeedSeries(const PlotSeries&,const QString& unit="RPM");
    void setXRange(double first,double last){plot_->setXRange(first,last);}
    void setNavigationBounds(double first,double last){plot_->setNavigationBounds(first,last);}
    void setAmplitudeStats(const QVector<QPointF>& values,const QString& scope);
    void setSpeedSummary(const QString& summary,const QString& detail={});
    void setSpeedValue(std::optional<double> value,const QString& scope,bool rpmKnown=true);
    void clearData();
    // Data-dependent navigation may be unavailable before acquisition; the
    // user's Y-range editor remains available so limits can be set first.
    void setDataAvailable(bool available);
    // Presentation-only user limits. Waveforms normalize to +/- max(|a|,|b|),
    // spectra to 0..maximum. Unit profiles are stored separately in V7 settings.
    bool setFixedYRange(double minimum,double maximum);
    void setAutomaticYRange();
    void setChannelChecked(int channel,bool checked);
    void setSpeedSelected(bool selected);
    void setSpeedControlsVisible(bool visible);
    QVector<int> selectedChannels() const;
    bool showingSpeed() const {return speedSelected_;}
    const QVector<PlotSeries>& sourceSeries() const {return sources_;}
    const PlotSeries& speedSeries() const {return speed_;}
    std::function<void()> selectionChanged;
protected:
    void resizeEvent(QResizeEvent*) override;
private:
    PlotCanvas* plot_=nullptr;
    QWidget* sidebar_=nullptr;
    std::array<QAbstractButton*,3> choices_{};
    QAbstractButton* speedChoice_=nullptr;
    QLabel *unitLabel_=nullptr,*statsLabel_=nullptr;
    QPushButton* rangeButton_=nullptr;
    std::array<QLabel*,3> rmsFields_{},peakFields_{};
    QVector<PlotSeries> sources_;PlotSeries speed_;
    QString unit_="V",speedUnit_="RPM",chartUnit_,statsScope_,speedSummary_,speedDetail_;
    QVector<QPointF> stats_;
    std::array<bool,3> selected_{{true,false,false}},previous_{{true,false,false}};
    std::optional<QPointF> vibrationY_,speedY_;
    bool vibrationYFixed_=false,speedYFixed_=false,applyingRange_=false;
    std::optional<double> speedValue_;
    QString speedValueScope_;
    bool speedValueRpm_=true,speedSelected_=false,initialized_=false;
    const bool allowSpeed_;
    const QString xLabel_;
    void render(bool switched=false);
    void saveYRange();
    QString rangeProfile() const;
    void loadYRange();
    void persistYRange();
    void applyYRange();
    void updateRangeTools();
    void updateSidebar();
    void showSpeedValue();
};
}
