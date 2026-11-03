#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QSysInfo>
#include <QTouchDevice>
#include <QTouchEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace vibration::workbench;
namespace {
bool near(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<=1e-10*std::max({1.,std::abs(a),std::abs(b)});}
bool symmetric(const QPointF& range){return range.y()>0&&near(range.x(),-range.y());}
bool same(const QVector<QPointF>& a,const QVector<QPointF>& b){
    if(a.size()!=b.size())return false;
    for(int i=0;i<a.size();++i){const double ax=a[i].x(),ay=a[i].y(),bx=b[i].x(),by=b[i].y();
        if(std::memcmp(&ax,&bx,sizeof(double))||std::memcmp(&ay,&by,sizeof(double)))return false;}
    return true;
}
void paint(QWidget& widget){QApplication::processEvents();widget.repaint();QApplication::processEvents();}
void mouse(PlotCanvas& plot,QEvent::Type type,QPointF pos,Qt::MouseButton button,Qt::MouseButtons buttons){
    QMouseEvent event(type,pos,button,buttons,Qt::NoModifier);QApplication::sendEvent(&plot,&event);
}
void drag(PlotCanvas& plot,QPointF a,QPointF b){mouse(plot,QEvent::MouseButtonPress,a,Qt::LeftButton,Qt::LeftButton);mouse(plot,QEvent::MouseMove,b,Qt::NoButton,Qt::LeftButton);mouse(plot,QEvent::MouseButtonRelease,b,Qt::LeftButton,Qt::NoButton);}
void pinch(PlotCanvas& plot,bool vertical){
    QTouchDevice device;device.setType(QTouchDevice::TouchScreen);device.setCapabilities(QTouchDevice::Position);device.setMaximumTouchPoints(2);
    const auto area=plot.plotArea();
    const auto point=[&](double amount){return vertical?QPointF(area.center().x(),area.top()+area.height()*amount):QPointF(area.left()+area.width()*amount,area.center().y());};
    const auto send=[&](QEvent::Type type,double low,double high,Qt::TouchPointState state){
        QList<QTouchEvent::TouchPoint> points;
        for(int id=0;id<2;++id){QTouchEvent::TouchPoint finger(id+10);const auto pos=point(id?high:low);finger.setState(state);finger.setPos(pos);finger.setStartPos(point(id?.65:.35));finger.setLastPos(pos);finger.setScreenPos(plot.mapToGlobal(pos.toPoint()));points.push_back(finger);}
        QTouchEvent event(type,&device,Qt::NoModifier,state,points);QApplication::sendEvent(&plot,&event);
    };
    send(QEvent::TouchBegin,.35,.65,Qt::TouchPointPressed);send(QEvent::TouchUpdate,.25,.75,Qt::TouchPointMoved);send(QEvent::TouchEnd,.25,.75,Qt::TouchPointReleased);
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);bool passed=true;QJsonObject checks;
    const QString evidence=argc>1?QString::fromUtf8(argv[1]):QString();if(!evidence.isEmpty()&&!QDir().mkpath(evidence))return 2;
    const auto check=[&](const QString& name,bool value){checks[name]=value;passed&=value;};
    const auto save=[&](QWidget& widget,const QString& name){if(!evidence.isEmpty())check(name+"_screenshot",widget.grab().save(evidence+"/"+name+".png"));};
    const double nan=std::numeric_limits<double>::quiet_NaN();
    SingleWaveformView view;view.resize(1020,600);view.setNavigationBounds(0,10);view.setXRange(0,10);view.show();
    int unitIndex=0;
    for(const QString& unit:{QStringLiteral("V"),QStringLiteral("m/s²"),QStringLiteral("g（名义）")}){
        const double scale=unitIndex==0?1.:unitIndex==1?9.80665:.1;
        PlotSeries source{"CH1",QColor("#3769c7"),{{-1,1e9},{0,scale},{2.5,2*scale},{5,3*scale},{6,nan},{7.5,4*scale},{10,2*scale},{11,-1e9}}};
        view.setSourceSeries({source},unit);view.plot()->clearYRange();paint(view);const auto range=view.plot()->displayedYRange();
        check(unit+" positive-only DC window symmetric and all values inside",view.plot()->zeroCenteredY()&&symmetric(range)&&range.y()>=4*scale&&range.y()<5*scale);
        check(unit+" zero tick at middle",view.plot()->yTickLabels().size()==3&&view.plot()->yTickLabels()[1]==QStringLiteral("0"));
        check(unit+" raw coordinates and NaN payload bitwise unchanged",same(view.sourceSeries()[0].points,source.points)&&same(view.plot()->series()[0].points,source.points));
        save(view,QStringLiteral("positive-unit-%1").arg(unitIndex++));
    }
    PlotSeries negative{"CH1",QColor("#3769c7"),{{0,-2},{5,-8},{6,nan},{10,-4}}};
    view.setSourceSeries({negative},"V");paint(view);check("negative-only signal and NaN retain original values",symmetric(view.plot()->displayedYRange())&&view.plot()->displayedYRange().y()>=8&&same(view.sourceSeries()[0].points,negative.points));save(view,"negative-only");
    for(double dc:{0.,5.,-5.}){PlotSeries line{"CH1",QColor("#3769c7"),{{0,dc},{10,dc}}};view.setSourceSeries({line},"V");paint(view);check(QStringLiteral("constant %1 remains centered without demeaning").arg(dc),symmetric(view.plot()->displayedYRange())&&view.plot()->displayedYRange().y()>=std::abs(dc)&&same(view.plot()->series()[0].points,line.points));}
    PlotSeries positive{"CH1",QColor("#3769c7"),{{0,1},{5,3},{6,nan},{10,2}}};
    view.setSourceSeries({positive},"V");auto* plot=view.plot();plot->setYRange(-1,3);paint(view);
    check("asymmetric manual request normalized to maximum absolute limit",plot->explicitYRange()==std::optional<QPointF>(QPointF(-3,3)));
    const auto xBefore=plot->explicitXRange();plot->zoomY(.5,2.5);paint(view);check("manual zoom ignores off-center anchor and preserves time",plot->explicitYRange()==std::optional<QPointF>(QPointF(-1.5,1.5))&&plot->explicitXRange()==xBefore);
    plot->setYRange(-4,4);paint(view);auto area=plot->plotArea();
    const QPointF wheelAt(std::max(2.,area.left()*.5),area.top()+area.height()*.2);
    QWheelEvent wheel(wheelAt,wheelAt,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(plot,&wheel);paint(view);
    check("gutter wheel retains zero center and X bounds",symmetric(plot->displayedYRange())&&near(plot->displayedYRange().y(),3.2)&&plot->explicitXRange()==xBefore);
    area=plot->plotArea();const double beforeDrag=plot->displayedYRange().y();drag(*plot,{area.left()*.5,area.top()+area.height()*.25},{area.left()*.5,area.top()+area.height()*.5});paint(view);
    check("gutter drag scales around zero and preserves X",symmetric(plot->displayedYRange())&&near(plot->displayedYRange().y(),beforeDrag*std::sqrt(2.))&&plot->explicitXRange()==xBefore);
    plot->setYRange(-4,4);paint(view);pinch(*plot,true);paint(view);
    check("vertical touch pinch scales around zero rather than centroid",symmetric(plot->displayedYRange())&&near(plot->displayedYRange().y(),2.4)&&plot->explicitXRange()==xBefore);
    plot->setXRange(2,8);paint(view);const auto yBeforeHorizontal=plot->explicitYRange();pinch(*plot,false);paint(view);auto x=plot->explicitXRange();
    check("horizontal pinch retains original time anchor and Y",x&&near(x->x(),3.2)&&near(x->y(),6.8)&&plot->explicitYRange()==yBeforeHorizontal);
    plot->setXRange(2,8);paint(view);area=plot->plotArea();drag(*plot,{area.left()+area.width()*.4,area.center().y()},{area.left()+area.width()*.5,area.center().y()});paint(view);x=plot->explicitXRange();
    check("horizontal drag remains ordinary time pan",x&&near(x->x(),1.4)&&near(x->y(),7.4)&&plot->explicitYRange()==yBeforeHorizontal);
    check("all manual and touch gestures preserve source bits and missing span",same(view.sourceSeries()[0].points,positive.points)&&same(plot->series()[0].points,positive.points));save(view,"manual-centered");
    view.setSpeedSeries({"RPM",QColor("#3769c7"),{{0,1000},{5,1100},{10,1200}}},"RPM");view.setSpeedSelected(true);paint(view);
    check("speed auto axis remains independent and positive",!plot->zeroCenteredY()&&plot->displayedYRange().x()>0);plot->setYRange(1000,2000);paint(view);save(view,"speed-positive");view.setSpeedSelected(false);paint(view);
    check("returning from speed restores centered vibration range",plot->zeroCenteredY()&&plot->explicitYRange()==yBeforeHorizontal);view.setSpeedSelected(true);check("speed manual bounds preserved separately",plot->explicitYRange()==std::optional<QPointF>(QPointF(1000,2000)));
    view.setSpeedSelected(false);PlotSeries dense{"CH1",QColor("#3769c7"),{}};
    for(int i=0;i<=100000;++i)dense.points.push_back({i/10000.,2.+std::sin(i*.03)});
    dense.points[50000].setY(nan);dense.points[73123].setY(-12);view.setSourceSeries({dense},"V");plot->clearYRange();plot->setXRange(0,10);paint(view);
    bool peak=false,gap=false;for(const auto& point:plot->displayPoints(0)){peak|=point==dense.points[73123];gap|=std::isnan(point.y());}
    check("dense M4 spike and NaN survive zero-centered autoscale",symmetric(plot->displayedYRange())&&plot->displayedYRange().y()>=12&&peak&&gap&&same(plot->series()[0].points,dense.points));save(view,"dense-spike-centered");
    SingleWaveformView spectrum(nullptr,false,QStringLiteral("频率 / Hz"));spectrum.resize(1020,600);spectrum.setSourceSeries({{"CH1",QColor("#3769c7"),{{0,1},{10,3}}}},"V");spectrum.show();paint(spectrum);
    check("spectrum stays zero-based nonnegative with no centering policy",!spectrum.plot()->zeroCenteredY()&&near(spectrum.plot()->displayedYRange().x(),0)&&spectrum.plot()->displayedYRange().y()>3);save(spectrum,"spectrum-nonnegative");
    QJsonObject report{{"passed",passed},{"checks",checks},{"runtime_cpu_architecture",QSysInfo::currentCpuArchitecture()},
        {"scope","Host Qt presentation and injected gesture regression; axis viewport only, no sample demeaning, sensor or physical touchscreen claim."}};
    const auto bytes=QJsonDocument(report).toJson();if(!evidence.isEmpty()){QFile out(evidence+"/zero-centered.json");if(!out.open(QIODevice::WriteOnly)||out.write(bytes)!=bytes.size())return 2;}
    std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return passed?0:1;
}
