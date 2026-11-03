#include "analysis_pane.hpp"
#include "plot_canvas.hpp"
#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include "window.hpp"
#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
using namespace vibration;
using namespace vibration::workbench;
namespace {
bool near(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<1e-9;}
bool samePoints(const QVector<QPointF>& a,const QVector<QPointF>& b){if(a.size()!=b.size())return false;for(int i=0;i<a.size();++i)if(a[i].x()!=b[i].x()||(!(std::isnan(a[i].y())&&std::isnan(b[i].y()))&&a[i].y()!=b[i].y()))return false;return true;}
bool sameSeries(const QVector<PlotSeries>& a,const QVector<PlotSeries>& b){if(a.size()!=b.size())return false;for(int i=0;i<a.size();++i)if(a[i].name!=b[i].name||!samePoints(a[i].points,b[i].points))return false;return true;}
bool yRange(const PlotCanvas& plot,double lower,double upper){const auto y=plot.explicitYRange();return y&&near(y->x(),lower)&&near(y->y(),upper);}
bool xRange(const PlotCanvas& plot,double lower,double upper){const auto x=plot.explicitXRange();return x&&near(x->x(),lower)&&near(x->y(),upper);}
template<class T>T* child(QWidget& parent,const char* name){auto* widget=dynamic_cast<T*>(parent.findChild<QWidget*>(QString::fromLatin1(name)));if(!widget)throw std::runtime_error(std::string("Missing control: ")+name);return widget;}
void paint(QWidget& widget){QApplication::processEvents();widget.grab();}
void mouse(PlotCanvas& plot,QEvent::Type type,const QPointF& position,Qt::MouseButton button,Qt::MouseButtons buttons,Qt::KeyboardModifiers modifiers=Qt::NoModifier){QMouseEvent event(type,position,button,buttons,modifiers);QApplication::sendEvent(&plot,&event);}
void drag(PlotCanvas& plot,const QPointF& first,const QPointF& last,Qt::KeyboardModifiers modifiers=Qt::NoModifier){mouse(plot,QEvent::MouseButtonPress,first,Qt::LeftButton,Qt::LeftButton,modifiers);mouse(plot,QEvent::MouseMove,last,Qt::NoButton,Qt::LeftButton,modifiers);mouse(plot,QEvent::MouseButtonRelease,last,Qt::LeftButton,Qt::NoButton,modifiers);}
void wheel(PlotCanvas& plot,const QPointF& position){QWheelEvent event(position,position,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(&plot,&event);}
void put(QByteArray& bytes,int offset,quint64 value,int count){for(int i=0;i<count;++i)bytes[offset+i]=char(value>>(i*8));}
QJsonObject frame(int first,int sequence){
    QByteArray bytes(1000*32,0);for(int i=0;i<1000;++i){const int index=first+i,at=i*32;put(bytes,at,index,8);put(bytes,at+8,quint64(index)*10000,8);for(int ch=0;ch<3;++ch)put(bytes,at+16+ch*2,quint16(1000*std::sin(index*.1)+ch*100),2);put(bytes,at+22,3,2);put(bytes,at+24,500000+sequence*1000,4);}
    return {{"schema","vibration.capture.v2"},{"type","event"},{"event","preview"},{"job_id","single_waveform_probe"},{"generation","1"},{"frame_seq",sequence},{"first_record",first},{"record_count",1000},{"committed_records",first+1000},{"requested_records",4000},{"sample_rate_hz",5000},{"clock_hz",50000000},{"ppr",1},{"adc_range_uv",5000000},{"channel_map",328449},{"record_bytes",32},{"source","test_fixture"},{"hardware_confirmed",false},{"synthetic",true},{"source_flags",1},{"validity_flags",27},{"raw_base64",QString::fromLatin1(bytes.toBase64())},{"crc32",double(crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),bytes.size()))},{"dropped_preview_records",0}};
}
bool selectContextAction(PlotCanvas& plot,const QString& text){
    bool selected=false,timedOut=false;QTimer timer;timer.setInterval(20);QElapsedTimer elapsed;elapsed.start();
    QObject::connect(&timer,&QTimer::timeout,&plot,[&]{if(elapsed.elapsed()>5000){timedOut=true;timer.stop();if(auto* popup=QApplication::activePopupWidget())popup->close();return;}auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu||menu->objectName()!=QStringLiteral("plotContextMenu"))return;for(auto* action:menu->actions())if(action->text()==text){selected=true;timer.stop();menu->setActiveAction(action);QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&enter);return;}});
    const auto point=plot.plotArea().center().toPoint();timer.start();QContextMenuEvent event(QContextMenuEvent::Mouse,point,plot.mapToGlobal(point));QApplication::sendEvent(&plot,&event);timer.stop();return selected&&!timedOut;
}
bool inspectSpeedValue(SingleWaveformView& view,const QString& expected,const QString& scope,bool requirePpr=false){
    bool inspected=false,timedOut=false;QTimer timer;timer.setInterval(20);QElapsedTimer elapsed;elapsed.start();
    QObject::connect(&timer,&QTimer::timeout,&view,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(elapsed.elapsed()>5000){timedOut=true;timer.stop();if(dialog)dialog->reject();return;}if(!dialog||dialog->objectName()!=QStringLiteral("speedValueDialog"))return;auto* reading=dialog->findChild<QLabel*>("speedValueReading");auto* provenance=dialog->findChild<QLabel*>("speedValueScope");QString allLabels;for(auto* label:dialog->findChildren<QLabel*>())allLabels+=label->text();inspected=reading&&provenance&&reading->text().contains(expected)&&provenance->text().contains(scope)&&(!requirePpr||allLabels.contains(QStringLiteral("PPR")));timer.stop();dialog->accept();});
    timer.start();child<QPushButton>(view,"speedValueButton")->click();timer.stop();return inspected&&!timedOut;
}
struct AmplitudeDialogResult {bool opened=false,rejectedNan=false,applied=false,timedOut=false;};
AmplitudeDialogResult inspectAmplitudeDialog(PlotCanvas& plot){
    AmplitudeDialogResult result;const auto before=plot.explicitYRange();int phase=0;QTimer timer;timer.setInterval(20);QElapsedTimer elapsed;elapsed.start();
    QObject::connect(&timer,&QTimer::timeout,&plot,[&]{
        if(elapsed.elapsed()>5000){result.timedOut=true;timer.stop();if(auto* popup=QApplication::activePopupWidget())popup->close();if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();return;}
        if(phase==0){auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu||menu->objectName()!=QStringLiteral("plotContextMenu"))return;for(auto* action:menu->actions())if(action->text()==QStringLiteral("精确幅值范围…")){phase=1;menu->setActiveAction(action);QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&enter);return;}}
        else if(phase==1||phase==2){auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog||dialog->objectName()!=QStringLiteral("plotAmplitudeDialog"))return;result.opened=true;auto* lower=dialog->findChild<QLineEdit*>("plotAmplitudeLower");auto* upper=dialog->findChild<QLineEdit*>("plotAmplitudeUpper");auto* error=dialog->findChild<QLabel*>("plotAmplitudeError");auto* buttons=dialog->findChild<QDialogButtonBox*>();auto* apply=buttons?buttons->button(QDialogButtonBox::Ok):nullptr;if(!lower||!upper||!error||!apply){timer.stop();dialog->reject();return;}if(phase==1){lower->setText(QStringLiteral("nan"));upper->setText(QStringLiteral("2"));apply->click();result.rejectedNan=dialog->isVisible()&&!error->text().isEmpty()&&plot.explicitYRange()==before;phase=2;}else{lower->setText(QStringLiteral("-1.25"));upper->setText(QStringLiteral("3.75"));phase=3;timer.stop();apply->click();result.applied=yRange(plot,-3.75,3.75);}}
    });
    const auto point=plot.plotArea().center().toPoint();timer.start();QContextMenuEvent event(QContextMenuEvent::Mouse,point,plot.mapToGlobal(point));QApplication::sendEvent(&plot,&event);timer.stop();return result;
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);bool passed=true;QJsonObject report;const auto check=[&](const char* name,bool value){report[name]=value;passed&=value;};
    const QString evidence=argc>1?QString::fromLocal8Bit(argv[1]):QString();if(!evidence.isEmpty()&&!QDir().mkpath(evidence))return 2;
    try {
        QVector<PlotSeries> original;for(int ch=0;ch<3;++ch){PlotSeries line{QString("CH%1").arg(ch+1),QColor(ch==0?"#3569c8":ch==1?"#198b7c":"#b27b2f"),{}};for(int i=0;i<=1000;++i)line.points.push_back({i/100.,std::sin(i/10.)+ch});line.points[500].setY(std::numeric_limits<double>::quiet_NaN());original.push_back(line);}
        PlotSeries rpm{QStringLiteral("转速"),QColor("#6e6596"),{}};for(int i=0;i<=1000;++i)rpm.points.push_back({i/100.,1500+50*std::sin(i/20.)});
        SingleWaveformView view;view.resize(1280,720);view.setSourceSeries(original,"V");view.setSpeedSeries(rpm,"RPM");view.setNavigationBounds(0,10);view.setXRange(0,10);view.show();paint(view);auto* plot=view.plot();
        check("one_large_canvas_with_stable_object_names",view.objectName()==QStringLiteral("singleWaveformView")&&plot&&plot->objectName()==QStringLiteral("singleWaveCanvas"));
        int canvasCount=0;for(auto* widget:view.findChildren<QWidget*>())canvasCount+=dynamic_cast<PlotCanvas*>(widget)!=nullptr;check("only_one_time_domain_canvas",canvasCount==1);
        auto* ch1=child<QAbstractButton>(view,"channelSelect1");auto* ch2=child<QAbstractButton>(view,"channelSelect2");auto* ch3=child<QAbstractButton>(view,"channelSelect3");auto* speedSelect=child<QAbstractButton>(view,"speedSelect");auto* unit=child<QLabel>(view,"singleWaveUnit");
        bool readableRows=true,visibleFrames=true;for(auto* button:{ch1,ch2,ch3,speedSelect,static_cast<QAbstractButton*>(child<QPushButton>(view,"speedValueButton"))})readableRows&=button->font().pixelSize()>=16&&button->height()>=44;
        for(int channel=1;channel<=3;++channel)for(const auto& kind:{QStringLiteral("Rms"),QStringLiteral("Peak")}){auto* field=child<QLabel>(view,QStringLiteral("single%1%2").arg(kind).arg(channel).toLatin1().constData());const auto image=field->grab().toImage();visibleFrames&=field->font().pixelSize()>=16&&field->height()>=44&&!image.isNull()&&qGray(image.pixel(image.width()/2,0))<190;}check("channel_checkbox_rows_and_statistics_frames_are_readable",readableRows&&visibleFrames);
        check("default_selects_ch1_only",view.selectedChannels()==QVector<int>({0})&&!view.showingSpeed()&&ch1->isChecked()&&!ch2->isChecked()&&!ch3->isChecked()&&!speedSelect->isChecked()&&plot->series().size()==1&&samePoints(plot->series()[0].points,original[0].points));
        ch3->click();check("channel_multiselect_overlays_original_series",view.selectedChannels()==QVector<int>({0,2})&&plot->series().size()==2&&samePoints(plot->series()[0].points,original[0].points)&&samePoints(plot->series()[1].points,original[2].points));
        view.setAmplitudeStats({QPointF(.5,1.4),QPointF(.6,1.7),QPointF(.7,2.)},QStringLiteral("原始预览可见窗"));auto* stats=child<QLabel>(view,"singleWaveStats");check("amplitude_stats_keep_authoritative_scope",(stats->text()+stats->toolTip()).contains(QStringLiteral("原始预览可见窗"))&&(stats->text()+stats->toolTip()).contains(QStringLiteral("RMS")));
        plot->setYRange(-2,2);speedSelect->click();check("speed_is_mutually_exclusive_with_vibration",view.showingSpeed()&&view.selectedChannels().isEmpty()&&speedSelect->isChecked()&&!ch1->isChecked()&&!ch2->isChecked()&&!ch3->isChecked()&&plot->series().size()==1&&samePoints(plot->series()[0].points,rpm.points)&&unit->text().contains(QStringLiteral("RPM")));
        plot->setYRange(1000,2000);speedSelect->click();check("unchecking_speed_restores_previous_channel_mask",!view.showingSpeed()&&view.selectedChannels()==QVector<int>({0,2})&&plot->series().size()==2&&yRange(*plot,-2,2));
        view.setSpeedSelected(true);check("speed_manual_range_is_independent",view.showingSpeed()&&yRange(*plot,1000,2000));ch2->click();check("selecting_channel_from_speed_returns_only_new_channel",!view.showingSpeed()&&view.selectedChannels()==QVector<int>({1})&&plot->series().size()==1&&samePoints(plot->series()[0].points,original[1].points)&&yRange(*plot,-2,2));
        ch2->click();check("all_channels_off_is_explicit_empty_display",!view.showingSpeed()&&view.selectedChannels().isEmpty()&&std::all_of(plot->series().begin(),plot->series().end(),[](const auto& line){return !line.visible||line.points.empty();})&&sameSeries(view.sourceSeries(),original));
        auto updated=original;for(auto& line:updated)for(auto& point:line.points)if(std::isfinite(point.y()))point.setY(point.y()+.25);
        view.setSpeedSelected(true);view.setSourceSeries(updated,"V");view.setSpeedSeries(rpm,"RPM");check("hidden_channel_refresh_retains_all_original_inputs",view.showingSpeed()&&sameSeries(view.sourceSeries(),updated)&&samePoints(view.speedSeries().points,rpm.points)&&yRange(*plot,1000,2000));view.setChannelChecked(2,true);check("newly_selected_channel_uses_latest_hidden_data",view.selectedChannels()==QVector<int>({2})&&plot->series().size()==1&&samePoints(plot->series()[0].points,updated[2].points)&&yRange(*plot,-2,2));
        view.setSourceSeries(updated,QStringLiteral("g（名义）"));check("vibration_unit_change_resets_only_vibration_manual_range",!plot->explicitYRange()&&unit->text().contains(QStringLiteral("名义")));view.setSpeedSelected(true);check("vibration_unit_change_preserves_speed_manual_range",yRange(*plot,1000,2000)&&unit->text().contains(QStringLiteral("RPM")));view.setSpeedSelected(false);
        check("caller_raw_data_and_gap_unchanged",original[0].points.size()==1001&&std::isnan(original[0].points[500].y())&&near(original[0].points[0].y(),0)&&samePoints(view.sourceSeries()[0].points,updated[0].points));
        view.setSpeedValue(6000,QStringLiteral("全记录平均"),true);check("speed_value_dialog_uses_authoritative_rpm_and_scope",inspectSpeedValue(view,QStringLiteral("6000"),QStringLiteral("全记录平均")));view.setSpeedValue(33.3,QStringLiteral("预览窗口"),false);check("unknown_ppr_does_not_invent_rpm",inspectSpeedValue(view,QStringLiteral("—"),QStringLiteral("预览窗口"),true));view.setSpeedValue(std::nullopt,QStringLiteral("无有效数据"),true);check("missing_speed_value_is_dash_not_zero",inspectSpeedValue(view,QStringLiteral("—"),QStringLiteral("无有效数据")));
        view.setSourceSeries(original,"V");view.setChannelChecked(0,true);view.setChannelChecked(1,false);view.setChannelChecked(2,false);view.setXRange(0,10);paint(view);check("default_mouse_navigation_is_pan",plot->navigationMode()==PlotCanvas::NavigationMode::Pan);
        const auto area=plot->plotArea();const QPointF boxFirst(area.left()+area.width()*.25,area.center().y()),boxLast(area.left()+area.width()*.75,area.center().y());drag(*plot,boxFirst,boxLast,Qt::ShiftModifier);check("shift_drag_box_zoom_maps_to_time",xRange(*plot,2.5,7.5));plot->setXRange(2,6);paint(view);const auto panArea=plot->plotArea();drag(*plot,{panArea.left()+panArea.width()*.25,panArea.center().y()},{panArea.left()+panArea.width()*.5,panArea.center().y()});check("plain_drag_pans_time",xRange(*plot,1,5));
        plot->setYRange(-2,6);paint(view);const auto yArea=plot->plotArea();const auto beforeY=plot->displayedYRange();const auto beforeYX=plot->explicitXRange();wheel(*plot,{std::max(2.,yArea.left()*.5),yArea.top()+yArea.height()*.25});paint(view);const auto zoomedY=plot->displayedYRange();
        check("y_axis_wheel_zooms_amplitude_around_zero",plot->explicitYRange()&&near(zoomedY.x(),-zoomedY.y())&&near(zoomedY.y(),beforeY.y()*.8)&&plot->explicitXRange()==beforeYX);
        const auto dragArea=plot->plotArea();const auto beforeDragY=plot->displayedYRange();drag(*plot,{std::max(2.,dragArea.left()*.5),dragArea.top()+dragArea.height()*.25},{std::max(2.,dragArea.left()*.5),dragArea.top()+dragArea.height()*.5});paint(view);const auto draggedY=plot->displayedYRange();
        check("y_axis_drag_scales_amplitude_around_zero_without_time_change",near(draggedY.x(),-draggedY.y())&&near(draggedY.y(),beforeDragY.y()*std::sqrt(2.))&&plot->explicitXRange()==beforeYX);
        mouse(*plot,QEvent::MouseButtonDblClick,plot->plotArea().center(),Qt::LeftButton,Qt::LeftButton);check("double_click_restores_full_time_and_auto_amplitude",xRange(*plot,0,10)&&!plot->explicitYRange());
        const auto amplitudeDialog=inspectAmplitudeDialog(*plot);check("right_click_precise_amplitude_dialog_rejects_nan",amplitudeDialog.opened&&amplitudeDialog.rejectedNan&&!amplitudeDialog.timedOut);check("right_click_precise_amplitude_dialog_applies_valid_range",amplitudeDialog.applied&&yRange(*plot,-3.75,3.75));check("right_click_auto_amplitude_clears_manual_range",selectContextAction(*plot,QStringLiteral("自动幅值"))&&!plot->explicitYRange());check("mouse_amplitude_interactions_keep_raw_samples_unchanged",sameSeries(view.sourceSeries(),original));
        for(const auto& size:{QSize(1280,720),QSize(1024,600)}){view.resize(size);view.setChannelChecked(0,true);view.setChannelChecked(2,true);view.setSpeedSelected(false);paint(view);const QRect canvas(plot->mapTo(&view,QPoint()),plot->size());const int selectorLeft=ch1->mapTo(&view,QPoint()).x();check(size.width()==1280?"large_canvas_and_right_selector_fit_1280":"large_canvas_and_right_selector_fit_1024",view.size()==size&&plot->width()>=size.width()*.70&&plot->width()<=size.width()*.82&&plot->height()>size.height()*.65&&selectorLeft>=canvas.right());if(!evidence.isEmpty())check(size.width()==1280?"multi_1280_screenshot_saved":"multi_1024_screenshot_saved",view.grab().save(evidence+QStringLiteral("/single-multi-%1x%2.png").arg(size.width()).arg(size.height())));view.setSpeedSelected(true);paint(view);if(!evidence.isEmpty())check(size.width()==1280?"speed_1280_screenshot_saved":"speed_1024_screenshot_saved",view.grab().save(evidence+QStringLiteral("/single-speed-%1x%2.png").arg(size.width()).arg(size.height())));}
        AnalysisPane pane;pane.resize(1280,720);pane.show();QString error;check("live_first_frame_accepted",pane.beginLive(frame(0,1),true,&error));if(!pane.isLive())throw std::runtime_error(error.toStdString());paint(pane);auto* live=child<SingleWaveformView>(pane,"singleWaveformView");auto* livePlot=live->plot();auto* follow=child<QPushButton>(pane,"liveFollow");
        livePlot->zoomX(.5);const auto frozenSources=live->sourceSeries();const auto frozenSpeed=live->speedSeries();const auto frozenRange=livePlot->explicitXRange();check("next_frame_ingested_while_display_paused",pane.acceptLiveFrame(frame(1000,2),&error));live->setChannelChecked(2,true);live->setSpeedSelected(true);pane.setView(1);pane.setView(0);paint(pane);
        check("selector_and_page_switches_preserve_frozen_raw_snapshots",sameSeries(live->sourceSeries(),frozenSources)&&samePoints(live->speedSeries().points,frozenSpeed.points)&&livePlot->series().size()==1&&samePoints(livePlot->series()[0].points,frozenSpeed.points)&&livePlot->explicitXRange()==frozenRange&&follow->text().contains(QStringLiteral("回到最新")));
        follow->click();paint(pane);check("existing_live_follow_restores_latest_both_sources",!sameSeries(live->sourceSeries(),frozenSources)&&!samePoints(live->speedSeries().points,frozenSpeed.points)&&live->showingSpeed()&&follow->text().contains(QStringLiteral("暂停显示")));
        livePlot->zoomX(.5);const auto secondFrozen=live->sourceSeries();check("third_frame_ingested_while_paused",pane.acceptLiveFrame(frame(2000,3),&error));live->setChannelChecked(0,true);paint(pane);check("choosing_channel_does_not_resume_live_follow",sameSeries(live->sourceSeries(),secondFrozen)&&!live->showingSpeed()&&live->selectedChannels()==QVector<int>({0})&&follow->text().contains(QStringLiteral("回到最新")));
        const bool contextResume=selectContextAction(*livePlot,QStringLiteral("回到最新"));paint(pane);check("right_click_resume_latest_keeps_selected_channel",contextResume&&!sameSeries(live->sourceSeries(),secondFrozen)&&live->selectedChannels()==QVector<int>({0})&&!live->showingSpeed()&&follow->text().contains(QStringLiteral("暂停显示")));
        if(!evidence.isEmpty())check("live_single_canvas_screenshot_saved",pane.grab().save(evidence+QStringLiteral("/single-live-1280x720.png")));pane.endLive();view.clearData();const auto clearedSources=view.sourceSeries();check("clear_drops_all_retained_samples",std::all_of(clearedSources.begin(),clearedSources.end(),[](const auto& line){return line.points.empty();})&&view.speedSeries().points.empty());
        view.hide();pane.hide();Window window;window.show();check("acceleration_tab_is_visible",child<QPushButton>(window,"viewTab2")->isVisible()&&child<QPushButton>(window,"viewTab2")->text()==QStringLiteral("加速度"));
        if(argc>2){window.openCapture(QString::fromLocal8Bit(argv[2]));QElapsedTimer loading;loading.start();while(window.isAnalyzing()&&loading.elapsed()<30000){QApplication::processEvents();QThread::msleep(5);}auto* actual=child<SingleWaveformView>(window,"singleWaveformView");check("actual_window_loaded_original_file_fixture",!window.isAnalyzing()&&actual->sourceSeries().size()==3&&!actual->sourceSeries()[0].points.empty());
            for(const auto& size:{QSize(1024,600),QSize(1280,720)}){window.resize(size);window.setView(0);actual->setSpeedSelected(false);actual->setChannelChecked(0,true);actual->setChannelChecked(1,false);actual->setChannelChecked(2,false);paint(window);bool saved=evidence.isEmpty()||window.grab().save(evidence+QStringLiteral("/window-single-default-%1x%2.png").arg(size.width()).arg(size.height()));actual->setChannelChecked(1,true);actual->setChannelChecked(2,true);paint(window);saved=(evidence.isEmpty()||window.grab().save(evidence+QStringLiteral("/window-single-multi-%1x%2.png").arg(size.width()).arg(size.height())))&&saved;actual->setSpeedSelected(true);paint(window);saved=(evidence.isEmpty()||window.grab().save(evidence+QStringLiteral("/window-single-speed-%1x%2.png").arg(size.width()).arg(size.height())))&&saved;check(size.width()==1024?"actual_window_single_modes_1024":"actual_window_single_modes_1280",window.size()==size&&saved&&child<QPushButton>(window,"viewTab2")->isVisible()&&actual->showingSpeed()&&actual->plot()->series().size()==1);
                window.showAcquisition();paint(window);bool parameterReadable=window.size()==size&&child<QLabel>(window,"captureDirectoryLabel")->font().pixelSize()>=16&&child<QPushButton>(window,"captureStart")->height()>=44;
                if(!evidence.isEmpty())parameterReadable=window.grab().save(evidence+QStringLiteral("/window-capture-readable-%1x%2.png").arg(size.width()).arg(size.height()))&&parameterReadable;
                check(size.width()==1024?"capture_page_readable_and_fits_1024":"capture_page_readable_and_fits_1280",parameterReadable);
            }
        }else report["optional_actual_window_fixture"]="not provided; pass argv[2] CAPDDR2 to exercise file-loaded Window screenshots";
    } catch(const std::exception& error){report["error"]=QString::fromUtf8(error.what());passed=false;}
    report["measurement_scope"]="Host Qt software-rendered synthetic fixtures and selector/live-state checks; no physical display, ADC, sensor or calibration acceptance";report["passed"]=passed;std::puts(QJsonDocument(report).toJson(QJsonDocument::Indented).constData());return passed?0:1;
}
