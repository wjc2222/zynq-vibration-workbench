#include "analysis_pane.hpp"
#include "single_waveform_view.hpp"
#include "controls.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTouchDevice>
#include <QTouchEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace vibration;
using namespace vibration::workbench;
namespace {
bool near(double a,double b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<1e-9*std::max({1.,std::abs(a),std::abs(b)});}
bool fixed(const PlotCanvas& plot,double low,double high){const auto r=plot.explicitYRange();return plot.fixedYRange()&&r&&near(r->x(),low)&&near(r->y(),high);}
bool same(const QVector<PlotSeries>& a,const QVector<PlotSeries>& b){
    if(a.size()!=b.size())return false;
    for(int s=0;s<a.size();++s){if(a[s].points.size()!=b[s].points.size())return false;for(int i=0;i<a[s].points.size();++i){const double ax=a[s].points[i].x(),ay=a[s].points[i].y(),bx=b[s].points[i].x(),by=b[s].points[i].y();if(std::memcmp(&ax,&bx,sizeof(double))||std::memcmp(&ay,&by,sizeof(double)))return false;}}
    return true;
}
void paint(QWidget& w){QApplication::processEvents();w.grab();QApplication::processEvents();}
void pump(int ms){QElapsedTimer timer;timer.start();while(timer.elapsed()<ms){QApplication::processEvents();QThread::msleep(2);}QApplication::processEvents();}
// Use the same touch targets as a finger, including field focus, clear, sign,
// decimal, backspace and Apply. No setText()/setFixedYRange() shortcut here.
bool enterRange(SingleWaveformView& view,double extent,bool spectrum){
    bool applied=false;
    QTimer::singleShot(100,&view,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog||dialog->objectName()!="plotAmplitudeDialog")return;
        try{
            auto* low=dialog->findChild<QLineEdit*>("plotAmplitudeLower");auto* high=dialog->findChild<QLineEdit*>("plotAmplitudeUpper");auto* box=dialog->findChild<QDialogButtonBox*>();
            if(!low||!high||!box)throw std::runtime_error("missing range touch fields");
            const auto key=[&](int i){qa::interaction::tap(dialog->findChild<QPushButton*>(QStringLiteral("plotAmplitudeKey%1").arg(i)));};
            if(!spectrum){qa::interaction::tap(low);key(7);key(11);key(9);} // -2
            qa::interaction::tap(high);key(7);key(9);key(13);key(10);key(3); // 2.3, backspace -> 2.
            if(high->text()!=QStringLiteral("2.")||(!spectrum&&low->text()!=QStringLiteral("-2")))throw std::runtime_error(QStringLiteral("keypad focus/edit mismatch: lower=%1 upper=%2").arg(low->text(),high->text()).toStdString());
            key(7);const QString digits=QString::number(extent,'f',0);for(const auto digit:digits){const int indices[]={12,8,9,10,4,5,6,0,1,2};key(indices[digit.digitValue()]);}
            if(spectrum&&(!low->isReadOnly()||low->text()!=QStringLiteral("0")))throw std::runtime_error("spectrum zero reference editable");
            qa::interaction::tap(box->button(QDialogButtonBox::Ok));applied=true;
        }catch(const std::exception& error){std::fprintf(stderr,"range keypad probe: %s\n",error.what());dialog->reject();}
    });
    QTimer guard;guard.setSingleShot(true);QObject::connect(&guard,&QTimer::timeout,[&]{if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});guard.start(3000);
    qa::interaction::tap(view.findChild<QPushButton*>("plotSetRange"));guard.stop();return applied;
}
bool chooseAutomatic(SingleWaveformView& view){
    bool selected=false;QTimer::singleShot(100,&view,[&]{if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))if(auto* automatic=dialog->findChild<QPushButton*>("plotAutoRange")){qa::interaction::tap(automatic);selected=true;}});
    qa::interaction::tap(view.findChild<QPushButton*>("plotSetRange"));return selected;
}
void mouse(PlotCanvas& p,QEvent::Type type,QPointF pos,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent e(type,pos,button,buttons,Qt::NoModifier);QApplication::sendEvent(&p,&e);}
void drag(PlotCanvas& p,QPointF from,QPointF to){mouse(p,QEvent::MouseButtonPress,from,Qt::LeftButton,Qt::LeftButton);mouse(p,QEvent::MouseMove,to,Qt::NoButton,Qt::LeftButton);mouse(p,QEvent::MouseButtonRelease,to,Qt::LeftButton,Qt::NoButton);}
void pinch(PlotCanvas& plot,bool vertical){
    QTouchDevice device;device.setType(QTouchDevice::TouchScreen);device.setCapabilities(QTouchDevice::Position);device.setMaximumTouchPoints(2);const auto area=plot.plotArea();
    const auto position=[&](double amount){return vertical?QPointF(area.center().x(),area.top()+area.height()*amount):QPointF(area.left()+area.width()*amount,area.center().y());};
    const auto send=[&](QEvent::Type type,double a,double b,Qt::TouchPointState state){QList<QTouchEvent::TouchPoint> points;for(int i=0;i<2;++i){QTouchEvent::TouchPoint finger(i+20);auto p=position(i?b:a);finger.setState(state);finger.setPos(p);finger.setStartPos(position(i?.65:.35));finger.setLastPos(p);finger.setScreenPos(plot.mapToGlobal(p.toPoint()));points.push_back(finger);}QTouchEvent e(type,&device,Qt::NoModifier,state,points);QApplication::sendEvent(&plot,&e);};
    send(QEvent::TouchBegin,.35,.65,Qt::TouchPointPressed);send(QEvent::TouchUpdate,.25,.75,Qt::TouchPointMoved);send(QEvent::TouchEnd,.25,.75,Qt::TouchPointReleased);
}
QVector<PlotSeries> samples(double scale=1){QVector<PlotSeries> out;for(int ch=0;ch<3;++ch){PlotSeries line{QStringLiteral("CH%1").arg(ch+1),QColor("#3769c7"),{}};for(int i=0;i<=1000;++i)line.points.push_back({i/100.,scale*(ch+1)*std::sin(i*.05)});line.points[500].setY(std::numeric_limits<double>::quiet_NaN());out.push_back(line);}return out;}
void put(QByteArray& bytes,int at,quint64 value,int n){for(int i=0;i<n;++i)bytes[at+i]=char(value>>(i*8));}
QJsonObject frame(int first,int sequence){
    QByteArray bytes(200*32,0);for(int i=0;i<200;++i){const int n=first+i,at=i*32;put(bytes,at,n,8);put(bytes,at+8,quint64(n)*10000,8);for(int ch=0;ch<3;++ch)put(bytes,at+16+2*ch,quint16(qint16((n%2000-1000)*(ch+1))),2);put(bytes,at+22,3,2);put(bytes,at+24,5000000,4);}
    return {{"schema","vibration.capture.v2"},{"type","event"},{"event","preview"},{"job_id","fixed_range_fixture"},{"generation","1"},{"frame_seq",sequence},{"first_record",first},{"record_count",200},
        {"committed_records",first+200},{"requested_records",100000},{"sample_rate_hz",5000},{"clock_hz",50000000},{"ppr",1},{"adc_range_uv",5000000},{"channel_map",328449},{"record_bytes",32},
        {"source","test_fixture"},{"hardware_confirmed",false},{"synthetic",true},{"source_flags",1},{"validity_flags",27},{"raw_base64",QString::fromLatin1(bytes.toBase64())},
        {"crc32",double(vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),std::size_t(bytes.size())))},{"dropped_preview_records",0}};
}
}
int main(int argc,char** argv){
    QApplication app(argc,argv);qa::configureFonts(app);QTemporaryDir settingsDir;if(!settingsDir.isValid())return 2;
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settingsDir.path());bool passed=true;QJsonObject checks;
    const auto check=[&](const char* name,bool value){checks[name]=value;passed&=value;};
    {
        AnalysisPane empty;empty.resize(1020,560);empty.show();paint(empty);
        auto* wave=dynamic_cast<SingleWaveformView*>(empty.findChild<QWidget*>("singleWaveformView"));auto* fft=dynamic_cast<SingleWaveformView*>(empty.findChild<QWidget*>("singleSpectrumView"));
        check("idle waveform range entry is enabled before data while data navigation is disabled",wave&&fft&&wave->findChild<QPushButton*>("plotSetRange")->isEnabled()&&!wave->plot()->isEnabled());
        if(wave&&fft){
            check("idle time range is set by actual numeric keypad",enterRange(*wave,4,false)&&fixed(*wave->plot(),-4,4));
            empty.setView(1);paint(empty);check("idle spectrum range is set by actual numeric keypad",enterRange(*fft,6,true)&&fixed(*fft->plot(),0,6));
            empty.setAccelerationMode(true);empty.setView(0);paint(empty);check("idle acceleration range is set by actual numeric keypad",enterRange(*wave,40,false)&&fixed(*wave->plot(),-40,40));
            empty.setAccelerationMode(false);empty.clear();paint(empty);check("idle mode return and clear preserve independently entered ranges",fixed(*wave->plot(),-4,4)&&fixed(*fft->plot(),0,6)&&wave->findChild<QPushButton*>("plotSetRange")->isEnabled());
            QString error;bool accepted=empty.beginLive(frame(0,1),true,&error);for(int i=1;i<8&&accepted;++i)accepted&=empty.acceptLiveFrame(frame(i*200,i+1),&error);pump(150);paint(empty);
            check("first acquisition and streaming keep ranges configured before data",accepted&&fixed(*wave->plot(),-4,4)&&fixed(*fft->plot(),0,6));
            empty.setAccelerationMode(true);paint(empty);check("first acquisition remembers preset acceleration range",fixed(*wave->plot(),-40,40));empty.endLive();empty.clear();empty.setAccelerationMode(false);paint(empty);
            check("cleared data keeps direct range editor and automatic action reachable",chooseAutomatic(*wave)&&!wave->plot()->fixedYRange()&&wave->findChild<QPushButton*>("plotSetRange")->isEnabled());
        }
    }
    {QSettings clean(QSettings::IniFormat,QSettings::UserScope,QStringLiteral("VibrationWorkbench"),QStringLiteral("WorkbenchV7"));clean.clear();clean.sync();}
    SingleWaveformView view;view.resize(1020,600);view.setSourceSeries(samples(),"V");view.setNavigationBounds(0,10);view.setXRange(2,8);view.show();paint(view);auto* plot=view.plot();
    check("fresh V7 profile starts automatic",!plot->explicitYRange()&&!plot->fixedYRange());
    check("manual voltage limits become symmetric fixed limits",view.setFixedYRange(-2,3)&&fixed(*plot,-3,3));
    const auto raw=samples(40);view.setSourceSeries(raw,"V");view.setChannelChecked(1,true);view.setChannelChecked(0,false);view.setChannelChecked(1,false);view.setChannelChecked(2,true);paint(view);
    check("growing frames and empty or changed channel selection retain fixed voltage axis",fixed(*plot,-3,3)&&near(plot->displayedYRange().x(),-3)&&near(plot->displayedYRange().y(),3)&&same(view.sourceSeries(),raw));
    const auto xBefore=plot->explicitXRange();auto area=plot->plotArea();QPointF gutter(area.left()*.5,area.center().y());
    QWheelEvent ywheel(gutter,gutter,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(plot,&ywheel);drag(*plot,gutter,{gutter.x(),gutter.y()+40});pinch(*plot,true);paint(view);
    check("fixed voltage ignores wheel axis drag vertical touch and direct Y zoom",fixed(*plot,-3,3)&&plot->explicitXRange()==xBefore&&!plot->zoomY(.5));
    QPointF center=plot->plotArea().center();QWheelEvent xwheel(center,center,QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(plot,&xwheel);
    check("fixed voltage retains mouse horizontal zoom",plot->explicitXRange()!=xBefore&&fixed(*plot,-3,3));
    plot->setXRange(2,8);paint(view);pinch(*plot,false);check("fixed voltage retains horizontal touch pinch",plot->explicitXRange()!=xBefore&&fixed(*plot,-3,3));
    plot->setXRange(2,8);paint(view);area=plot->plotArea();drag(*plot,{area.left()+area.width()*.4,area.center().y()},{area.left()+area.width()*.5,area.center().y()});check("fixed voltage retains horizontal drag",plot->explicitXRange()!=xBefore&&fixed(*plot,-3,3));
    plot->restoreView();check("restore full X keeps fixed Y until explicit auto",plot->explicitXRange()==std::optional<QPointF>(QPointF(0,10))&&fixed(*plot,-3,3));
    view.setSpeedSeries({"RPM",QColor("#3769c7"),{{0,1000},{10,2000}}},"RPM");view.setSpeedSelected(true);view.setFixedYRange(1000,2500);view.setSpeedSelected(false);
    check("speed selection uses independent range and restores fixed voltage",fixed(*plot,-3,3));
    view.setSourceSeries(samples(100),QStringLiteral("m/s²"));check("acceleration starts with its own auto profile",!plot->fixedYRange()&&!plot->explicitYRange());view.setFixedYRange(-100,100);view.setSourceSeries(raw,"V");check("return to voltage restores voltage range",fixed(*plot,-3,3));view.setSourceSeries(samples(100),QStringLiteral("m/s²"));check("return to acceleration restores acceleration range",fixed(*plot,-100,100));view.clearData();check("clear data keeps user acceleration range",fixed(*plot,-100,100));
    SingleWaveformView spectrum(nullptr,false,QStringLiteral("频率 / Hz"));spectrum.resize(1020,600);spectrum.setSourceSeries(samples(),"V");spectrum.show();paint(spectrum);
    check("spectrum profile starts independently automatic",!spectrum.plot()->fixedYRange());check("spectrum fixed range is zero to positive upper bound",spectrum.setFixedYRange(0,1.5)&&fixed(*spectrum.plot(),0,1.5));const auto spectrumRaw=samples(300);spectrum.setSourceSeries(spectrumRaw,"V");spectrum.setChannelChecked(1,true);spectrum.clearData();spectrum.setSourceSeries(spectrumRaw,"V");paint(spectrum);
    check("spectrum frames channel changes and clear keep fixed range and original samples",fixed(*spectrum.plot(),0,1.5)&&near(spectrum.plot()->displayedYRange().x(),0)&&near(spectrum.plot()->displayedYRange().y(),1.5)&&same(spectrum.sourceSeries(),spectrumRaw));
    check("invalid inputs do not modify fixed range",!view.setFixedYRange(0,0)&&!view.setFixedYRange(-1,std::numeric_limits<double>::infinity())&&!spectrum.setFixedYRange(0,-1)&&fixed(*spectrum.plot(),0,1.5));
    {SingleWaveformView restored;check("voltage fixed setting persists across widget recreation",fixed(*restored.plot(),-3,3));restored.setSourceSeries({},QStringLiteral("m/s²"));check("acceleration fixed setting persists independently",fixed(*restored.plot(),-100,100));SingleWaveformView restoredSpectrum(nullptr,false);check("spectrum fixed setting persists independently",fixed(*restoredSpectrum.plot(),0,1.5));}
    view.raise();view.activateWindow();paint(view);view.setSourceSeries(raw,"V");bool dialogOpened=false,invalidRejected=false;int phase=0;QTimer edit;edit.setInterval(20);QObject::connect(&edit,&QTimer::timeout,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog||dialog->objectName()!="plotAmplitudeDialog")return;dialogOpened=true;auto* low=dialog->findChild<QLineEdit*>("plotAmplitudeLower");auto* high=dialog->findChild<QLineEdit*>("plotAmplitudeUpper");auto* buttons=dialog->findChild<QDialogButtonBox*>();if(!low||!high||!buttons){edit.stop();dialog->reject();return;}if(!phase){low->setText("nan");high->setText("2");buttons->button(QDialogButtonBox::Ok)->click();invalidRejected=dialog->isVisible()&&fixed(*plot,-3,3);phase=1;}else{edit.stop();low->setText("-7");high->setText("7");buttons->button(QDialogButtonBox::Ok)->click();}});
    QTimer::singleShot(3000,[&]{if(edit.isActive()){edit.stop();if(auto* d=qobject_cast<QDialog*>(QApplication::activeModalWidget()))d->reject();}});edit.start();qa::interaction::tap(view.findChild<QPushButton*>("plotSetRange"));edit.stop();
    check("touch sized toolbar opens actual dialog rejects NaN and applies fixed bounds",dialogOpened&&invalidRejected&&fixed(*plot,-7,7)&&view.findChild<QPushButton*>("plotSetRange")->height()>=44);
    check("explicit auto button releases fixed mode",chooseAutomatic(view)&&!plot->fixedYRange()&&!plot->explicitYRange());{SingleWaveformView restored;check("explicit auto choice persists across recreation",!restored.plot()->fixedYRange()&&!restored.plot()->explicitYRange());restored.setSourceSeries({},QStringLiteral("m/s^2"));SingleWaveformView restoredSpectrum(nullptr,false);check("voltage auto leaves acceleration and spectrum fixed independently",fixed(*restored.plot(),-100,100)&&fixed(*restoredSpectrum.plot(),0,1.5));}
    AnalysisResult result;result.duration_s=1;result.header.sample_rate_hz=5000;result.config.amplitude_unit=AmplitudeUnit::Voltage;for(int i=0;i<=100;++i)result.waveform.push_back({i*.01,{{std::sin(i),2*std::sin(i),3*std::sin(i)}}});for(auto& s:result.spectra){s.frequency_hz={0,10,20};s.peak_amplitude={0,.2,.1};}result.order_bands[0][0]={1,10.,1.,.2,"valid"};result.speed.pulses_per_rev=1;result.speed.held_mean_rpm=600;
    AnalysisPane pane;pane.resize(1020,600);pane.setAccelerationMode(false);pane.setResult({},&result);pane.show();paint(pane);auto* wave=dynamic_cast<SingleWaveformView*>(pane.findChild<QWidget*>("singleWaveformView"));auto* fft=dynamic_cast<SingleWaveformView*>(pane.findChild<QWidget*>("singleSpectrumView"));
    check("integration exposes single waveform and spectrum",wave&&fft);QString error;
    if(wave&&fft){wave->setFixedYRange(-9,9);fft->setFixedYRange(0,2);const auto fftSamples=fft->sourceSeries();check("order overlay starts disabled",!pane.orderMarkersVisible()&&fft->plot()->referenceLines().isEmpty());pane.setOrderMarkersVisible(true);check("order overlay uses provided rotational reference without FFT changes",pane.orderMarkersVisible()&&fft->plot()->referenceLines()==QVector<QPointF>{{10,1}}&&same(fftSamples,fft->sourceSeries()));pane.setOrderMarkersVisible(false);check("order overlay can be hidden without FFT changes",fft->plot()->referenceLines().isEmpty()&&same(fftSamples,fft->sourceSeries()));pane.setResult({},&result);pane.setWindow(.1,.2);pane.setView(1);pane.setView(0);check("offline reload window and page switch preserve fixed V and spectrum",fixed(*wave->plot(),-9,9)&&fixed(*fft->plot(),0,2));pane.setAccelerationMode(true);wave->setFixedYRange(-90,90);pane.setAccelerationMode(false);check("AnalysisPane switching V and acceleration restores each range",fixed(*wave->plot(),-9,9));pane.setAccelerationMode(true);check("AnalysisPane acceleration range survives return",fixed(*wave->plot(),-90,90));pane.setAccelerationMode(false);pane.setView(0);
        const bool began=pane.beginLive(frame(0,1),true,&error);bool accepted=began;for(int i=1;i<6&&accepted;++i)accepted&=pane.acceptLiveFrame(frame(i*200,i+1),&error);pump(120);paint(pane);check("live start clear and successive original frames retain fixed V and spectrum",accepted&&fixed(*wave->plot(),-9,9)&&fixed(*fft->plot(),0,2));pane.setAccelerationMode(true);paint(pane);check("live acceleration conversion retains independent fixed range",fixed(*wave->plot(),-90,90));pane.setAccelerationMode(false);pane.setView(1);pump(300);check("live view switch retains fixed spectrum range",fixed(*fft->plot(),0,2));const auto liveFFT=fft->sourceSeries();pane.setOrderMarkersVisible(true);const auto liveReferences=fft->plot()->referenceLines();check("confirmed live RPM creates reference overlay without changing FFT bins",!liveFFT.isEmpty()&&!liveReferences.isEmpty()&&liveReferences.first()==QPointF(10,1)&&same(liveFFT,fft->sourceSeries()));pane.setOrderMarkersVisible(false);check("live order overlay can be removed without FFT or range changes",fft->plot()->referenceLines().isEmpty()&&same(liveFFT,fft->sourceSeries())&&fixed(*fft->plot(),0,2));pane.endLive();pane.setView(0);const bool unknownPpr=pane.beginLive(frame(0,1),false,&error);pane.setView(1);pane.setOrderMarkersVisible(true);pump(300);check("unknown PPR never produces RPM order references",unknownPpr&&fft->plot()->referenceLines().isEmpty());pane.endLive();pane.setResult({},&result);check("return from live to offline retains both display ranges",fixed(*wave->plot(),-9,9)&&fixed(*fft->plot(),0,2));}
    QJsonObject report{{"passed",passed},{"checks",checks},{"error",error},{"synthetic",true},{"hardware_confirmed",false},{"scope","Host Qt fixed-axis, injected touch/mouse, isolated QSettings, offline and synthetic live regression. No physical touchscreen or sensor acceptance."}};const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);if(argc>1){QFile out(QString::fromUtf8(argv[1]));if(!out.open(QIODevice::WriteOnly)||out.write(bytes)!=bytes.size())return 2;}std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return passed?0:1;
}
