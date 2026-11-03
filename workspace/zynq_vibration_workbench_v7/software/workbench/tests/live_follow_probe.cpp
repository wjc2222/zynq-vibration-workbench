#include "analysis_pane.hpp"
#include "single_waveform_view.hpp"
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QThread>
#include <QElapsedTimer>
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace vibration::workbench;
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void put32(unsigned char* p,quint32 n){for(int i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(i*8));}
void put64(unsigned char* p,quint64 n){put32(p,quint32(n));put32(p+4,quint32(n>>32));}
QJsonObject frame(quint64 first,quint64 sequence,int rawValue){
    QByteArray raw(2000*32,'\0');for(int i=0;i<2000;++i){auto* p=reinterpret_cast<unsigned char*>(raw.data())+i*32;put64(p,first+quint64(i));put64(p+8,1000000+(first+quint64(i))*1000);
        for(int ch=0;ch<3;++ch){const auto value=quint16(rawValue+ch*1000);p[16+ch*2]=static_cast<unsigned char>(value);p[17+ch*2]=static_cast<unsigned char>(value>>8);}p[22]=1;}
    const QString session(32,'1');return {{"schema","vibration.stream.v1"},{"type","event"},{"event","preview"},{"emitter","stream_service"},{"id","follow-test"},{"request_id","follow-test"},{"job_id",session},{"session_id",session},{"source","test_fixture"},{"synthetic",true},{"hardware_confirmed",false},{"frame_seq",QString::number(sequence)},{"global_first_index",QString::number(first)},{"first_tick",QString::number(1000000+first*1000)},{"last_tick",QString::number(1000000+(first+1999)*1000)},{"record_count",2000},{"sample_rate_hz",50000},{"clock_hz",50000000},{"ppr",1},{"adc_range_uv",5000000},{"channel_map",0x50301},{"record_bytes",32},{"total_records","0"},{"crc32",double(vibration::crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()),std::size_t(raw.size())))},{"raw_base64",QString::fromLatin1(raw.toBase64())}};
}
bool waitFor(const std::function<bool()>& predicate){QElapsedTimer t;t.start();while(!predicate()&&t.elapsed()<3000){QApplication::processEvents();QThread::msleep(2);}return predicate();}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qputenv("AX7_LIVE_DEMAND_ANALYSIS","1");QJsonArray checks;bool passed=false;QString error;
    try{
        AnalysisPane pane;pane.resize(1024,600);pane.show();pane.setView(0);auto* wave=dynamic_cast<SingleWaveformView*>(pane.findChild<QWidget*>("singleWaveformView"));auto* note=pane.findChild<QLabel*>("plotNavigationNote");require(wave&&note,"Actual waveform and navigation widgets required");
        QString priorRms;
        for(int phase=0;phase<3;++phase){const quint64 first=quint64(phase)*100000;const int value=phase?phase*10000:1000;const auto next=frame(first,quint64(phase+1),value);
            require(phase?pane.acceptLiveFrame(next):pane.beginLive(next,true),"Actual live model rejected changed raw window");
            const auto expectedEnd=double(first+1999)/50000.;require(waitFor([&]{return wave->plot()->explicitXRange()&&std::abs(wave->plot()->explicitXRange()->y()-expectedEnd)<1e-9;}),"Wave axis did not adopt latest raw time window");
            const auto range=*wave->plot()->explicitXRange();const auto expected=QStringLiteral("实时跟随 · %1–%2 s").arg(QString::number(range.x(),'g',5),QString::number(range.y(),'g',5));
            require(note->text()==expected,"Live-follow label must match the newly displayed plot range");
            for(int ch=0;ch<3;++ch){auto* rms=pane.findChild<QLabel*>(QString("singleRms%1").arg(ch+1));auto* peak=pane.findChild<QLabel*>(QString("singlePeak%1").arg(ch+1));const double wanted=double(value+ch*1000)*5/32768.;
                require(rms&&peak&&rms->text()==QString::number(wanted,'g',3)&&peak->text()=="0","RMS/peak must be recomputed from the current changed raw values");
                const auto scope=QStringLiteral("原始预览可见窗 %1–%2 s").arg(QString::number(range.x(),'g',7),QString::number(range.y(),'g',7));require(rms->toolTip().contains(scope),"RMS scope must move with the displayed time window");
                if(ch==0){if(phase)require(rms->text()!=priorRms,"Changed window amplitude must not retain the first RMS");priorRms=rms->text();}}
            checks.append(QJsonObject{{"phase",phase},{"range_start",range.x()},{"range_end",range.y()},{"label",note->text()},{"rms_ch1",priorRms},{"passed",true}});
        }
        pane.endLive();passed=true;
    }catch(const std::exception& e){error=QString::fromUtf8(e.what());}
    const auto report=QJsonDocument(QJsonObject{{"schema","vibration.stream.live-follow.host-qa.v1"},{"synthetic",true},{"passed",passed},{"error",error},{"checks",checks}}).toJson(QJsonDocument::Compact);std::fwrite(report.constData(),1,std::size_t(report.size()),stdout);std::fputc('\n',stdout);return passed?0:1;
}
