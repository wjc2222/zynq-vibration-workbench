#include "analysis_pane.hpp"
#include "single_waveform_view.hpp"
#include "live_render_budget.hpp"
#include "test_fonts.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <cmath>
#include <cstdio>
using namespace vibration::workbench;
namespace {
void put(QByteArray& bytes,int at,quint64 value,int n){for(int i=0;i<n;++i)bytes[at+i]=char(value>>(i*8));}
QJsonObject frame(int first,int sequence){
    QByteArray bytes(200*32,0);for(int i=0;i<200;++i){const int n=first+i,at=i*32;put(bytes,at,n,8);put(bytes,at+8,quint64(n)*10000,8);for(int ch=0;ch<3;++ch)put(bytes,at+16+2*ch,quint16(qint16(n%2000-1000+ch)),2);put(bytes,at+22,1,2);}
    return {{"schema","vibration.capture.v2"},{"type","event"},{"event","preview"},{"job_id","batch_fixture"},{"generation","1"},{"frame_seq",sequence},{"first_record",first},{"record_count",200},
        {"committed_records",first+200},{"requested_records",100000},{"sample_rate_hz",5000},{"clock_hz",50000000},{"ppr",1},{"adc_range_uv",5000000},{"channel_map",328449},{"record_bytes",32},
        {"source","test_fixture"},{"hardware_confirmed",false},{"synthetic",true},{"source_flags",1},{"validity_flags",27},{"raw_base64",QString::fromLatin1(bytes.toBase64())},
        {"crc32",double(vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),std::size_t(bytes.size())))},{"dropped_preview_records",0}};
}
void pump(int ms){QElapsedTimer t;t.start();while(t.elapsed()<ms){QApplication::processEvents();QThread::msleep(1);}QApplication::processEvents();}
}
int main(int argc,char** argv){
    qputenv("AX7_LIVE_DEMAND_ANALYSIS","1");QApplication app(argc,argv);qa::configureFonts(app);bool passed=true;QJsonObject checks;
    const auto check=[&](const char* name,bool value){checks[name]=value;passed&=value;};
    LiveRenderBudget budget;check("fast default 40ms",budget.intervalMs()==40);budget.observe(5000000,160000000);check("165ms frame reserves equal main-loop headroom",budget.intervalMs()==330);
    for(int i=0;i<7;++i)budget.observe(0,1000000);check("cheap cursor samples cannot immediately erase costly frame",budget.intervalMs()==330);budget.observe(0,1000000);check("budget recovers after bounded history",budget.intervalMs()==40);budget.observe(5000000000LL,0);check("pacing has finite upper bound",budget.intervalMs()==2000);
    AnalysisPane pane;pane.resize(1020,600);pane.show();pane.setView(0);QString error;bool accepted=true;
    pane.beginLiveBatch();for(int i=0;i<8;++i)accepted&=i?pane.acceptLiveFrame(frame(i*200,i+1),&error):pane.beginLive(frame(0,1),false,&error);
    check("first batch validates every chunk without presenting intermediates",accepted&&pane.property("liveRenderedFrames").toULongLong()==0);pane.endLiveBatch();
    check("first bounded batch adopts one final window",pane.property("liveRenderedFrames").toULongLong()==1&&pane.property("liveAcceptedFrames").toULongLong()==8);
    auto* view=dynamic_cast<SingleWaveformView*>(pane.findChild<QWidget*>("singleWaveformView"));
    check("batch window contains all contiguous samples not one latest chunk",view&&view->sourceSeries().size()==3&&view->sourceSeries()[0].points.size()>=1250);
    const auto before=pane.property("liveRenderedFrames").toULongLong();pane.beginLiveBatch();pane.beginLiveBatch();for(int i=8;i<16;++i)accepted&=pane.acceptLiveFrame(frame(i*200,i+1),&error);
    pane.endLiveBatch();check("nested batch does not publish until outer end",pane.property("liveRenderedFrames").toULongLong()==before);pane.endLiveBatch();pump(100);
    check("later batch ingests all chunks and publishes once",accepted&&pane.property("liveAcceptedFrames").toULongLong()==16&&pane.property("liveRenderedFrames").toULongLong()==before+1);
    int gaps=0;if(view)for(const auto& point:view->sourceSeries()[0].points)gaps+=!std::isfinite(point.y());check("contiguous raw chunks do not create NaN gaps",gaps==0);
    pane.beginLiveBatch();accepted&=pane.acceptLiveFrame(frame(3600,17),&error);pane.endLiveBatch();pump(100);gaps=0;if(view)for(const auto& point:view->sourceSeries()[0].points)gaps+=!std::isfinite(point.y());
    check("true missing chunks remain explicit NaN and dropped count",accepted&&gaps==1&&pane.performanceProfile().value("preview_dropped_records").toDouble()==400);
    pane.endLive();const QJsonObject report{{"passed",passed},{"checks",checks},{"error",error},{"scope","Synthetic GUI batch and pure render-budget regression; no physical latency or board claim."}};
    std::puts(QJsonDocument(report).toJson().constData());return passed?0:1;
}
