#include "analysis_pane.hpp"
#include "plot_canvas.hpp"
#include "waveform_stack.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QAction>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
using namespace vibration;
using namespace vibration::workbench;
namespace {
bool near(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<1e-9;}
bool range(const PlotCanvas& plot,double a,double b){const auto r=plot.explicitXRange();return r&&near(r->x(),a)&&near(r->y(),b);}
bool samePoints(const QVector<QPointF>& a,const QVector<QPointF>& b){if(a.size()!=b.size())return false;for(int i=0;i<a.size();++i)if(a[i].x()!=b[i].x()||(!(std::isnan(a[i].y())&&std::isnan(b[i].y()))&&a[i].y()!=b[i].y()))return false;return true;}
void mouse(PlotCanvas& plot,QEvent::Type type,const QPointF& position,Qt::MouseButton button,Qt::MouseButtons buttons,Qt::KeyboardModifiers modifiers=Qt::NoModifier){QMouseEvent event(type,position,button,buttons,modifiers);QApplication::sendEvent(&plot,&event);}
void drag(PlotCanvas& plot,const QPointF& first,const QPointF& last,Qt::KeyboardModifiers modifiers=Qt::NoModifier){mouse(plot,QEvent::MouseButtonPress,first,Qt::LeftButton,Qt::LeftButton,modifiers);mouse(plot,QEvent::MouseMove,last,Qt::NoButton,Qt::LeftButton,modifiers);mouse(plot,QEvent::MouseButtonRelease,last,Qt::LeftButton,Qt::NoButton,modifiers);}
void wheel(PlotCanvas& plot,const QPointF& position,int delta=120){QWheelEvent event(position,position,QPoint(),QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(&plot,&event);}
void put(QByteArray& bytes,int offset,quint64 value,int count){for(int i=0;i<count;++i)bytes[offset+i]=char(value>>(i*8));}
QJsonObject frame(int first,int sequence){
    QByteArray bytes(1000*32,0);for(int i=0;i<1000;++i){const int index=first+i,at=i*32;put(bytes,at,index,8);put(bytes,at+8,quint64(index)*10000,8);for(int ch=0;ch<3;++ch)put(bytes,at+16+ch*2,quint16(1000*std::sin(index*.1)+ch*100),2);put(bytes,at+22,3,2);put(bytes,at+24,500000,4);}
    return {{"schema","vibration.capture.v2"},{"type","event"},{"event","preview"},{"job_id","navigation_probe"},{"generation","1"},{"frame_seq",sequence},{"first_record",first},{"record_count",1000},{"committed_records",first+1000},{"requested_records",4000},{"sample_rate_hz",5000},{"clock_hz",50000000},{"ppr",1},{"adc_range_uv",5000000},{"channel_map",328449},{"record_bytes",32},{"source","test_fixture"},{"hardware_confirmed",false},{"synthetic",true},{"source_flags",1},{"validity_flags",27},{"raw_base64",QString::fromLatin1(bytes.toBase64())},{"crc32",double(crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),bytes.size()))},{"dropped_preview_records",0}};
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);bool passed=true;QJsonObject report;const auto check=[&](const char* name,bool value){report[name]=value;passed&=value;};
    QVector<PlotSeries> source;for(int ch=0;ch<3;++ch){PlotSeries line{QString("CH%1").arg(ch+1),QColor("#3569c8"),{}};for(int i=0;i<=1000;++i)line.points.push_back({i/100.,std::sin(i/10.)+ch});line.points[500].setY(std::numeric_limits<double>::quiet_NaN());source.push_back(line);}
    PlotSeries speedSeries{QStringLiteral("转速"),QColor("#6e6596"),{}};for(int i=0;i<=1000;++i)speedSeries.points.push_back({i/100.,1500+50*std::sin(i/20.)});
    WaveformStack stack;stack.resize(1000,600);stack.setSourceSeries(source,"V");stack.setSpeedSeries(speedSeries,"RPM");stack.setNavigationBounds(0,10);stack.setXRange(0,10);stack.show();QApplication::processEvents();
    auto* speedRow=stack.plot(4);check("default_fifth_speed_row_has_stable_id",speedRow&&stack.panelOrder()==QVector<int>({0,1,2,3,4})&&stack.panelChannels(4).isEmpty());if(!speedRow)return 1;
    check("fifth_speed_row_uses_original_speed_points",speedRow->series().size()==1&&samePoints(speedRow->series()[0].points,speedSeries.points));
    auto* plot=stack.plot(0);check("zoom_anchor",plot->zoomX(.5,2)&&range(*plot,1,6));bool linked=true;for(int i=0;i<5;++i)linked&=range(*stack.plot(i),1,6);check("five_time_rows_linked",linked);
    speedRow->setYRange(1000,2000);check("fifth_speed_row_cannot_merge_vibration_channels",!stack.addChannel(4,0)&&!stack.removeChannel(4,0)&&!stack.moveChannel(0,4,0)&&!stack.moveChannel(4,0,0)&&!stack.splitChannel(4,0)&&stack.panelChannels(4).isEmpty());
    const bool movedSpeed=stack.movePanel(4,0);check("speed_row_reorder_preserves_plot_identities",movedSpeed&&stack.panelOrder().first()==4&&stack.plot(4)==speedRow&&stack.plot(0)==plot&&samePoints(speedRow->series()[0].points,speedSeries.points));stack.movePanel(4,4);
    plot->panX(-100);check("pan_clamped_left",range(*plot,0,5));plot->panX(100);check("pan_clamped_right",range(*plot,5,10));
    const auto before=plot->explicitXRange();check("nonfinite_zoom_rejected",!plot->zoomX(std::numeric_limits<double>::quiet_NaN())&&plot->explicitXRange()==before);
    check("manual_validation",stack.setYRange(-1.25,3.75)&&!stack.setYRange(1,1)&&!plot->setYRange(0,std::numeric_limits<double>::infinity()));
    check("vibration_manual_bounds_do_not_override_speed_y",speedRow->explicitYRange()&&near(speedRow->explicitYRange()->x(),1000)&&near(speedRow->explicitYRange()->y(),2000));
    stack.movePanel(0,2);stack.moveChannel(1,0,1);stack.setSourceSeries(source,"V");check("manual_persists_refresh_merge_reorder",plot->explicitYRange()&&near(plot->explicitYRange()->x(),-3.75)&&near(plot->explicitYRange()->y(),3.75));
    plot->grab();check("manual_axis_labels_exact",plot->yTickLabels().contains("-3.75")&&plot->yTickLabels().contains("0")&&plot->yTickLabels().contains("3.75"));
    bool gap=false;for(const auto& p:plot->displayPoints(0))gap|=!std::isfinite(p.y());check("missing_gap_preserved",gap);bool unchanged=plot->series()[0].points.size()==source[0].points.size();for(int i=0;i<source[0].points.size()&&unchanged;++i)unchanged=plot->series()[0].points[i].x()==source[0].points[i].x()&&((std::isnan(plot->series()[0].points[i].y())&&std::isnan(source[0].points[i].y()))||plot->series()[0].points[i].y()==source[0].points[i].y());check("original_samples_unchanged",unchanged);
    stack.setSourceSeries(source,"g（名义）");check("unit_change_resets_manual",!plot->explicitYRange());stack.setYRange(-2,2);stack.clearYRange();plot->restoreView();check("auto_restore",!plot->explicitYRange()&&range(*plot,0,10));
    check("vibration_auto_restore_preserves_independent_speed_y",speedRow->explicitYRange()&&near(speedRow->explicitYRange()->x(),1000)&&near(speedRow->explicitYRange()->y(),2000));
    PlotCanvas gesture;gesture.resize(900,260);gesture.setChart({},"s","V",{source[0]});gesture.setNavigationBounds(0,10);gesture.setXRange(0,10);gesture.show();QApplication::processEvents();gesture.grab();
    check("default_mouse_navigation_is_pan",gesture.navigationMode()==PlotCanvas::NavigationMode::Pan);
    const auto area=gesture.plotArea();const QPointF begin(area.left()+area.width()*.25,area.center().y()),end(area.left()+area.width()*.75,area.center().y());
    drag(gesture,begin,end,Qt::ShiftModifier);check("shift_left_box_gesture_maps_coordinates",range(gesture,2.5,7.5));
    check("shift_box_does_not_change_default_mode",gesture.navigationMode()==PlotCanvas::NavigationMode::Pan);
    gesture.setXRange(2,6);gesture.grab();const auto panArea=gesture.plotArea();const QPointF panBegin(panArea.left()+panArea.width()*.25,panArea.center().y()),panEnd(panArea.left()+panArea.width()*.5,panArea.center().y());
    drag(gesture,panBegin,panEnd);check("unmodified_left_drag_pan_maps_coordinates",range(gesture,1,5));
    gesture.setXRange(0,10);gesture.grab();const auto wheelArea=gesture.plotArea();const QPointF xAnchor(wheelArea.left()+wheelArea.width()*.2,wheelArea.center().y());wheel(gesture,xAnchor);
    const auto xZoom=gesture.explicitXRange();check("wheel_over_plot_zooms_x_at_pointer",xZoom&&xZoom->y()-xZoom->x()<10&&near(xZoom->x()+.2*(xZoom->y()-xZoom->x()),2));
    gesture.setYRange(-2,6);gesture.grab();const auto yArea=gesture.plotArea();const QPointF yAnchor(std::max(2.,yArea.left()*.5),yArea.top()+yArea.height()*.25);const auto priorY=gesture.displayedYRange();const auto priorX=gesture.explicitXRange();wheel(gesture,yAnchor);gesture.grab();
    const auto yZoom=gesture.displayedYRange();const double yAnchorBefore=priorY.x()+.75*(priorY.y()-priorY.x()),yAnchorAfter=yZoom.x()+.75*(yZoom.y()-yZoom.x());
    check("wheel_in_y_gutter_zooms_y_at_pointer",gesture.explicitYRange()&&yZoom.y()-yZoom.x()<priorY.y()-priorY.x()&&near(yAnchorBefore,yAnchorAfter));
    check("y_wheel_preserves_time_range",gesture.explicitXRange()==priorX);
    const auto shiftedArea=gesture.plotArea();const QPointF yStart(std::max(2.,shiftedArea.left()*.5),shiftedArea.top()+shiftedArea.height()*.25),yEnd(yStart.x(),shiftedArea.top()+shiftedArea.height()*.5);const auto yBeforeDrag=gesture.displayedYRange();drag(gesture,yStart,yEnd);gesture.grab();const auto yAfterDrag=gesture.displayedYRange();
    check("drag_y_axis_shifts_y_with_same_span",near(yAfterDrag.y()-yAfterDrag.x(),yBeforeDrag.y()-yBeforeDrag.x())&&near(std::abs(yAfterDrag.x()-yBeforeDrag.x()),(yBeforeDrag.y()-yBeforeDrag.x())*.25));
    check("y_drag_preserves_time_range",gesture.explicitXRange()==priorX);
    mouse(gesture,QEvent::MouseButtonDblClick,gesture.plotArea().center(),Qt::LeftButton,Qt::LeftButton);gesture.grab();const auto restored=gesture.explicitXRange().value_or(gesture.navigationBounds());
    check("double_click_restores_full_x_and_auto_y",near(restored.x(),0)&&near(restored.y(),10)&&!gesture.explicitYRange());
    const auto beforeDialogY=gesture.explicitYRange();bool preciseMenuSelected=false,invalidDialogRefused=false,dialogTimedOut=false;int dialogPhase=0;
    QTimer dialogTimer;dialogTimer.setInterval(20);QElapsedTimer dialogElapsed;dialogElapsed.start();
    QObject::connect(&dialogTimer,&QTimer::timeout,&gesture,[&]{
        if(dialogElapsed.elapsed()>5000){dialogTimedOut=true;dialogTimer.stop();if(auto* popup=QApplication::activePopupWidget())popup->close();if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();return;}
        if(dialogPhase==0){
            auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu||menu->objectName()!=QStringLiteral("plotContextMenu"))return;
            for(auto* action:menu->actions())if(action->text()==QStringLiteral("精确幅值范围…")){preciseMenuSelected=true;dialogPhase=1;menu->setActiveAction(action);QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&enter);return;}
        } else if(dialogPhase==1||dialogPhase==2){
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog||dialog->objectName()!=QStringLiteral("plotAmplitudeDialog"))return;
            auto* lower=dialog->findChild<QLineEdit*>("plotAmplitudeLower");auto* upper=dialog->findChild<QLineEdit*>("plotAmplitudeUpper");auto* error=dialog->findChild<QLabel*>("plotAmplitudeError");auto* actions=dialog->findChild<QDialogButtonBox*>();auto* apply=actions?actions->button(QDialogButtonBox::Ok):nullptr;
            if(!lower||!upper||!error||!apply){dialogPhase=-1;dialogTimer.stop();dialog->reject();return;}
            if(dialogPhase==1){lower->setText(QStringLiteral("nan"));upper->setText(QStringLiteral("2"));apply->click();invalidDialogRefused=dialog->isVisible()&&!error->text().isEmpty()&&gesture.explicitYRange()==beforeDialogY;dialogPhase=2;}
            else {lower->setText(QStringLiteral("-1.25"));upper->setText(QStringLiteral("3.75"));dialogPhase=3;dialogTimer.stop();apply->click();}
        }
    });
    const auto contextPosition=gesture.plotArea().center().toPoint();dialogTimer.start();QContextMenuEvent preciseContext(QContextMenuEvent::Mouse,contextPosition,gesture.mapToGlobal(contextPosition));QApplication::sendEvent(&gesture,&preciseContext);dialogTimer.stop();
    check("right_click_menu_opens_precise_amplitude_dialog",preciseMenuSelected&&!dialogTimedOut&&dialogPhase==3);
    check("precise_dialog_rejects_nan_without_range_change",invalidDialogRefused);
    const auto preciseY=gesture.explicitYRange();check("precise_dialog_applies_exact_manual_bounds",preciseY&&near(preciseY->x(),-1.25)&&near(preciseY->y(),3.75));
    bool autoMenuSelected=false,autoMenuTimedOut=false;QTimer autoTimer;autoTimer.setInterval(20);QElapsedTimer autoElapsed;autoElapsed.start();
    QObject::connect(&autoTimer,&QTimer::timeout,&gesture,[&]{
        if(autoElapsed.elapsed()>5000){autoMenuTimedOut=true;autoTimer.stop();if(auto* popup=QApplication::activePopupWidget())popup->close();return;}
        auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu||menu->objectName()!=QStringLiteral("plotContextMenu"))return;
        for(auto* action:menu->actions())if(action->text()==QStringLiteral("自动幅值")){autoMenuSelected=true;autoTimer.stop();menu->setActiveAction(action);QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&enter);return;}
    });
    autoTimer.start();QContextMenuEvent autoContext(QContextMenuEvent::Mouse,contextPosition,gesture.mapToGlobal(contextPosition));QApplication::sendEvent(&gesture,&autoContext);autoTimer.stop();
    check("right_click_auto_amplitude_clears_manual_bounds",autoMenuSelected&&!autoMenuTimedOut&&!gesture.explicitYRange());
    check("all_mouse_navigation_preserves_original_samples_and_gap",gesture.series().size()==1&&samePoints(gesture.series()[0].points,source[0].points));
    // The current AnalysisPane uses SingleWaveformView. Keep the retained
    // stack/generic-canvas tests independently runnable without its obsolete
    // five-row AnalysisPane assumptions below; current live UI has its own
    // single_waveform, live_follow and touch_gestures probes.
    if(argc>1&&QByteArray(argv[1])=="--stack-only"){
        report["scope"]="Retained WaveformStack and generic PlotCanvas gesture regression; current SingleWaveformView live integration is covered separately.";
        report["passed"]=passed;std::puts(QJsonDocument(report).toJson(QJsonDocument::Indented).constData());return passed?0:1;
    }
    AnalysisPane pane;pane.resize(1280,650);pane.show();QString error;check("live_frame_accepted",pane.beginLive(frame(0,1),true,&error));QApplication::processEvents();
    if(!pane.isLive()){std::fprintf(stderr,"live test frame failed: %s\n",error.toUtf8().constData());return 1;}
    auto* livePlot=dynamic_cast<PlotCanvas*>(pane.findChild<QWidget*>("workbenchWaveCH1"));auto* speed=dynamic_cast<PlotCanvas*>(pane.findChild<QWidget*>("workbenchSpeed"));auto* liveStack=dynamic_cast<WaveformStack*>(pane.findChild<QWidget*>("waveformStack"));auto* liveSpeedRow=liveStack?liveStack->plot(4):nullptr;check("live_fifth_speed_row_available",liveSpeedRow!=nullptr);if(!liveSpeedRow)return 1;
    bool removedToolbar=true;for(const char* name:{"plotPanLeft","plotPanRight","plotZoomIn","plotZoomOut","plotNavigationMode","plotRestoreView","plotLatest","plotAutoY","plotApplyY","plotYLower","plotYUpper"})removedToolbar&=!pane.findChild<QWidget*>(QString::fromLatin1(name));check("added_navigation_amplitude_toolbar_removed",removedToolbar);
    livePlot->grab();wheel(*livePlot,livePlot->plotArea().center());const auto frozen=livePlot->series()[0].points;const auto frozenRange=livePlot->explicitXRange();const auto frozenSpeed=speed->series();const auto frozenSpeedRange=speed->explicitXRange();const auto frozenSpeedRow=liveSpeedRow->series();const auto frozenSpeedRowRange=liveSpeedRow->explicitXRange();
    check("live_fifth_trace_matches_speed_page_snapshot",frozenSpeedRow.size()==1&&frozenSpeed.size()==1&&samePoints(frozenSpeedRow[0].points,frozenSpeed[0].points));
    check("later_frame_accepted_while_paused",pane.acceptLiveFrame(frame(1000,2),&error));pane.setView(2);pane.setView(1);pane.setView(0);QApplication::processEvents();
    bool speedFrozen=speed->series().size()==frozenSpeed.size();for(int i=0;i<frozenSpeed.size()&&speedFrozen;++i)speedFrozen=speed->series()[i].points==frozenSpeed[i].points;
    check("live_navigation_freezes_all_pages",livePlot->series()[0].points==frozen&&livePlot->explicitXRange()==frozenRange&&speedFrozen&&speed->explicitXRange()==frozenSpeedRange);
    check("fifth_speed_trace_and_time_range_freeze_with_waveforms",liveSpeedRow->series().size()==1&&frozenSpeedRow.size()==1&&samePoints(liveSpeedRow->series()[0].points,frozenSpeedRow[0].points)&&liveSpeedRow->explicitXRange()==frozenSpeedRowRange&&liveSpeedRow->explicitXRange()==frozenRange);
    auto* note=pane.findChild<QLabel*>("plotNavigationNote");check("pause_explicit",note&&note->text().contains(QStringLiteral("暂停")));
    auto* stats=pane.findChild<QLabel*>("waveMetrics0");check("raw_stats_provenance",stats&&stats->toolTip().contains(QStringLiteral("原始预览可见窗"))&&stats->toolTip().contains("RMS"));
    auto* liveFollow=pane.findChild<QPushButton*>("liveFollow");check("existing_live_follow_exposes_resume",liveFollow&&liveFollow->text().contains(QStringLiteral("回到最新")));
    if(liveFollow)liveFollow->click();QApplication::processEvents();check("existing_live_follow_resumes_latest",livePlot->series()[0].points!=frozen&&note&&note->text().contains(QStringLiteral("实时跟随")));
    check("fifth_speed_trace_resumes_with_speed_page",liveSpeedRow->series().size()==1&&speed->series().size()==1&&frozenSpeedRow.size()==1&&!samePoints(liveSpeedRow->series()[0].points,frozenSpeedRow[0].points)&&samePoints(liveSpeedRow->series()[0].points,speed->series()[0].points));
    if(argc>1){pane.resize(1280,720);QApplication::processEvents();pane.grab().save(QString::fromLocal8Bit(argv[1]));}
    pane.endLive();report["passed"]=passed;std::puts(QJsonDocument(report).toJson(QJsonDocument::Indented).constData());return passed?0:1;
}
