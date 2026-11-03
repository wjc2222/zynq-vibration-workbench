#include "plot_widget.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QThread>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using vibration::gui::PlotWidget;
using vibration::gui::PlotSeries;
namespace {
const QColor trace("#3f6f83");
QVector<PlotSeries> curves() {
    QVector<PlotSeries> output;
    for (int channel=0;channel<3;++channel) {
        PlotSeries s{QString("CH%1").arg(channel+1),channel==0?trace:QColor(channel==1?"#668672":"#aa8052"),{}};
        for(int i=0;i<1800;++i)s.points.push_back({i/1799.0, std::sin(i*.05+channel)*(channel+1)});
        output.push_back(std::move(s));
    }
    return output;
}
void mouse(PlotWidget& plot,QEvent::Type type,QPointF pos,Qt::MouseButton button,Qt::MouseButtons buttons) {
    QMouseEvent event(type,pos,button,buttons,Qt::NoModifier);QApplication::sendEvent(&plot,&event);
}
QImage image(PlotWidget& plot) { QApplication::processEvents();return plot.grab().toImage().convertToFormat(QImage::Format_RGB32); }
qint64 percentile(std::vector<qint64> values,double fraction) {
    std::sort(values.begin(),values.end());return values[std::size_t((values.size()-1)*fraction)];
}
void screenshots(PlotWidget& plot,const QString& destination) {
    QDir().mkpath(destination);
    for (const auto& size:{QSize(1024,600),QSize(1280,720)}) {
        plot.resize(size);plot.setCompact(false);plot.setLegendVisible(true);plot.setZeroBasedY(false);
        plot.setChart("Trace", "Time / s", "g",curves());plot.clearXRange();plot.setReferenceLines({{.25,1},{.75,3}});
        const auto suffix=QString("-%1x%2.png").arg(size.width()).arg(size.height());
        image(plot).save(destination+"/curves"+suffix);
        plot.setCursor(.4);image(plot).save(destination+"/cursor"+suffix);
        plot.setChannelVisible(1,false);plot.setXRange(.21,.43);image(plot).save(destination+"/range"+suffix);
        plot.setCompact(true);image(plot).save(destination+"/compact"+suffix);
        plot.setCompact(false);plot.setChart("Empty","Time / s","g",{},"No data");image(plot).save(destination+"/empty"+suffix);
        const double nan=std::numeric_limits<double>::quiet_NaN();
        plot.setChart("Gap","Time / s","g",{{"CH1",trace,{{0,0},{1,1},{2,nan},{3,1},{4,0}}}});plot.clearXRange();plot.setReferenceLines({});
        image(plot).save(destination+"/nan"+suffix);
    }
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);QFont font("sans-serif");font.setPixelSize(14);app.setFont(font);
    PlotWidget plot;plot.resize(1024,600);plot.show();
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--image-only") {screenshots(plot,QString::fromLocal8Bit(argv[2]));return 0;}
#ifdef VIBRATION_LEGACY_PLOT
    return 2;
#else
    QJsonObject result;bool passed=true;
    auto check=[&](const char* name,bool ok){result.insert(name,ok);passed=passed&&ok;};
    plot.setChart("Trace","Time / s","g",curves());plot.setReferenceLines({{.25,1}});
    auto clean=image(plot);auto builds=plot.renderStats().staticBuilds;
    plot.setCursor(.4);auto cursor=image(plot);
    check("cursor_reuses_static_layer",plot.renderStats().staticBuilds==builds&&cursor!=clean);
    plot.setCursor({});check("overlay_erases_without_trace_change",image(plot)==clean&&plot.renderStats().staticBuilds==builds);
    plot.setChannelVisible(1,false);auto hidden=image(plot);check("channel_invalidates",hidden!=clean&&plot.renderStats().staticBuilds==++builds);
    plot.setChannelVisible(1,true);check("channel_restore_exact",image(plot)==clean&&plot.renderStats().staticBuilds==++builds);
    plot.setReferenceLines({{.3,2}});image(plot);check("reference_invalidates",plot.renderStats().staticBuilds==++builds);
    plot.setXRange(.2,.4);auto ranged=image(plot);check("range_invalidates",plot.renderStats().staticBuilds==++builds&&ranged!=clean);
    plot.setXRange(.2,.4);plot.setChannelVisible(0,true);image(plot);check("identical_setters_reuse_cache",plot.renderStats().staticBuilds==builds);
    plot.setZeroBasedY(true);image(plot);check("zero_based_invalidates",plot.renderStats().staticBuilds==++builds);
    plot.setLegendVisible(false);image(plot);check("legend_invalidates",plot.renderStats().staticBuilds==++builds);
    plot.setCompact(true);image(plot);check("compact_invalidates",plot.renderStats().staticBuilds==++builds);
    auto changedFont=plot.font();changedFont.setFamily("monospace");plot.setFont(changedFont);image(plot);check("font_invalidates",plot.renderStats().staticBuilds==++builds);
    plot.resize(1280,720);image(plot);check("resize_invalidates",plot.renderStats().staticBuilds==++builds);
    plot.setChart("Empty","Time / s","g",{},"No data");auto empty=image(plot);check("empty_data_invalidates",plot.renderStats().staticBuilds==++builds&&empty!=ranged);
    const double nan=std::numeric_limits<double>::quiet_NaN();
    plot.setCompact(false);plot.setFont(font);plot.setLegendVisible(false);plot.setReferenceLines({});plot.clearXRange();
    plot.setChart("Gap","Time / s","g",{{"CH1",trace,{{0,0},{1,1},{2,nan},{3,1},{4,0}}}});
    auto gap=image(plot);const double dpr=gap.devicePixelRatio();const int middle=qRound(plot.plotArea().center().x()*dpr);int tracePixels=0;
    for(int x=middle-8;x<=middle+8;++x)for(int y=qCeil(plot.plotArea().top()*dpr);y<=qFloor(plot.plotArea().bottom()*dpr);++y)if(gap.pixelColor(x,y)==trace)++tracePixels;
    check("nan_gap_has_no_crossing",tracePixels==0);
    plot.setChart("Trace","Time / s","g",curves());plot.setXRange(0,1);image(plot);
    int callbacks=0;double selected=-1;
    plot.cursorSelected=[&](double x){++callbacks;selected=x;plot.setCursor(x);};
    const auto area=plot.plotArea();
    mouse(plot,QEvent::MouseButtonPress,area.center(),Qt::LeftButton,Qt::LeftButton);
    for(int i=0;i<200;++i)mouse(plot,QEvent::MouseMove,{area.left()+area.width()*i/200,area.center().y()},Qt::NoButton,Qt::LeftButton);
    const int beforeRelease=callbacks;
    mouse(plot,QEvent::MouseButtonRelease,{area.right()+100,area.center().y()},Qt::LeftButton,Qt::NoButton);
    check("mouse_moves_are_coalesced",beforeRelease==1);
    check("mouse_release_is_exact_and_clamped",callbacks==2&&selected==1);
    QThread::msleep(20);app.processEvents();check("release_cancels_pending_move",callbacks==2);
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
    QTouchDevice device;device.setType(QTouchDevice::TouchScreen);device.setCapabilities(QTouchDevice::Position);device.setMaximumTouchPoints(1);
    auto touch=[&](QEvent::Type type,Qt::TouchPointState state,QPointF position){QTouchEvent::TouchPoint point(0);point.setState(state);point.setPos(position);point.setScreenPos(plot.mapToGlobal(position.toPoint()));QTouchEvent event(type,&device,Qt::NoModifier,state,{point});QApplication::sendEvent(&plot,&event);};
    callbacks=0;touch(QEvent::TouchBegin,Qt::TouchPointPressed,area.center());
    touch(QEvent::TouchUpdate,Qt::TouchPointMoved,{area.left()+area.width()*.2,area.center().y()});
    touch(QEvent::TouchEnd,Qt::TouchPointReleased,{area.left()-100,area.center().y()});
    check("single_touch_release_is_exact_and_clamped",callbacks==2&&selected==0);
#endif
    std::vector<qint64> cold,warm;
    for(int i=0;i<110;++i){plot.setReferenceLines({{.2+i*.0001,1}});plot.repaint();if(i>=10)cold.push_back(plot.renderStats().lastPaintNs);}
    builds=plot.renderStats().staticBuilds;
    for(int i=0;i<310;++i){plot.setCursor(.1+(i%280)/350.0);app.processEvents();if(i>=10)warm.push_back(plot.renderStats().lastPaintNs);}
    check("warm_cursor_has_zero_static_rebuilds",plot.renderStats().staticBuilds==builds);
    result.insert("cold_paint_p50_ns",percentile(cold,.5));result.insert("cold_paint_p95_ns",percentile(cold,.95));
    result.insert("warm_cursor_paint_p50_ns",percentile(warm,.5));result.insert("warm_cursor_paint_p95_ns",percentile(warm,.95));
    result.insert("cold_samples",int(cold.size()));result.insert("warm_samples",int(warm.size()));result.insert("device_pixel_ratio",plot.devicePixelRatioF());
    result.insert("measurement_scope","Linux x86_64 host QWidget paint only; no ARM or event-to-panel latency claim");result.insert("passed",passed);
    const auto json=QJsonDocument(result).toJson();std::fwrite(json.data(),1,std::size_t(json.size()),stdout);return passed?0:1;
#endif
}
