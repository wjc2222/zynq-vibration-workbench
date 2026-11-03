#include "analysis_pane.hpp"
#include "plot_canvas.hpp"
#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QTouchDevice>
#include <QTouchEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
using namespace vibration::workbench;
namespace {
bool near(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<1e-7;}
bool range(const PlotCanvas& p,double a,double b){auto r=p.explicitXRange();return r&&near(r->x(),a)&&near(r->y(),b);}
bool finiteRange(const std::optional<QPointF>& r){return r&&std::isfinite(r->x())&&std::isfinite(r->y())&&r->y()>r->x();}
bool samePoints(const QVector<QPointF>& a,const QVector<QPointF>& b){if(a.size()!=b.size())return false;for(int i=0;i<a.size();++i)if(a[i].x()!=b[i].x()||(!(std::isnan(a[i].y())&&std::isnan(b[i].y()))&&a[i].y()!=b[i].y()))return false;return true;}
void pump(int milliseconds){QElapsedTimer t;t.start();while(t.elapsed()<milliseconds){QApplication::processEvents();QThread::msleep(2);}QApplication::processEvents();}
struct Finger {int id;QPointF position;Qt::TouchPointState state;};
// Actual Qt touch events, with stable contact IDs and complete current contact
// sets. No gesture method or private implementation field is called by tests.
class Touch {
public:
    explicit Touch(PlotCanvas& plot):plot_(plot){device_.setType(QTouchDevice::TouchScreen);device_.setCapabilities(QTouchDevice::Position);device_.setMaximumTouchPoints(10);}
    void send(QEvent::Type type,std::initializer_list<Finger> fingers){
        QList<QTouchEvent::TouchPoint> points;Qt::TouchPointStates states;
        for(const auto& f:fingers){QTouchEvent::TouchPoint p(f.id);auto old=positions_.find(f.id);const auto start=old==positions_.end()?f.position:starts_[f.id];const auto last=old==positions_.end()?f.position:old->second;
            p.setState(f.state);p.setPos(f.position);p.setStartPos(start);p.setLastPos(last);p.setScreenPos(plot_.mapToGlobal(f.position.toPoint()));p.setStartScreenPos(plot_.mapToGlobal(start.toPoint()));p.setLastScreenPos(plot_.mapToGlobal(last.toPoint()));p.setPressure(f.state==Qt::TouchPointReleased?0:1);points.push_back(p);states|=f.state;
            starts_[f.id]=start;positions_[f.id]=f.position;
        }
        QTouchEvent event(type,&device_,Qt::NoModifier,states,points);QApplication::sendEvent(&plot_,&event);
        for(const auto& f:fingers)if(f.state==Qt::TouchPointReleased){positions_.erase(f.id);starts_.erase(f.id);}
        if(type==QEvent::TouchEnd||type==QEvent::TouchCancel){positions_.clear();starts_.clear();}
    }
    void cancel(){send(QEvent::TouchCancel,{});}
private:PlotCanvas& plot_;QTouchDevice device_;std::map<int,QPointF> positions_,starts_;
};
void mouse(PlotCanvas& p,QEvent::Type type,QPointF pos,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent e(type,pos,button,buttons,Qt::NoModifier);QApplication::sendEvent(&p,&e);}
void put32(unsigned char* p,quint32 n){for(int i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(i*8));}
void put64(unsigned char* p,quint64 n){put32(p,quint32(n));put32(p+4,quint32(n>>32));}
QJsonObject frame(quint64 first,quint64 sequence,int rawValue){
    QByteArray raw(2000*32,'\0');for(int i=0;i<2000;++i){auto* p=reinterpret_cast<unsigned char*>(raw.data())+i*32;put64(p,first+quint64(i));put64(p+8,1000000+(first+quint64(i))*1000);for(int ch=0;ch<3;++ch){const auto value=quint16(rawValue+ch*1000);p[16+ch*2]=static_cast<unsigned char>(value);p[17+ch*2]=static_cast<unsigned char>(value>>8);}p[22]=1;}
    const QString session(32,'1');return {{"schema","vibration.stream.v1"},{"type","event"},{"event","preview"},{"emitter","stream_service"},{"id","touch-test"},{"request_id","touch-test"},{"job_id",session},{"session_id",session},{"source","test_fixture"},{"synthetic",true},{"hardware_confirmed",false},{"frame_seq",QString::number(sequence)},{"global_first_index",QString::number(first)},{"first_tick",QString::number(1000000+first*1000)},{"last_tick",QString::number(1000000+(first+1999)*1000)},{"record_count",2000},{"sample_rate_hz",50000},{"clock_hz",50000000},{"ppr",1},{"adc_range_uv",5000000},{"channel_map",0x50301},{"record_bytes",32},{"total_records","0"},{"crc32",double(vibration::crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()),std::size_t(raw.size())))},{"raw_base64",QString::fromLatin1(raw.toBase64())}};
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);bool passed=true;QJsonObject checks;
    auto check=[&](const char* name,bool value){checks[name]=value;passed&=value;};
    PlotSeries original{QStringLiteral("CH1"),QColor("#3569c8"),{}};for(int i=0;i<=1000;++i)original.points.push_back({i/100.,std::sin(i/10.)});original.points[500].setY(std::numeric_limits<double>::quiet_NaN());
    PlotCanvas plot;plot.resize(1000,400);plot.setChart({},"s","V",{original});plot.setNavigationBounds(0,10);plot.show();QApplication::processEvents();plot.grab();Touch touch(plot);int navigations=0;plot.navigationStarted=[&]{++navigations;};
    auto reset=[&]{touch.cancel();plot.setNavigationBounds(0,10);plot.setXRange(2,8);plot.setYRange(-2,6);plot.grab();navigations=0;};
    auto at=[&](double x,double y=.5){const auto a=plot.plotArea();return QPointF(a.left()+a.width()*x,a.top()+a.height()*y);};
    reset();auto p=at(.4),q=at(.5);touch.send(QEvent::TouchBegin,{{7,p,Qt::TouchPointPressed}});check("single_press_does_not_start_navigation",navigations==0&&range(plot,2,8));touch.send(QEvent::TouchUpdate,{{7,q,Qt::TouchPointMoved}});touch.send(QEvent::TouchEnd,{{7,q,Qt::TouchPointReleased}});check("single_finger_pan_maps_pixels_to_time",range(plot,1.4,7.4)&&navigations==1);
    // Deterministic equivalent of pauseNavigation adopting a newer live frame
    // between pointer-down and the first movement. The first delta must use
    // these freshly frozen ranges, then keep that base for later movement.
    auto adoptNewWindow=[&]{if(++navigations==1){plot.setNavigationBounds(10,40);plot.setXRange(20,30);plot.setYRange(10,30);}};
    reset();plot.navigationStarted=adoptNewWindow;p=at(.4);q=at(.5);
    touch.send(QEvent::TouchBegin,{{71,p,Qt::TouchPointPressed}});
    touch.send(QEvent::TouchUpdate,{{71,q,Qt::TouchPointMoved}});
    check("first_touch_pan_uses_range_adopted_by_freeze_callback",range(plot,19,29)&&navigations==1);
    q=at(.6);touch.send(QEvent::TouchUpdate,{{71,q,Qt::TouchPointMoved}});touch.send(QEvent::TouchEnd,{{71,q,Qt::TouchPointReleased}});
    check("later_touch_pan_keeps_frozen_base_and_single_callback",range(plot,18,28)&&navigations==1);
    reset();const auto axis=plot.plotArea();p=QPointF(axis.left()*.5,axis.top()+axis.height()*.4);q=QPointF(p.x(),axis.top()+axis.height()*.5);
    touch.send(QEvent::TouchBegin,{{72,p,Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{72,q,Qt::TouchPointMoved}});
    auto refreshedY=plot.explicitYRange();check("first_touch_y_pan_uses_amplitude_adopted_by_freeze_callback",refreshedY&&near(refreshedY->x(),12)&&near(refreshedY->y(),32)&&range(plot,20,30)&&navigations==1);
    q.setY(axis.top()+axis.height()*.6);touch.send(QEvent::TouchUpdate,{{72,q,Qt::TouchPointMoved}});touch.send(QEvent::TouchEnd,{{72,q,Qt::TouchPointReleased}});
    refreshedY=plot.explicitYRange();check("later_touch_y_pan_keeps_frozen_amplitude_base",refreshedY&&near(refreshedY->x(),14)&&near(refreshedY->y(),34)&&navigations==1);
    reset();p=at(.4);q=at(.5);mouse(plot,QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton);mouse(plot,QEvent::MouseMove,q,Qt::NoButton,Qt::LeftButton);mouse(plot,QEvent::MouseButtonRelease,q,Qt::LeftButton,Qt::NoButton);
    check("first_mouse_pan_also_uses_range_adopted_by_freeze_callback",range(plot,19,29)&&navigations==1);
    plot.navigationStarted=[&]{++navigations;};
    reset();touch.send(QEvent::TouchBegin,{{12,at(.2),Qt::TouchPointPressed},{39,at(.6),Qt::TouchPointPressed}});check("two_finger_press_does_not_mutate",range(plot,2,8));touch.send(QEvent::TouchUpdate,{{12,at(.1),Qt::TouchPointMoved},{39,at(.7),Qt::TouchPointMoved}});check("horizontal_spread_zooms_in_at_initial_centroid",range(plot,2.8,6.8)&&plot.explicitYRange()==std::optional<QPointF>(QPointF(-2,6))&&navigations==1);touch.send(QEvent::TouchUpdate,{{12,at(.25),Qt::TouchPointMoved},{39,at(.55),Qt::TouchPointMoved}});check("horizontal_close_zooms_out_without_incremental_drift",range(plot,1.2,9.2)&&navigations==1);touch.send(QEvent::TouchEnd,{{12,at(.25),Qt::TouchPointReleased},{39,at(.55),Qt::TouchPointReleased}});
    reset();touch.send(QEvent::TouchBegin,{{4,at(.5,.3),Qt::TouchPointPressed},{18,at(.5,.7),Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{4,at(.5,.2),Qt::TouchPointMoved},{18,at(.5,.8),Qt::TouchPointMoved}});auto y=plot.explicitYRange();check("vertical_spread_scales_amplitude_around_anchor",y&&near(y->x(),-2./3.)&&near(y->y(),14./3.)&&range(plot,2,8)&&navigations==1);touch.cancel();
    reset();touch.send(QEvent::TouchBegin,{{51,at(.4),Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{51,at(.45),Qt::TouchPointMoved}});auto beforeLanding=plot.explicitXRange();touch.send(QEvent::TouchUpdate,{{51,at(.45),Qt::TouchPointStationary},{3,at(.7),Qt::TouchPointPressed}});check("second_finger_landing_rebases_without_jump",plot.explicitXRange()==beforeLanding);touch.send(QEvent::TouchUpdate,{{3,at(.7),Qt::TouchPointStationary},{51,at(.45),Qt::TouchPointStationary}});check("touch_list_reordering_preserves_contact_identity",plot.explicitXRange()==beforeLanding);touch.send(QEvent::TouchUpdate,{{3,at(.75),Qt::TouchPointMoved},{51,at(.4),Qt::TouchPointMoved}});auto afterSpread=plot.explicitXRange();const double anchor=1.7+.575*6;const double width=6*.25/.35;check("reordered_ids_keep_pinch_anchor_and_scale",afterSpread&&near(afterSpread->x(),anchor-.575*width)&&near(afterSpread->y(),anchor+.425*width)&&navigations==1);touch.send(QEvent::TouchUpdate,{{51,at(.4),Qt::TouchPointStationary},{3,at(.75),Qt::TouchPointReleased}});check("second_finger_release_has_no_jump",plot.explicitXRange()==afterSpread);touch.send(QEvent::TouchUpdate,{{51,at(.6),Qt::TouchPointMoved}});touch.send(QEvent::TouchEnd,{{51,at(.6),Qt::TouchPointReleased}});check("pinch_to_one_finger_requires_new_sequence",plot.explicitXRange()==afterSpread&&navigations==1);
    reset();touch.send(QEvent::TouchBegin,{{1,at(.5),Qt::TouchPointPressed},{2,at(.5),Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{2,at(.5),Qt::TouchPointStationary},{1,at(.5),Qt::TouchPointStationary}});check("coincident_contacts_do_not_divide_by_zero",range(plot,2,8)&&finiteRange(plot.explicitYRange()));touch.send(QEvent::TouchUpdate,{{1,at(.3),Qt::TouchPointMoved},{2,at(.7),Qt::TouchPointMoved}});check("zero_distance_to_spread_stays_finite",finiteRange(plot.explicitXRange())&&finiteRange(plot.explicitYRange()));touch.cancel();
    reset();touch.send(QEvent::TouchBegin,{{9,at(.2),Qt::TouchPointPressed},{10,at(.6),Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{9,at(.1),Qt::TouchPointMoved},{10,at(.7),Qt::TouchPointMoved}});touch.send(QEvent::TouchUpdate,{{9,at(.4),Qt::TouchPointMoved},{10,at(.4),Qt::TouchPointMoved}});check("collapse_to_zero_distance_keeps_valid_bounds",finiteRange(plot.explicitXRange())&&finiteRange(plot.explicitYRange()));auto cancelled=plot.explicitXRange();touch.cancel();touch.send(QEvent::TouchUpdate,{{9,at(.1),Qt::TouchPointMoved},{10,at(.9),Qt::TouchPointMoved}});check("touch_cancel_ends_sequence_and_ignores_stale_update",plot.explicitXRange()==cancelled);touch.cancel();
    reset();p=at(.4);q=at(.5);touch.send(QEvent::TouchBegin,{{5,p,Qt::TouchPointPressed}});touch.cancel();touch.send(QEvent::TouchBegin,{{8,p,Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{8,q,Qt::TouchPointMoved}});touch.send(QEvent::TouchEnd,{{8,q,Qt::TouchPointReleased}});check("fresh_touch_after_cancel_has_clean_origin",range(plot,1.4,7.4));
    bool sawMenu=false;QTimer menuGuard;menuGuard.setInterval(5);QObject::connect(&menuGuard,&QTimer::timeout,&app,[&]{if(auto* m=qobject_cast<QMenu*>(QApplication::activePopupWidget())){sawMenu=true;m->close();}});menuGuard.start();
    reset();touch.send(QEvent::TouchBegin,{{21,at(.4),Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{21,at(.5),Qt::TouchPointMoved}});pump(650);check("pan_movement_cancels_long_press_menu",!sawMenu);touch.cancel();sawMenu=false;
    reset();touch.send(QEvent::TouchBegin,{{21,at(.4),Qt::TouchPointPressed}});touch.send(QEvent::TouchUpdate,{{21,at(.4),Qt::TouchPointStationary},{22,at(.6),Qt::TouchPointPressed}});pump(650);check("second_finger_cancels_long_press_menu",!sawMenu);touch.cancel();sawMenu=false;
    reset();touch.send(QEvent::TouchBegin,{{21,at(.4),Qt::TouchPointPressed}});touch.cancel();pump(650);check("touch_cancel_cancels_long_press_menu",!sawMenu);sawMenu=false;
    reset();touch.send(QEvent::TouchBegin,{{21,at(.4),Qt::TouchPointPressed}});pump(650);check("stationary_long_press_still_opens_menu",sawMenu);touch.send(QEvent::TouchEnd,{{21,at(.4),Qt::TouchPointReleased}});menuGuard.stop();
    reset();p=at(.45);touch.send(QEvent::TouchBegin,{{31,p,Qt::TouchPointPressed}});touch.send(QEvent::TouchEnd,{{31,p,Qt::TouchPointReleased}});pump(50);touch.send(QEvent::TouchBegin,{{32,p,Qt::TouchPointPressed}});touch.send(QEvent::TouchEnd,{{32,p,Qt::TouchPointReleased}});check("actual_touch_double_tap_restores_full_x_auto_y",range(plot,0,10)&&!plot.explicitYRange());
    reset();p=at(.5);QWheelEvent wheel(p,p,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(&plot,&wheel);check("mouse_wheel_still_zooms_at_pointer",range(plot,2.6,7.4));reset();p=at(.4);q=at(.5);mouse(plot,QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton);mouse(plot,QEvent::MouseMove,q,Qt::NoButton,Qt::LeftButton);mouse(plot,QEvent::MouseButtonRelease,q,Qt::LeftButton,Qt::NoButton);check("mouse_pan_still_works_after_touch",range(plot,1.4,7.4));check("all_touch_and_mouse_navigation_preserves_original_samples_and_nan_gap",plot.series().size()==1&&samePoints(plot.series()[0].points,original.points));
    AnalysisPane pane;pane.resize(1024,600);pane.show();pane.setView(0);QString error;const bool began=pane.beginLive(frame(0,1,1000),true,&error);check("synthetic_live_fixture_accepted",began);QApplication::processEvents();auto* wave=dynamic_cast<SingleWaveformView*>(pane.findChild<QWidget*>("singleWaveformView"));auto* note=pane.findChild<QLabel*>("plotNavigationNote");auto* follow=pane.findChild<QPushButton*>("liveFollow");check("actual_live_waveform_controls_available",wave&&note&&follow);
    if(began&&wave&&note&&follow){auto* livePlot=wave->plot();livePlot->grab();const auto a=livePlot->plotArea();auto liveAt=[&](double x){return QPointF(a.left()+a.width()*x,a.center().y());};Touch liveTouch(*livePlot);liveTouch.send(QEvent::TouchBegin,{{40,liveAt(.3),Qt::TouchPointPressed},{90,liveAt(.7),Qt::TouchPointPressed}});liveTouch.send(QEvent::TouchUpdate,{{40,liveAt(.2),Qt::TouchPointMoved},{90,liveAt(.8),Qt::TouchPointMoved}});liveTouch.send(QEvent::TouchEnd,{{40,liveAt(.2),Qt::TouchPointReleased},{90,liveAt(.8),Qt::TouchPointReleased}});const auto frozen=livePlot->series();const auto frozenRange=livePlot->explicitXRange();check("touch_pinch_explicitly_pauses_live_follow",note->text().contains(QStringLiteral("暂停")));check("later_synthetic_live_frame_accepted",pane.acceptLiveFrame(frame(100000,2,10000),&error));pump(40);bool unchanged=livePlot->series().size()==frozen.size();for(int i=0;i<frozen.size()&&unchanged;++i)unchanged=samePoints(livePlot->series()[i].points,frozen[i].points);check("pinched_live_snapshot_and_range_remain_frozen",unchanged&&livePlot->explicitXRange()==frozenRange);follow->click();pump(40);check("existing_follow_button_resumes_latest_after_touch",!note->text().contains(QStringLiteral("暂停"))&&livePlot->explicitXRange()!=frozenRange&&!samePoints(livePlot->series()[0].points,frozen[0].points));}
    if(began&&wave&&note){
        pane.endLive();const bool restarted=pane.beginLive(frame(0,1,1000),true,&error);check("delayed_pan_synthetic_live_fixture_restarted",restarted);QApplication::processEvents();
        if(restarted){auto* livePlot=wave->plot();livePlot->grab();const auto a=livePlot->plotArea();const QPointF first(a.left()+a.width()*.4,a.center().y()),last(a.left()+a.width()*.5,a.center().y());Touch delayed(*livePlot);
            delayed.send(QEvent::TouchBegin,{{81,first,Qt::TouchPointPressed}});
            // Deliberately avoid pumping the timer: first movement itself must
            // freeze/adopt the frame accepted after this contact went down.
            check("later_frame_arrives_between_press_and_first_single_pan",pane.acceptLiveFrame(frame(100000,2,10000),&error));
            delayed.send(QEvent::TouchUpdate,{{81,last,Qt::TouchPointMoved}});delayed.send(QEvent::TouchEnd,{{81,last,Qt::TouchPointReleased}});
            const auto frozen=livePlot->explicitXRange();const auto bounds=livePlot->navigationBounds();
            check("actual_live_first_single_pan_freezes_new_window_without_old_span_zoom",frozen&&near(frozen->x(),1.78998)&&near(frozen->y(),2.03998)&&near(bounds.x(),frozen->x())&&near(bounds.y(),frozen->y())&&note->text().contains(QStringLiteral("暂停")));
        }
    }
    pane.endLive();QJsonObject report{{"schema","vibration.touch.host-qa.v1"},{"synthetic",true},{"hardware_confirmed",false},{"scope","Qt5 injected QTouchEvent widget regression; no physical touchscreen or acquisition acceptance"},{"passed",passed},{"fixture_error",error},{"checks",checks}};std::puts(QJsonDocument(report).toJson(QJsonDocument::Indented).constData());return passed?0:1;
}
