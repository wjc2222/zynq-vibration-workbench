// Isolated trace-raster experiment. This does not change PlotCanvas policy.
#include "plot_canvas.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QBackingStore>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSysInfo>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace vibration::workbench;
namespace {
constexpr double pi = 3.14159265358979323846;
struct Run { QColor color; QVector<QPointF> points; };
struct Drawing { QImage image; qint64 nanoseconds = 0; };
double p95(std::vector<qint64> values) {
    std::sort(values.begin(), values.end()); return values.empty() ? 0 : values[(values.size()-1)*95/100]/1e6;
}
Drawing draw(const QImage& background, const QVector<Run>& runs, const QRectF& plot, const QRegion& exposed,
             QPainter::CompositionMode mode) {
    Drawing result{background.copy()};
    QPainter p(&result.image); p.setOpacity(1.0); p.setClipRegion(exposed);
    p.setClipRect(plot.adjusted(-1, -1, 1, 1), Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing, false); p.setCompositionMode(mode);
    QElapsedTimer elapsed; elapsed.start();
    for (const auto& run : runs) {
        QPen pen(run.color, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::BevelJoin); pen.setCosmetic(true); p.setPen(pen);
        if (run.points.size() > 1) p.drawPolyline(run.points.constData(), run.points.size());
        else if (run.points.size() == 1) p.drawPoint(run.points.first());
    }
    p.end(); result.nanoseconds = elapsed.nsecsElapsed(); return result;
}
QJsonObject compare(const QImage& background, const QVector<Run>& runs, const QRectF& plot,
                    const QRegion& exposed, const QString& clipName, int frames) {
    std::vector<qint64> over, source; QImage a, b;
    for (int i=0;i<frames+3;++i) {
        // Alternate ordering to reduce systematic warm-cache/order effects.
        Drawing overDraw, sourceDraw;
        if (i%2) {
            sourceDraw=draw(background,runs,plot,exposed,QPainter::CompositionMode_Source);
            overDraw=draw(background,runs,plot,exposed,QPainter::CompositionMode_SourceOver);
        } else {
            overDraw=draw(background,runs,plot,exposed,QPainter::CompositionMode_SourceOver);
            sourceDraw=draw(background,runs,plot,exposed,QPainter::CompositionMode_Source);
        }
        if(i>=3){over.push_back(overDraw.nanoseconds);source.push_back(sourceDraw.nanoseconds);}
        a=std::move(overDraw.image); b=std::move(sourceDraw.image);
    }
    int changed=0, maximum=0;
    for(int y=0;y<a.height();++y){
        const auto* rowA=reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* rowB=reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for(int x=0;x<a.width();++x) if(rowA[x]!=rowB[x]){
            ++changed; maximum=std::max({maximum,std::abs(qRed(rowA[x])-qRed(rowB[x])),
                std::abs(qGreen(rowA[x])-qGreen(rowB[x])),std::abs(qBlue(rowA[x])-qBlue(rowB[x]))});
        }
    }
    return {{"clip",clipName},{"clip_rectangles",exposed.rectCount()},{"frames",frames},
        {"pixels_exact",a==b},{"changed_pixels",changed},{"maximum_channel_difference",maximum},
        {"source_over_p95_ms",p95(over)},{"source_p95_ms",p95(source)},
        {"source_over_to_source_ratio",p95(source)>0?p95(over)/p95(source):0}};
}
QJsonObject scenario(double seconds, bool noise, bool missing, int frames) {
    QVector<PlotSeries> series;
    const QColor colors[]{QColor("#3769c7"),QColor("#268878"),QColor("#b78032")};
    for(int channel=0;channel<3;++channel){
        PlotSeries s{QStringLiteral("CH%1").arg(channel+1),colors[channel],{}};
        const int samples=int(seconds*50000); s.points.reserve(samples+2);
        for(int index=0;index<=samples;++index){
            if(missing&&index==samples/2)s.points.push_back({index/50000.,std::numeric_limits<double>::quiet_NaN()});
            quint32 mixed=quint32(index)+quint32(channel+1)*0x9e3779b9u;
            mixed^=mixed>>16;mixed*=0x7feb352du;mixed^=mixed>>15;mixed*=0x846ca68bu;mixed^=mixed>>16;
            const double value=noise?double(mixed>>8)/8388608.-1:std::sin(2*pi*1000*index/50000.+channel*.2);
            s.points.push_back({index/50000.,value});
        }
        series.push_back(std::move(s));
    }
    PlotCanvas canvas; canvas.setEmbedded(true); canvas.setLegendVisible(false); canvas.resize(1020,410);
    canvas.setChart({},QStringLiteral("s"),QStringLiteral("V"),series);canvas.setXRange(0,seconds);
    canvas.show();QApplication::processEvents();canvas.repaint();QApplication::processEvents();
    const QRectF plot=canvas.plotArea();const auto yRange=canvas.displayedYRange();
    const double xScale=plot.width()/seconds,yScale=plot.height()/(yRange.y()-yRange.x());
    QVector<Run> runs;quint64 vertices=0;int breaks=0;
    for(int channel=0;channel<3;++channel){
        Run run{colors[channel],{}};
        const auto flush=[&]{if(!run.points.isEmpty()){runs.push_back(std::move(run));run={colors[channel],{}};}};
        for(const auto& point:canvas.displayPoints(channel)){
            if(!std::isfinite(point.x())||!std::isfinite(point.y())){++breaks;flush();continue;}
            ++vertices;
            run.points.push_back({plot.left()+point.x()*xScale,
                plot.bottom()-(point.y()-yRange.x())*yScale});
        }
        flush();
    }
    QImage background(canvas.size(),QImage::Format_RGB32);
    for(int y=0;y<background.height();++y){auto* row=reinterpret_cast<QRgb*>(background.scanLine(y));
        for(int x=0;x<background.width();++x)row[x]=((x/19+y/17)%2)?qRgb(242,247,249):qRgb(255,255,255);}
    const QRegion full(background.rect());
    const int left=qRound(plot.left()+plot.width()*.37),right=qRound(plot.left()+plot.width()*.63);
    const QRegion strips=QRegion(QRect(left-2,0,5,background.height()))+QRegion(QRect(right-2,0,5,background.height()));
    const QRegion scroll(QRect(0,83,background.width(),211));
    QJsonArray rows;bool exact=true;
    for(const auto& clip:std::vector<std::pair<QString,QRegion>>{{"full_plot",full},{"cursor_strips",strips},{"scroll_exposure",scroll}}){
        auto row=compare(background,runs,plot,clip.second,clip.first,frames);exact&=row["pixels_exact"].toBool();rows.append(row);
    }
    return {{"seconds",seconds},{"noise",noise},{"missing_interval_marker",missing},{"mapped_original_vertices",double(vertices)},
        {"finite_runs",runs.size()},{"nan_break_markers",breaks},{"pixels_exact_all_clips",exact},{"comparisons",rows}};
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);qputenv("AX7_PLOT_TRACE_AA","0");
    int frames=21;
    if(qEnvironmentVariableIsSet("AX7_RASTER_AUDIT_FRAMES")){
        bool ok=false;frames=qEnvironmentVariableIntValue("AX7_RASTER_AUDIT_FRAMES",&ok);
        if(!ok||frames<1||frames>101){std::fputs("AX7_RASTER_AUDIT_FRAMES must be in [1,101]\n",stderr);return 2;}
    }
    QJsonArray rows;bool exact=true;
    for(double seconds:{.25,2.})for(bool noise:{false,true}){
        auto row=scenario(seconds,noise,false,frames);exact&=row["pixels_exact_all_clips"].toBool();rows.append(row);
    }
    auto missing=scenario(.25,true,true,frames);exact&=missing["pixels_exact_all_clips"].toBool();rows.append(missing);
    QJsonObject report{{"passed",exact},{"scenarios",rows},{"runtime_cpu_architecture",QSysInfo::currentCpuArchitecture()},
        {"qt_runtime_version",qVersion()},{"image_format","RGB32"},{"opacity",1.0},{"antialias",false},{"pen_physical_width",1.0},
        {"scope","Isolated QImage trace raster using production PlotCanvas M4 vertices and mapped geometry; exact SourceOver/Source pixels compared with full, partial and scroll clips."},
        {"limitations","No production composition policy changed. Timing excludes model materialization, envelope/map generation, background copy, axes, QWidget backing-store presentation, scanout and sensor capture."}};
    const auto bytes=QJsonDocument(report).toJson();
    if(argc>1){QFile out(QString::fromUtf8(argv[1]));if(!out.open(QIODevice::WriteOnly)||out.write(bytes)!=bytes.size())return 2;}
    std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return exact?0:1;
}
