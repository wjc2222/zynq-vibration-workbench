#include "single_waveform_view.hpp"
#include "controls.hpp"
#include <QAbstractButton>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSettings>
#include <QScrollArea>
#include <QResizeEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <cmath>
#include <algorithm>
#include <limits>
namespace vibration::workbench {
namespace {
QString value(double number,int precision=4){return std::isfinite(number)?QString::number(number,'g',precision):QStringLiteral("—");}
class SelectorCheck final:public QCheckBox {
public:explicit SelectorCheck(const QString& text):QCheckBox(text){setMinimumHeight(44);setStyleSheet("font-size:16px;color:#263345;");}
protected:
    bool hitButton(const QPoint& point)const override{return rect().contains(point);}
    void paintEvent(QPaintEvent*)override{
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);const double top=(height()-20)*.5;
        const QColor accent=isEnabled()?QColor("#3569c8"):QColor("#a0aab8");
        painter.setPen(QPen(isChecked()?accent:QColor("#8c98a8"),1.3));painter.setBrush(isChecked()?accent:QColor("white"));painter.drawRoundedRect(QRectF(1,top,20,20),3,3);
        if(isChecked()){painter.setPen(QPen(QColor("white"),2.2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.drawLine(QPointF(5,top+10),QPointF(9,top+14));painter.drawLine(QPointF(9,top+14),QPointF(17,top+6));}
        painter.setPen(isEnabled()?QColor("#263345"):QColor("#687586"));painter.setFont(font());painter.drawText(QRectF(30,0,std::max(1,width()-30),height()),Qt::AlignLeft|Qt::AlignVCenter,text());
        if(hasFocus()){painter.setPen(QPen(accent,1,Qt::DotLine));painter.setBrush(Qt::NoBrush);painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),3,3);}
    }
};
}
SingleWaveformView::SingleWaveformView(QWidget* parent,bool allowSpeed,const QString& xLabel):QWidget(parent),allowSpeed_(allowSpeed),xLabel_(xLabel){
    setObjectName(allowSpeed_?"singleWaveformView":"singleSpectrumView");auto* body=new QHBoxLayout(this);body->setContentsMargins(0,0,0,0);body->setSpacing(0);
    auto* graph=new QWidget;auto* graphLayout=new QVBoxLayout(graph);graphLayout->setContentsMargins(0,0,0,0);graphLayout->setSpacing(2);
    // AnalysisPane adds live follow/window controls to this shared top row.
    auto* rangeTools=new QWidget;rangeTools->setObjectName("plotRangeTools");auto* rangeRow=new QHBoxLayout(rangeTools);rangeRow->setContentsMargins(0,0,0,0);rangeRow->setSpacing(8);graphLayout->addWidget(rangeTools);
    rangeButton_=new TouchButton(QStringLiteral("纵轴量程…"));rangeButton_->setObjectName("plotSetRange");rangeButton_->setAccessibleName(QStringLiteral("设置纵轴量程"));rangeButton_->setMinimumHeight(44);
    plot_=new PlotCanvas;plot_->setObjectName(allowSpeed_?"singleWaveCanvas":"workbenchSpectrum");plot_->setCompact(true);plot_->setLegendVisible(false);plot_->setMinimumHeight(160);plot_->setZeroBasedY(!allowSpeed_);graphLayout->addWidget(plot_,1);body->addWidget(graph,1);
    connect(rangeButton_,&QPushButton::clicked,this,[this]{plot_->editYRange();});
    plot_->yRangeChanged=[this]{if(applyingRange_)return;const bool wasFixed=vibrationYFixed_;saveYRange();if(!speedSelected_&&(wasFixed||vibrationYFixed_))persistYRange();updateRangeTools();};
    loadYRange();
    auto* sidebar=new QFrame;sidebar->setObjectName("measurementChannelSidebar");sidebar->setStyleSheet("QFrame#measurementChannelSidebar{background:white;border:0;border-left:1px solid #d6dce4;}QLabel{background:transparent;font-size:16px;color:#263345;}");
    auto* controls=new QVBoxLayout(sidebar);controls->setContentsMargins(10,8,10,8);controls->setSpacing(6);
    unitLabel_=new QLabel;unitLabel_->setObjectName(allowSpeed_?"singleWaveUnit":"singleSpectrumUnit");unitLabel_->setWordWrap(true);unitLabel_->setStyleSheet("font-size:16px;font-weight:600;color:#263345;");controls->addWidget(unitLabel_);
    auto* matrix=new QGridLayout;matrix->setContentsMargins(0,0,0,0);matrix->setHorizontalSpacing(6);matrix->setVerticalSpacing(6);
    const QStringList headers{QStringLiteral("通道"),QStringLiteral("RMS"),QStringLiteral("峰峰值")};if(allowSpeed_)for(int column=0;column<3;++column){auto* heading=new QLabel(headers[column]);heading->setAlignment(column?Qt::AlignCenter:Qt::AlignLeft|Qt::AlignVCenter);heading->setStyleSheet("font-size:16px;color:#263345;");matrix->addWidget(heading,0,column);}
    const QStringList colors{"#3769c7","#268878","#b78032"};
    for(int channel=0;channel<3;++channel){auto* cell=new QWidget;auto* row=new QHBoxLayout(cell);row->setContentsMargins(0,0,0,0);row->setSpacing(4);auto* choice=new SelectorCheck(QStringLiteral("CH%1").arg(channel+1));choices_[channel]=choice;choice->setObjectName((allowSpeed_?QStringLiteral("channelSelect%1"):QStringLiteral("spectrumChannel%1")).arg(channel+1));choice->setChecked(selected_[channel]);row->addWidget(choice,1);auto* color=new QFrame;color->setObjectName((allowSpeed_?QStringLiteral("channelColor%1"):QStringLiteral("spectrumColor%1")).arg(channel+1));color->setFixedSize(8,20);color->setStyleSheet(QStringLiteral("background:%1;border:0;border-radius:2px;").arg(colors[channel]));row->addWidget(color);matrix->addWidget(cell,channel+1,0);connect(choice,&QAbstractButton::toggled,this,[this,channel](bool checked){setChannelChecked(channel,checked);});
        if(allowSpeed_)for(int measure=0;measure<2;++measure){auto* field=new QLabel(QStringLiteral("—"));field->setObjectName(QStringLiteral("single%1%2").arg(measure?QStringLiteral("Peak"):QStringLiteral("Rms")).arg(channel+1));field->setMinimumHeight(44);field->setMinimumWidth(56);field->setAlignment(Qt::AlignCenter);field->setStyleSheet("font-size:16px;color:#263345;background:#f8fafc;border:1px solid #aeb7c3;border-radius:4px;padding:0 3px;");matrix->addWidget(field,channel+1,measure+1);if(measure)peakFields_[channel]=field;else rmsFields_[channel]=field;}
    }
    matrix->setColumnMinimumWidth(0,80);matrix->setColumnStretch(0,1);controls->addLayout(matrix);controls->addSpacing(6);
    if(allowSpeed_){speedChoice_=new SelectorCheck(QStringLiteral("转速波形"));speedChoice_->setObjectName("speedSelect");controls->addWidget(speedChoice_);connect(speedChoice_,&QAbstractButton::toggled,this,[this](bool checked){setSpeedSelected(checked);});
        auto* speedValue=new QPushButton(QStringLiteral("转速数值"));speedValue->setObjectName("speedValueButton");speedValue->setMinimumHeight(44);speedValue->setStyleSheet("QPushButton{font-size:16px;min-height:44px;padding:0 12px;border:1px solid #8d9bad;border-radius:5px;background:white;color:#263345;}QPushButton:hover{background:#f0f4fa;}QPushButton:pressed{background:#dbe6f5;}");controls->addWidget(speedValue);connect(speedValue,&QPushButton::clicked,this,[this]{showSpeedValue();});}
    controls->addWidget(rangeButton_);statsLabel_=new QLabel(sidebar);statsLabel_->setObjectName(allowSpeed_?"singleWaveStats":"singleSpectrumStats");statsLabel_->hide();controls->addStretch();auto* sideScroll=new QScrollArea;sidebar_=sideScroll;sideScroll->setObjectName(allowSpeed_?"waveSelectorScroll":"spectrumSelectorScroll");sideScroll->setFrameShape(QFrame::NoFrame);sideScroll->setFixedWidth(allowSpeed_?240:180);sideScroll->setWidgetResizable(true);sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);sideScroll->setWidget(sidebar);body->addWidget(sideScroll);render(true);
}
void SingleWaveformView::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);sidebar_->setFixedWidth(allowSpeed_?(width()<1100?240:260):180);}
void SingleWaveformView::saveYRange(){if(speedSelected_){speedY_=plot_->explicitYRange();speedYFixed_=plot_->fixedYRange();}else{vibrationY_=plot_->explicitYRange();vibrationYFixed_=plot_->fixedYRange();}}
QString SingleWaveformView::rangeProfile()const{
    const bool acceleration=unit_==QStringLiteral("m/s²")||unit_==QStringLiteral("m/s^2");
    const QString view=allowSpeed_?(unit_==QStringLiteral("V")?QStringLiteral("timeVoltage"):acceleration?QStringLiteral("acceleration"):QStringLiteral("timeOther")):QStringLiteral("spectrum");
    return QStringLiteral("plotRanges/%1/%2").arg(view,acceleration?QStringLiteral("m_s2"):unit_);
}
void SingleWaveformView::loadYRange(){
    QSettings settings(QSettings::IniFormat,QSettings::UserScope,QStringLiteral("VibrationWorkbench"),QStringLiteral("WorkbenchV7"));settings.beginGroup(rangeProfile());
    const double low=settings.value("minimum").toDouble(),high=settings.value("maximum").toDouble();
    vibrationYFixed_=settings.value("fixed",false).toBool()&&std::isfinite(low)&&std::isfinite(high)&&high>low&&std::isfinite(high-low)&&(!allowSpeed_?low==0:low==-high);
    vibrationY_=vibrationYFixed_?std::optional<QPointF>(QPointF(low,high)):std::nullopt;
}
void SingleWaveformView::persistYRange(){
    QSettings settings(QSettings::IniFormat,QSettings::UserScope,QStringLiteral("VibrationWorkbench"),QStringLiteral("WorkbenchV7"));settings.beginGroup(rangeProfile());settings.setValue("fixed",vibrationYFixed_);
    if(vibrationYFixed_&&vibrationY_){settings.setValue("minimum",vibrationY_->x());settings.setValue("maximum",vibrationY_->y());}else{settings.remove("minimum");settings.remove("maximum");}
    settings.sync();
}
void SingleWaveformView::applyYRange(){
    const auto range=speedSelected_?speedY_:vibrationY_;const bool fixed=speedSelected_?speedYFixed_:vibrationYFixed_;
    plot_->clearYRange();if(range){if(fixed)plot_->setFixedYRange(range->x(),range->y());else plot_->setYRange(range->x(),range->y());}
}
bool SingleWaveformView::setFixedYRange(double minimum,double maximum){
    if(!allowSpeed_&&!speedSelected_){if(!std::isfinite(minimum)||!std::isfinite(maximum)||minimum<0||maximum<=minimum)return false;minimum=0;}
    return plot_->setFixedYRange(minimum,maximum);
}
void SingleWaveformView::setAutomaticYRange(){plot_->clearYRange();}
void SingleWaveformView::updateRangeTools(){
    const auto range=plot_->explicitYRange();QString unit=speedSelected_?speedUnit_:unit_;if(unit==QStringLiteral("m/s^2"))unit=QStringLiteral("m/s²");
    const QString text=plot_->fixedYRange()&&range?QStringLiteral("纵轴：固定 %1…%2 %3").arg(value(range->x(),5),value(range->y(),5),unit):range?QStringLiteral("纵轴：手动 %1…%2 %3").arg(value(range->x(),5),value(range->y(),5),unit):QStringLiteral("纵轴：自动 / %1").arg(unit);
    rangeButton_->setText(plot_->fixedYRange()&&range?(range->x()==-range->y()?QStringLiteral("纵轴：±%1 %2").arg(value(range->y(),4),unit):QStringLiteral("纵轴：0…%1 %2").arg(value(range->y(),4),unit)):range?QStringLiteral("纵轴：手动…"):QStringLiteral("纵轴量程…"));rangeButton_->setToolTip(text+QStringLiteral("\n固定纵轴时，横轴仍可拖动和缩放。"));
}
void SingleWaveformView::render(bool switched){
    if(!switched)saveYRange();const bool previousApplying=applyingRange_;applyingRange_=true;QVector<PlotSeries> lines;
    if(speedSelected_){if(!speed_.points.isEmpty())lines.push_back(speed_);}else for(int channel=0;channel<3;++channel)if(selected_[channel]&&channel<sources_.size())lines.push_back(sources_[channel]);
    const auto unit=speedSelected_?speedUnit_:unit_;const auto cursor=plot_->cursorX();
    plot_->setZeroCenteredY(allowSpeed_&&!speedSelected_);
    if(!initialized_||chartUnit_!=unit||switched){plot_->setChart({},xLabel_,unit,lines,speedSelected_?QStringLiteral("暂无有效测速"):selectedChannels().isEmpty()?QStringLiteral("请在右侧选择通道"):QStringLiteral("等待数据"));chartUnit_=unit;initialized_=true;applyYRange();plot_->setCursor(cursor);}
    else {plot_->updateSeries(std::move(lines),false);if(selectedChannels().isEmpty()&&!speedSelected_)plot_->setChart({},xLabel_,unit,{},QStringLiteral("请在右侧选择通道"));}
    applyingRange_=previousApplying;updateSidebar();updateRangeTools();
}
void SingleWaveformView::setSourceSeries(const QVector<PlotSeries>& series,const QString& unit){
    const bool unitChanged=unit_!=unit;if(unitChanged){saveYRange();unit_=unit;loadYRange();}sources_=series;
    if(!allowSpeed_){
        // A channel selection changes visibility, never the available frequency
        // domain. In particular, an empty selection must still be navigable.
        double first=std::numeric_limits<double>::infinity(),last=-first;
        for(const auto& line:sources_)for(const auto& point:line.points)if(std::isfinite(point.x())){first=std::min(first,point.x());last=std::max(last,point.x());}
        if(std::isfinite(first)&&last>first)plot_->setNavigationBounds(first,last);else plot_->clearNavigationBounds();
    }
    render(unitChanged&&!speedSelected_);
}
void SingleWaveformView::setSpeedSeries(const PlotSeries& series,const QString& unit){const bool unitChanged=speedUnit_!=unit;if(unitChanged){saveYRange();speedY_.reset();speedYFixed_=false;}speed_=series;speedUnit_=unit;render(unitChanged&&speedSelected_);}
void SingleWaveformView::setAmplitudeStats(const QVector<QPointF>& values,const QString& scope){stats_=values;statsScope_=scope;updateSidebar();}
void SingleWaveformView::setSpeedSummary(const QString& summary,const QString& detail){speedSummary_=summary;speedDetail_=detail;updateSidebar();}
void SingleWaveformView::setSpeedValue(std::optional<double> mean,const QString& scope,bool rpmKnown){speedValue_=mean&&std::isfinite(*mean)?mean:std::nullopt;speedValueScope_=scope;speedValueRpm_=rpmKnown;}
QVector<int> SingleWaveformView::selectedChannels()const{QVector<int> channels;if(!speedSelected_)for(int channel=0;channel<3;++channel)if(selected_[channel])channels.push_back(channel);return channels;}
void SingleWaveformView::setChannelChecked(int channel,bool checked){
    if(channel<0||channel>=3)return;const bool switched=speedSelected_&&checked;saveYRange();if(switched){speedSelected_=false;selected_={false,false,false};}selected_[channel]=checked;render(switched);if(selectionChanged)selectionChanged();
}
void SingleWaveformView::setSpeedControlsVisible(bool visible){
    if(!allowSpeed_)return;
    if(!visible)setSpeedSelected(false);
    if(speedChoice_)speedChoice_->setVisible(visible);
    if(auto* button=findChild<QPushButton*>("speedValueButton"))button->setVisible(visible);
}
void SingleWaveformView::setSpeedSelected(bool selected){
    if(!allowSpeed_)return;
    if(speedSelected_==selected)return;saveYRange();if(selected){previous_=selected_;selected_={false,false,false};}else selected_=previous_;speedSelected_=selected;render(true);if(selectionChanged)selectionChanged();
}
void SingleWaveformView::updateSidebar(){
    for(int channel=0;channel<3;++channel){const QSignalBlocker blocked(choices_[channel]);choices_[channel]->setChecked(!speedSelected_&&selected_[channel]);}
    if(speedChoice_){const QSignalBlocker blocked(speedChoice_);speedChoice_->setChecked(speedSelected_);}
    if(!allowSpeed_){unitLabel_->setText(QStringLiteral("显示通道"));return;}
    unitLabel_->setText(speedSelected_?(speedUnit_.compare(QStringLiteral("RPM"),Qt::CaseInsensitive)==0?QStringLiteral("转速 / %1").arg(speedUnit_):QStringLiteral("脉冲频率 / %1").arg(speedUnit_)):QStringLiteral("幅值 / %1").arg(unit_));QStringList detail;
    for(int channel=0;channel<3;++channel){const bool available=!speedSelected_&&channel<stats_.size();const auto measured=available?stats_[channel]:QPointF(std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::quiet_NaN());rmsFields_[channel]->setText(value(measured.x(),3));peakFields_[channel]->setText(value(measured.y(),3));
        const QString source=speedSelected_?QStringLiteral("当前为转速波形；振动幅值统计在选择 CH 通道后显示。"):statsScope_.isEmpty()?QStringLiteral("无原始窗口或全记录统计，显示未知。"):statsScope_;
        rmsFields_[channel]->setToolTip(source+QStringLiteral("\nCH%1 RMS %2 %3").arg(channel+1).arg(value(measured.x(),10),unit_));peakFields_[channel]->setToolTip(source+QStringLiteral("\nCH%1 峰峰值 %2 %3").arg(channel+1).arg(value(measured.y(),10),unit_));
        if(available)detail<<QStringLiteral("CH%1 RMS %2 / 峰峰值 %3 %4").arg(channel+1).arg(value(measured.x(),10),value(measured.y(),10),unit_);
    }
    statsLabel_->setText(speedSelected_?speedSummary_:QStringLiteral("RMS / 峰峰值"));statsLabel_->setToolTip(speedSelected_?speedDetail_:statsScope_+QStringLiteral("\n")+detail.join(QStringLiteral("\n")));
}
void SingleWaveformView::showSpeedValue(){
    QDialog dialog(window());dialog.setObjectName("speedValueDialog");dialog.setWindowTitle(QStringLiteral("转速数值"));dialog.setMinimumSize(330,220);auto* layout=new QVBoxLayout(&dialog);layout->setContentsMargins(24,20,24,20);layout->setSpacing(12);
    auto* scope=new QLabel(speedValueScope_.isEmpty()?QStringLiteral("暂无有效测速"):speedValueScope_);scope->setObjectName("speedValueScope");scope->setWordWrap(true);layout->addWidget(scope);
    auto* reading=new QLabel(speedValueRpm_&&speedValue_?value(*speedValue_,6)+QStringLiteral(" rpm"):QStringLiteral("— rpm"));reading->setObjectName("speedValueReading");reading->setStyleSheet("font-size:38px;font-weight:500;color:#263345;");layout->addWidget(reading);
    auto* note=new QLabel(!speedValueRpm_?QStringLiteral("PPR 未确认，确认每转脉冲后才可显示 rpm。%1").arg(speedValue_?QStringLiteral("当前脉冲频率 %1 Hz").arg(value(*speedValue_,5)):QString()):speedValue_?QString():QStringLiteral("无有效测速数据"));note->setObjectName("speedValueNote");note->setWordWrap(true);layout->addWidget(note);
    auto* close=new QDialogButtonBox(QDialogButtonBox::Close);close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));connect(close,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);layout->addWidget(close);dialog.exec();
}
void SingleWaveformView::clearData(){saveYRange();sources_.clear();speed_={};stats_.clear();statsScope_.clear();speedSummary_.clear();speedDetail_.clear();speedValue_.reset();speedValueScope_.clear();if(!vibrationYFixed_)vibrationY_.reset();speedY_.reset();speedYFixed_=false;plot_->clearXRange();plot_->clearNavigationBounds();plot_->setCursor(std::nullopt);render(true);}
void SingleWaveformView::setDataAvailable(bool available){
    plot_->setEnabled(available);
    for(auto* choice:choices_)choice->setEnabled(available);
    if(speedChoice_)speedChoice_->setEnabled(available);
    if(auto* value=findChild<QPushButton*>("speedValueButton"))value->setEnabled(available);
    rangeButton_->setEnabled(true);
}
}
