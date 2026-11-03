#include "analysis_pane.hpp"
#include "live_preview_model.hpp"
#include "plot_canvas.hpp"
#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include "vibration/core.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace vibration;
using namespace vibration::workbench;
namespace {
constexpr double pi=3.14159265358979323846, scale=5./32768.;
QJsonArray checks;
void check(bool ok,const char* text){checks.append(QJsonObject{{"check",text},{"passed",ok}});if(!ok)throw std::runtime_error(text);}
bool near(double a,double b){return std::abs(a-b)<1e-10;}
void paint(QWidget& widget){QApplication::processEvents();widget.grab();}
void put(QByteArray& bytes,int at,quint64 value,int n){for(int i=0;i<n;++i)bytes[at+i]=char(value>>(i*8));}
QJsonObject frame(int first,int count,int sequence){
    QByteArray bytes(count*32,0);
    for(int i=0;i<count;++i){const int index=first+i,at=i*32;put(bytes,at,index,8);put(bytes,at+8,quint64(index)*1000,8);
        for(int ch=0;ch<3;++ch)put(bytes,at+16+ch*2,quint16(std::int16_t(std::lround(2*std::sin(2*pi*1000*index/50000.+ch*.2)/scale))),2);
        put(bytes,at+22,1,2);}
    return {{"schema","vibration.capture.v2"},{"type","event"},{"event","preview"},{"job_id","software_raw_fidelity"},{"generation","1"},
        {"frame_seq",sequence},{"first_record",first},{"record_count",count},{"committed_records",first+count},{"requested_records",500000},
        {"sample_rate_hz",50000},{"clock_hz",50000000},{"ppr",1},{"adc_range_uv",5000000},{"channel_map",328449},{"record_bytes",32},
        {"source","test_fixture"},{"hardware_confirmed",false},{"synthetic",true},{"source_flags",1},{"validity_flags",27},
        {"raw_base64",QString::fromLatin1(bytes.toBase64())},{"crc32",double(crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),bytes.size()))},{"dropped_preview_records",0}};
}
bool subset(const QVector<QPointF>& display,const QVector<QPointF>& raw){
    int position=0;for(const auto& point:display){if(!std::isfinite(point.y()))continue;
        while(position<raw.size()&&raw[position].x()<point.x())++position;
        if(position>=raw.size()||point!=raw[position])return false;}
    return true;
}
void mouse(PlotCanvas& plot,QEvent::Type type,const QPointF& at,Qt::MouseButtons buttons){QMouseEvent event(type,at,Qt::LeftButton,buttons,Qt::ShiftModifier);QApplication::sendEvent(&plot,&event);}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);QJsonObject report;QString output=argc>1?QString::fromUtf8(argv[1]):QString();
    if(!output.isEmpty()&&!QDir().mkpath(output))return 2;
    try{
        report["phase"]="construct_capture_fixture";
        auto capture=std::make_shared<Capture>();capture->integrity_verified=true;capture->source_metadata.synthetic=true;capture->source_metadata.status="synthetic_software_fixture";
        capture->header.sample_rate_hz=50000;capture->header.clock_hz=50000000;capture->header.adc_range_uv=5000000;capture->header.pulses_per_rev=1;
        capture->header.completed_records=30000;capture->records.resize(30000);
        for(int i=0;i<30000;++i){auto& r=capture->records[i];r.index=i;r.tick=quint64(i)*1000;r.flags=1;
            for(int ch=0;ch<3;++ch)r.raw[ch]=std::int16_t(std::lround(2*std::sin(2*pi*1000*i/50000.+ch*.2)/scale));}
        const auto originals=capture->records;AnalysisConfig config;config.analog_gain=1;config.sensor_sensitivity_v_per_g=1;config.fft_size=8192;
        report["phase"]="analyze_capture";const auto result=analyze(*capture,config);
        report["phase"]="construct_analysis_pane";AnalysisPane pane;pane.resize(1280,720);pane.show();
        report["phase"]="populate_analysis_pane";pane.setResult(capture,&result);
        report["phase"]="paint_analysis_pane";paint(pane);
        auto* view=pane.findChild<SingleWaveformView*>("singleWaveformView");check(view,"actual analysis single canvas exists");auto* plot=view->plot();
        check(view->sourceSeries()[0].points.size()==12501,"offline quarter-second retains all12501 original samples before pixel reduction");
        bool exact=true;for(const auto& point:view->sourceSeries()[0].points){const int index=int(std::llround(point.x()*50000));exact&=near(point.x(),index/50000.)&&near(point.y(),originals[index].raw[0]*scale);}
        check(exact,"offline coordinates and amplitudes equal original sample indices and ADC scaling");
        check(subset(plot->displayPoints(0),view->sourceSeries()[0].points),"pixel envelope contains only original sample coordinates");
        if(!output.isEmpty())pane.grab().save(output+"/01-software-1khz-50k-overview.png");
        pane.setWindow(.1,.12);paint(pane);auto area=plot->plotArea();const QPointF a(area.left()+area.width()*.25,area.center().y()),b(area.left()+area.width()*.75,area.center().y());
        mouse(*plot,QEvent::MouseButtonPress,a,Qt::LeftButton);mouse(*plot,QEvent::MouseMove,b,Qt::LeftButton);mouse(*plot,QEvent::MouseButtonRelease,b,Qt::NoButton);paint(pane);
        const auto range=plot->explicitXRange();check(range&&near(range->x(),.105)&&near(range->y(),.115),"real mouse box zoom maps to exact10ms window");
        const auto zoomed=view->sourceSeries()[0].points;report["mouse_zoom_raw_count"]=zoomed.size();check(zoomed.size()==501,"mouse zoom recovers501 raw samples rather than enlarging an old envelope");
        exact=true;for(int i=0;i<zoomed.size();++i)exact&=near(zoomed[i].x(),(5250+i)/50000.)&&near(zoomed[i].y(),originals[5250+i].raw[0]*scale);
        check(exact,"mouse-zoom samples preserve20microsecond spacing and quantized sine values");
        if(!output.isEmpty())pane.grab().save(output+"/02-software-mouse-10ms-raw.png");
        const auto& spectrum=result.spectra[0];const auto peak=std::max_element(spectrum.peak_amplitude.begin()+1,spectrum.peak_amplitude.end());const auto bin=std::size_t(peak-spectrum.peak_amplitude.begin());
        check(std::abs(spectrum.frequency_hz[bin]-1000)<=spectrum.bin_width_hz,"raw-data FFT peak remains within one bin of1kHz");
        PlotCanvas frequency;frequency.resize(1000,420);QVector<QPointF> frequencyPoints;for(std::size_t i=0;i<spectrum.frequency_hz.size();++i)frequencyPoints.push_back({spectrum.frequency_hz[i],spectrum.peak_amplitude[i]});
        frequency.setChart({},QStringLiteral("频率 / Hz"),QStringLiteral("幅值 / V"),{{"CH1",QColor("#3769c7"),frequencyPoints}});frequency.setXRange(0,2000);frequency.show();paint(frequency);
        check(frequency.xTickLabels()==QStringList({"0","500","1000","1500","2000"}),"frequency ticks are readable decimal500Hz steps");
        if(!output.isEmpty())frequency.grab().save(output+"/03-software-fft-nice-ticks.png");
        frequency.setXRange(993,1007);paint(frequency);check(frequency.xTickLabels()==QStringList({"995","1000","1005"}),"zoomed frequency uses nice5Hz ticks without scientific notation");
        if(!output.isEmpty())frequency.grab().save(output+"/06-software-fft-5hz-tick-window.png");
        LivePreviewModel live;QString error;check(live.begin(frame(467500,1000,1),false,&error),"explicit synthetic global-time live frame accepted");
        for(int i=1;i<13;++i)check(live.accept(frame(467500+i*1000,i==12?500:1000,i+1),&error),"contiguous live software frame accepted");
        auto liveView=live.view(.25);const auto livePoints=liveView.voltage[0].points;int finiteCount=0;exact=true;
        for(const auto& point:livePoints){if(!std::isfinite(point.y()))continue;++finiteCount;const int index=int(std::llround(point.x()*50000));exact&=near(point.x(),index/50000.)&&near(point.y(),std::lround(2*std::sin(2*pi*1000*index/50000.)/scale)*scale);}
        check(finiteCount==12500&&exact,"live quarter-second retains12500 raw samples with global9.35s origin");
        check(near(liveView.endTime,479999/50000.),"live time coordinate uses global index not UART arrival time");
        const auto beforeGap=live.info().acceptedRecords;check(live.accept(frame(481000,1000,15),&error),"later live frame after explicit missing interval accepted");
        const auto gapView=live.view(.25);bool gap=false;for(const auto& point:gapView.voltage[0].points)gap|=!std::isfinite(point.y());
        check(gap&&live.info().acceptedRecords==beforeGap+1000,"missing live interval remains a NaN path break without interpolation");
        PlotCanvas pulse;pulse.resize(1000,420);QVector<QPointF> raw;for(int i=0;i<12500;++i)raw.push_back({9.35+i/50000.,0});raw[1111].setY(4.5);raw[1112].setY(-4.2);
        for(int i=5000;i<=6000;++i)raw[i].setY(std::numeric_limits<double>::quiet_NaN());
        pulse.setChart({},"s","V",{{"pulse",QColor("#3769c7"),raw}});pulse.setXRange(9.35,9.59998);pulse.show();paint(pulse);const auto reduced=pulse.displayPoints(0);
        check(reduced.contains(raw[1111])&&reduced.contains(raw[1112]),"one-sample positive and negative pulses survive display reduction");
        check(subset(reduced,raw),"pulse rendering never invents filtered or interpolated samples");
        const auto image=pulse.grab().toImage();const auto pulseArea=pulse.plotArea();const int gapX=int(pulseArea.left()+((9.46-9.35)/(.24998))*pulseArea.width());bool noBlue=true;
        for(int y=int(pulseArea.top()+2);y<int(pulseArea.bottom()-2);++y){const auto c=image.pixelColor(gapX,y);noBlue&=!(c.blue()>c.red()+30&&c.blue()>c.green()+10);}
        check(noBlue,"actual raster does not draw a false line through missing samples");
        if(!output.isEmpty())pulse.grab().save(output+"/04-software-single-sample-pulses-and-gap.png");
        view->setChannelChecked(0,false);view->setChannelChecked(1,false);view->setChannelChecked(2,false);paint(pane);
        check(view->selectedChannels().isEmpty()&&plot->series().isEmpty(),"unchecked channels leave no residual blue trace");
        if(!output.isEmpty())pane.grab().save(output+"/05-software-all-channels-unchecked.png");
        bool unchanged=capture->records.size()==originals.size();for(std::size_t i=0;i<originals.size();++i)unchanged&=capture->records[i].raw==originals[i].raw&&capture->records[i].tick==originals[i].tick&&capture->records[i].index==originals[i].index;
        check(unchanged,"all zoom rendering and channel changes preserve original raw data");report["result"]="passed";report["fft_peak_hz"]=spectrum.frequency_hz[bin];
    }catch(const std::exception& e){report["result"]="failed";report["error"]=e.what();}
    report["checks"]=checks;report["scope"]="Synthetic HDMI software QA only; no physical input, display optics, UART, calibration or industrial diagnostic accuracy claim";
    std::puts(QJsonDocument(report).toJson(QJsonDocument::Indented).constData());return report["result"]=="passed"?0:1;
}
