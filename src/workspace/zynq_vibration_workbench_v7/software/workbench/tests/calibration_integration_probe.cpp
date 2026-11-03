#include "analysis_pane.hpp"
#include "capture_page.hpp"
#include "calibration_settings.hpp"
#include "detection_pane.hpp"
#include "live_preview_model.hpp"
#include "single_waveform_view.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include "window.hpp"
#include "vibration/core.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QTextBrowser>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace vibration;
using namespace vibration::workbench;
namespace {
QJsonArray checks;
QString evidenceDirectory;
void check(bool passed, const QString& name) {
    checks.append(QJsonObject{{"check", name}, {"passed", passed}});
    if (!passed) throw std::runtime_error(name.toStdString());
}
bool near(double a, double b, double tolerance = 2e-10) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}
template<class T> T* child(QObject& owner, const QString& name) {
    auto* result = dynamic_cast<T*>(owner.findChild<QWidget*>(name));
    if (!result) throw std::runtime_error((QStringLiteral("Missing widget: ") + name).toStdString());
    return result;
}
void events(int milliseconds = 0) {
    QElapsedTimer elapsed; elapsed.start();
    do { QApplication::processEvents(); if (milliseconds) QThread::msleep(1); } while (elapsed.elapsed() < milliseconds);
}
void put(QByteArray& bytes, int at, quint64 value, int count) {
    for (int i = 0; i < count; ++i) bytes[at + i] = char(value >> (8 * i));
}
std::shared_ptr<Capture> fixture() {
    auto capture = std::make_shared<Capture>(); capture->integrity_verified = true;
    capture->source_metadata.synthetic = true; capture->source_metadata.status = "synthetic_software_fixture";
    capture->header.sample_rate_hz = 50000; capture->header.clock_hz = 50000000;
    capture->header.adc_range_uv = 5000000; capture->header.pulses_per_rev = 2;
    capture->header.completed_records = 16384; capture->records.resize(16384);
    for (std::size_t i = 0; i < capture->records.size(); ++i) {
        auto& record = capture->records[i]; record.index = i; record.tick = i * 1000;
        record.flags = 3; record.speed_period_ticks = 500000; record.speed_edge_count = std::uint32_t(i / 500);
        for (int channel = 0; channel < 3; ++channel)
            record.raw[channel] = std::int16_t(2500 * (channel + 1) + std::lround((1000 + channel * 700) * std::sin(2 * 3.14159265358979323846 * 32 * double(i) / 8192.0 + channel * .4)));
    }
    return capture;
}
QJsonObject frame(const Capture& capture, int first, int count, int sequence) {
    QByteArray bytes(count * 32, 0);
    for (int i = 0; i < count; ++i) {
        const auto& record = capture.records[std::size_t(first + i)]; const int at = i * 32;
        put(bytes, at, record.index, 8); put(bytes, at + 8, record.tick, 8);
        for (int channel = 0; channel < 3; ++channel) put(bytes, at + 16 + channel * 2, quint16(record.raw[channel]), 2);
        put(bytes, at + 22, record.flags, 2); put(bytes, at + 24, record.speed_period_ticks, 4); put(bytes, at + 28, record.speed_edge_count, 4);
    }
    return {{"schema", "vibration.capture.v2"}, {"type", "event"}, {"event", "preview"}, {"job_id", "synthetic_calibration_integration"},
        {"generation", "1"}, {"frame_seq", sequence}, {"first_record", first}, {"record_count", count}, {"committed_records", first + count},
        {"requested_records", 100000}, {"sample_rate_hz", 50000}, {"clock_hz", 50000000}, {"ppr", 2}, {"adc_range_uv", 5000000},
        {"channel_map", 328449}, {"record_bytes", 32}, {"source", "test_fixture"}, {"hardware_confirmed", false}, {"synthetic", true},
        {"source_flags", 1}, {"validity_flags", 27}, {"raw_base64", QString::fromLatin1(bytes.toBase64())},
        {"crc32", double(crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()), bytes.size()))}, {"dropped_preview_records", 0}};
}
AnalysisConfig config(AmplitudeUnit unit) {
    AnalysisConfig result; result.amplitude_unit = unit; result.fft_size = 8192; result.calibration_confirmed = false;
    result.channel_calibration = std::array<ChannelCalibration, 3>{{{2.0, -10.0, .12}, {4.0, 3.0, -.23}, {8.0, -2.0, .31}}};
    return result;
}
bool exactRecords(const std::vector<Record>& a, const std::vector<Record>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].index != b[i].index || a[i].tick != b[i].tick || a[i].raw != b[i].raw || a[i].flags != b[i].flags ||
            a[i].speed_period_ticks != b[i].speed_period_ticks || a[i].speed_edge_count != b[i].speed_edge_count) return false;
    return true;
}
void verifyVertices(const QVector<PlotSeries>& series, const Capture& raw, const AnalysisConfig& cfg, const QString& context) {
    check(series.size() == 3 && !series[0].points.isEmpty(), context + " exposes all three source series");
    for (int channel = 0; channel < 3; ++channel) {
        bool correct = true;
        for (const auto& point : series[channel].points) {
            if (!std::isfinite(point.y())) continue;
            const auto index = std::size_t(std::llround(point.x() * raw.header.sample_rate_hz));
            correct &= index < raw.records.size() && near(point.x(), double(index) / raw.header.sample_rate_hz) &&
                near(point.y(), adc_to_amplitude(raw.records[index].raw[channel], raw.header.adc_range_uv, channel, cfg));
        }
        check(correct, context + QString(" CH%1 every vertex matches core conversion").arg(channel + 1));
    }
}
void verifyRms(SingleWaveformView& view, const std::array<TimeStats, 3>& expected, const QString& context) {
    for (int channel = 0; channel < 3; ++channel) {
        auto* field = child<QLabel>(view, QString("singleRms%1").arg(channel + 1));
        check(field->text() == QString::number(expected[channel].rms, 'g', 3), context + QString(" CH%1 RMS shown from calibrated raw values").arg(channel + 1));
        check(field->toolTip().contains(QString::number(expected[channel].rms, 'g', 10)), context + QString(" CH%1 high precision RMS agrees with core").arg(channel + 1));
        auto* peak = child<QLabel>(view, QString("singlePeak%1").arg(channel + 1));
        check(peak->text() == QString::number(expected[channel].peak_to_peak, 'g', 3), context + QString(" CH%1 peak-to-peak agrees with core").arg(channel + 1));
    }
}
void verifyCursor(AnalysisPane& pane, SingleWaveformView& view, const Capture& raw, const AnalysisConfig& cfg, const QString& context) {
    const auto& point = view.sourceSeries()[0].points[view.sourceSeries()[0].points.size() / 3];
    const auto index = std::size_t(std::llround(point.x() * raw.header.sample_rate_hz));
    view.plot()->selectAtX(point.x()); events(); const auto text = child<QLabel>(pane, "waveCursorReadout")->text();
    for (int channel = 0; channel < 3; ++channel) {
        const auto value = adc_to_amplitude(raw.records[index].raw[channel], raw.header.adc_range_uv, channel, cfg);
        // Live cursor renders seven significant figures; offline uses six.
        check(text.contains(QString::number(value, 'g', context.startsWith("live") ? 7 : 6)), context + QString(" CH%1 cursor agrees with core").arg(channel + 1));
    }
    check(text.contains(QString::fromStdString(amplitude_unit_symbol(cfg))), context + " cursor displays active unit");
}
void verifyFrozenLiveCursor(AnalysisPane& pane, SingleWaveformView& view, const Capture& raw, const AnalysisConfig& cfg, double time, const QString& context) {
    const auto selected=view.plot()->cursorX();
    check(selected&&near(*selected,time),context+" retains the selected raw sample time");
    const auto index=std::size_t(std::llround(time*raw.header.sample_rate_hz));
    const auto text=child<QLabel>(pane,"waveCursorReadout")->text();
    for(int channel=0;channel<3;++channel)
        check(text.contains(QString::number(adc_to_amplitude(raw.records[index].raw[channel],raw.header.adc_range_uv,channel,cfg),'g',7)),context+QString(" CH%1 existing cursor reading adopts active conversion").arg(channel+1));
    check(text.endsWith(QString::fromStdString(amplitude_unit_symbol(cfg))),context+" existing cursor unit adopts active conversion");
}
void verifySpectra(const QVector<PlotSeries>& series, const std::array<Spectrum, 3>& spectra, const AnalysisConfig& cfg, bool voltageInput, const QString& context) {
    check(series.size() == 3 && !series[0].points.isEmpty(), context + " all three FFT series are available");
    for (int channel = 0; channel < 3; ++channel) {
        const double slope = voltageInput ? std::abs(voltage_to_amplitude(1.0, channel, cfg) - voltage_to_amplitude(0.0, channel, cfg)) : 1;
        bool correct = series[channel].points.size() == int(spectra[channel].frequency_hz.size());
        for (int i = 0; correct && i < series[channel].points.size(); ++i)
            correct &= near(series[channel].points[i].x(), spectra[channel].frequency_hz[std::size_t(i)]) &&
                near(series[channel].points[i].y(), spectra[channel].peak_amplitude[std::size_t(i)] * slope, 3e-9);
        check(correct, context + QString(" CH%1 FFT matches core absolute slope without DC bias").arg(channel + 1));
    }
}
void paneTests(const std::shared_ptr<Capture>& raw) {
    const auto originals = raw->records;
    std::vector<Record> snapshot(raw->records.end() - 8192, raw->records.end());
    const auto voltageSpectra = analyze_voltage_spectra(snapshot, 50000, 50000000, 5000000, 8192, 2000);
    for (const auto unit : {AmplitudeUnit::Voltage, AmplitudeUnit::AccelerationSI, AmplitudeUnit::Gravity}) {
        const auto cfg = config(unit); const auto suffix = QString::fromStdString(amplitude_unit_symbol(cfg));
        const auto result = analyze(*raw, cfg); AnalysisPane offline; offline.resize(1280, 720); offline.show(); offline.setResult(raw, &result); events();
        DetectionPane detection; detection.setResult(&result, nullptr);
        const auto features = child<QTextBrowser>(detection, "detectionFeatures")->toPlainText();
        const auto evidence = child<QTextBrowser>(detection, "detectionEvidence")->toPlainText();
        check(features.contains("RMS / " + suffix) && features.contains(QStringLiteral("峰值 / ") + suffix), "detection " + suffix + " numerical feature headings use actual amplitude unit");
        check(evidence.contains(unit == AmplitudeUnit::Voltage ? QStringLiteral("幅值为原始 ADC 电压 V") : QStringLiteral("幅值按用户输入参数换算为 ") + suffix), "detection " + suffix + " evidence explains active unit and nominal conversion");
        if (unit != AmplitudeUnit::Gravity) check(!features.contains("/ g"), "detection " + suffix + " has no stale gravity feature headings");
        for (int channel = 0; channel < 3; ++channel)
            check(features.contains(QString::number(result.time_stats[channel].ac_rms, 'g', 5)), QString("detection %1 CH%2 AC RMS matches core result").arg(suffix).arg(channel + 1));
        auto* wave = child<SingleWaveformView>(offline, "singleWaveformView");
        verifyVertices(wave->sourceSeries(), *raw, cfg, "offline " + suffix); verifyRms(*wave, result.time_stats, "offline " + suffix);
        verifyCursor(offline, *wave, *raw, cfg, "offline " + suffix);
        offline.setView(1); events(); auto* frequency = child<SingleWaveformView>(offline, "singleSpectrumView");
        verifySpectra(frequency->sourceSeries(), result.spectra, cfg, false, "offline " + suffix);
        check(!wave->speedSeries().points.isEmpty() && near(wave->speedSeries().points.back().y(), 3000), "offline " + suffix + " RPM unaffected by acceleration conversion");

        AnalysisPane live; live.resize(1280, 720); live.show(); live.setAnalysisConfig(cfg); events(); QString error;
        LivePreviewModel voltageModel;
        for (int sequence = 1; sequence <= 4; ++sequence) {
            const auto event = frame(*raw, (sequence - 1) * 4096, 4096, sequence);
            check(sequence == 1 ? live.beginLive(event, true, &error) : live.acceptLiveFrame(event, &error), "live " + suffix + " accepts explicit synthetic raw frame");
            check(sequence == 1 ? voltageModel.begin(event, true, &error) : voltageModel.accept(event, &error), "raw voltage model accepts identical frame");
        }
        events(90); auto* liveWave = child<SingleWaveformView>(live, "singleWaveformView");
        verifyVertices(liveWave->sourceSeries(), *raw, cfg, "live " + suffix); verifyCursor(live, *liveWave, *raw, cfg, "live " + suffix);
        if (!evidenceDirectory.isEmpty() && unit == AmplitudeUnit::AccelerationSI) {
            liveWave->setChannelChecked(1, true); liveWave->setChannelChecked(2, true); events();
            check(live.grab().save(evidenceDirectory + "/live-si-three-channel.png"), "actual live SI conversion view screenshot saved");
        }
        auto visibleCapture = *raw; visibleCapture.records.clear();
        for (const auto& point : liveWave->sourceSeries()[0].points) if (std::isfinite(point.y()))
            visibleCapture.records.push_back(raw->records[std::size_t(std::llround(point.x() * 50000))]);
        visibleCapture.header.completed_records = visibleCapture.records.size();
        verifyRms(*liveWave, analyze(visibleCapture, cfg).time_stats, "live " + suffix);
        const auto voltageView = voltageModel.view(.25, true, true); bool rawVoltage = true;
        for (int channel = 0; channel < 3; ++channel) for (const auto& point : voltageView.voltage[channel].points) {
            const auto index = std::size_t(std::llround(point.x() * 50000));
            if (std::isfinite(point.y())) rawVoltage &= near(point.y(), raw->records[index].raw[channel] * 5.0 / 32768.0);
        }
        check(rawVoltage && exactRecords(raw->records, originals), "live " + suffix + " raw voltage buffers and original records remain unchanged");
        check(!liveWave->speedSeries().points.isEmpty() && near(liveWave->speedSeries().points.back().y(), 3000), "live " + suffix + " RPM unaffected by calibration");
        live.setView(1); QElapsedTimer waiting; waiting.start(); auto* liveFrequency = child<SingleWaveformView>(live, "singleSpectrumView");
        while ((liveFrequency->sourceSeries().size() != 3 || liveFrequency->sourceSeries()[0].points.isEmpty()) && waiting.elapsed() < 6000) events(5);
        verifySpectra(liveFrequency->sourceSeries(), voltageSpectra, cfg, true, "live " + suffix);
        if(unit==AmplitudeUnit::Voltage){
            live.setView(0);verifyCursor(live,*liveWave,*raw,cfg,"live standalone before pause");
            child<QPushButton>(live,"liveFollow")->click();events();const auto selected=*liveWave->plot()->cursorX();
            const auto frozenRange=liveWave->plot()->explicitXRange();
            for(const auto frozenUnit:{AmplitudeUnit::AccelerationSI,AmplitudeUnit::Gravity,AmplitudeUnit::Voltage}){
                auto converted=cfg;converted.amplitude_unit=frozenUnit;const auto name="live standalone paused "+QString::fromStdString(amplitude_unit_symbol(converted));
                live.setAnalysisConfig(converted);events();verifyVertices(liveWave->sourceSeries(),*raw,converted,name);
                verifyFrozenLiveCursor(live,*liveWave,*raw,converted,selected,name);
                check(liveWave->plot()->explicitXRange()==frozenRange,name+" preserves the frozen horizontal viewport");
                live.setView(1);events();verifySpectra(liveFrequency->sourceSeries(),voltageSpectra,converted,true,name);
                liveFrequency->plot()->selectAtX(liveFrequency->sourceSeries()[0].points[8].x());events();
                check(child<QLabel>(live,"spectrumCursorReadout")->text().endsWith(QString::fromStdString(amplitude_unit_symbol(converted))),name+" standalone spectrum cursor retains SI/g compatibility");
                live.setView(0);
            }
        }
        live.endLive();
        AnalysisPane gapPane; gapPane.resize(1280, 720); gapPane.show(); gapPane.setAnalysisConfig(cfg);
        check(gapPane.beginLive(frame(*raw, 0, 4096, 1), true, &error) && gapPane.acceptLiveFrame(frame(*raw, 8192, 4096, 2), &error), "live " + suffix + " explicit preview gap fixture accepted");
        events(90); auto* gapWave = child<SingleWaveformView>(gapPane, "singleWaveformView");
        verifyVertices(gapWave->sourceSeries(), *raw, cfg, "live gap " + suffix);
        for (int channel = 0; channel < 3; ++channel) {
            int breaks = 0; for (const auto& point : gapWave->sourceSeries()[channel].points) if (!std::isfinite(point.y())) ++breaks;
            check(breaks == 1, QString("live %1 CH%2 retains NaN gap after calibration conversion").arg(suffix).arg(channel + 1));
        }
        gapPane.endLive();
    }
}
void settings(Window& window, const std::function<void(QDialog&)>& action) {
    qa::interaction::tap(qa::interaction::button(window,"workbenchOptions"));
    bool seen = false; QString error;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { error = "settings modal absent"; return; }
        seen = true;
        try { action(*dialog); }
        catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); dialog->reject(); }
    });
    qa::interaction::tap(child<QPushButton>(window, "workbenchSettings"));
    check(seen && error.isEmpty(), "actual Window settings workflow: " + error);
}
void showIsolated(Window& window) {
    // Window schedules connection on its first event-loop turn. Override its
    // service command before that turn so this numerical/UI probe cannot
    // start any capture process, even if a service binary is installed nearby.
    child<CapturePage>(window, "capturePage")->setServiceCommand("/nonexistent/calibration-probe-no-hardware");
    window.resize(1280, 720); window.show(); events();
}
bool sameSeriesBits(const QVector<PlotSeries>& a,const QVector<PlotSeries>& b) {
    if(a.size()!=b.size())return false;
    for(int ch=0;ch<a.size();++ch){if(a[ch].name!=b[ch].name||a[ch].points.size()!=b[ch].points.size())return false;
        for(int i=0;i<a[ch].points.size();++i){const double ax=a[ch].points[i].x(),ay=a[ch].points[i].y(),bx=b[ch].points[i].x(),by=b[ch].points[i].y();
            if(std::memcmp(&ax,&bx,sizeof(double))||std::memcmp(&ay,&by,sizeof(double)))return false;}}
    return true;
}
void waitWindow(Window& window) {
    QElapsedTimer clock;clock.start();while(window.isAnalyzing()&&clock.elapsed()<10000)events(5);
    check(!window.isAnalyzing(),"Window synthetic BIN analysis completes");events(40);
}
QString writeWindowFixture(const Capture& raw,const QString& directory) {
    QByteArray payload(int(raw.records.size()*32),0);
    for(std::size_t i=0;i<raw.records.size();++i){const auto& r=raw.records[i];const int at=int(i*32);put(payload,at,r.index,8);put(payload,at+8,r.tick,8);
        for(int ch=0;ch<3;++ch)put(payload,at+16+ch*2,quint16(r.raw[ch]),2);put(payload,at+22,r.flags,2);put(payload,at+24,r.speed_period_ticks,4);put(payload,at+28,r.speed_edge_count,4);}
    QByteArray header(4096,0);header.replace(0,8,QByteArray("CAPDDR2\0",8));
    for(const auto& item:std::initializer_list<std::pair<int,quint64>>{{8,2},{12,4096},{16,32},{20,2},{24,raw.header.sample_rate_hz},{28,raw.header.clock_hz},{32,raw.records.size()},{36,raw.records.size()},{40,quint64(payload.size())},{60,raw.header.pulses_per_rev},{64,raw.records.front().tick},{72,raw.records.back().tick},{80,0x50301},{84,5000000},{88,5},{92,100000000}})
        put(header,item.first,item.second,item.first==64||item.first==72?8:4);
    put(header,96,crc32(reinterpret_cast<const std::uint8_t*>(payload.constData()),std::size_t(payload.size())),4);
    put(header,100,crc32(reinterpret_cast<const std::uint8_t*>(header.constData()),128),4);
    const auto path=directory+"/window-fixture.BIN";QFile bin(path);check(bin.open(QIODevice::WriteOnly)&&bin.write(header)==header.size()&&bin.write(payload)==payload.size(),"Write only isolated synthetic Window BIN fixture");bin.close();
    QFile metadata(directory+"/window-fixture.meta.json");check(metadata.open(QIODevice::WriteOnly)&&metadata.write("{\"synthetic\":true}")>0,"Declare Window fixture synthetic");
    return path;
}
QDialog* openSensitivity(Window& window) {
    auto* dialog=child<QDialog>(window,"accelerationSensitivityDialog");
    if(!dialog->isVisible()){
        qa::interaction::tap(qa::interaction::button(window,"workbenchOptions"));
        auto* entry=child<QPushButton>(window,"openAccelerationSensitivity");
        check(entry->isVisible()&&entry->isEnabled()&&entry->height()>=44,"acceleration sensitivity has a visible touch entry");qa::interaction::tap(entry);events(20);
    }
    check(dialog->isVisible()&&dialog->isModal(),"sensitivity entry opens its same-page dialog");return dialog;
}
void windowLiveModeTests(const std::shared_ptr<Capture>& raw) {
    for(const bool gestureFreeze:{false,true}){
        QTemporaryDir directory;check(directory.isValid(),"isolated managed live mode fixture");
        auto volts=config(AmplitudeUnit::Voltage);
        check(QDir().mkpath(directory.path()+"/.vibration-workbench"),"create only managed live fixture parameter directory");
        {QFile file(directory.path()+"/.vibration-workbench/calibration.json");const auto bytes=QJsonDocument(calibration_settings_json(volts)).toJson();check(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size(),"seed managed live independent signed calibration");}
        Window window(nullptr,directory.path());showIsolated(window);auto* page=child<CapturePage>(window,"capturePage");
        auto* pane=child<AnalysisPane>(window,"analysisPane");auto* wave=child<SingleWaveformView>(window,"singleWaveformView");auto* frequency=child<SingleWaveformView>(window,"singleSpectrumView");
        for(int sequence=1;sequence<=3;++sequence)page->liveFrame(frame(*raw,(sequence-1)*4096,4096,sequence));
        window.setView(1);QElapsedTimer waiting;waiting.start();
        while((frequency->sourceSeries().size()!=3||frequency->sourceSeries()[0].points.isEmpty())&&waiting.elapsed()<6000)events(5);
        check(frequency->sourceSeries().size()==3&&!frequency->sourceSeries()[0].points.isEmpty(),"managed Window live FFT becomes available");
        // Source adoption and its scheduled paint are separate event-loop
        // turns. selectAtX intentionally rejects a canvas never painted yet.
        frequency->plot()->grab();
        const auto originalSpectrum=frequency->sourceSeries();frequency->plot()->selectAtX(originalSpectrum[0].points[8].x());events();
        const auto voltageSpectrumCursor=child<QLabel>(*pane,"spectrumCursorReadout")->text();check(voltageSpectrumCursor.endsWith(" V"),"managed live spectrum cursor starts in volts");
        window.setView(0);events();const auto originalVolts=wave->sourceSeries();
        auto acceleration=volts;acceleration.amplitude_unit=AmplitudeUnit::AccelerationSI;
        if(gestureFreeze)window.setView(2);
        verifyCursor(*pane,*wave,*raw,gestureFreeze?acceleration:volts,"live managed before freeze");
        if(gestureFreeze)check(wave->plot()->zoomX(.8),"managed acceleration horizontal navigation freezes live display");
        else child<QPushButton>(*pane,"liveFollow")->click();
        events();check(child<QLabel>(*pane,"plotNavigationNote")->text().contains(QStringLiteral("暂停")),"managed live snapshot is paused before mode changes");
        const auto selected=*wave->plot()->cursorX();const auto frozenRange=wave->plot()->explicitXRange();
        page->liveFrame(frame(*raw,12288,4096,4));QElapsedTimer accepted;accepted.start();
        while(pane->property("liveAcceptedFrames").toULongLong()<4&&accepted.elapsed()<3000)events(5);
        check(pane->property("liveAcceptedFrames").toULongLong()==4&&wave->plot()->explicitXRange()==frozenRange,"paused display continues accepting later raw chunks without adopting their viewport");
        const auto freezeName=gestureFreeze?QStringLiteral("gesture-frozen"):QStringLiteral("button-paused");
        for(const bool accelerationPage:{true,false,true,false}){
            window.setView(accelerationPage?2:0);events();const auto& cfg=accelerationPage?acceleration:volts;
            verifyVertices(wave->sourceSeries(),*raw,cfg,"live managed "+freezeName);
            verifyFrozenLiveCursor(*pane,*wave,*raw,cfg,selected,"live managed "+freezeName);
            check(wave->plot()->explicitXRange()==frozenRange,"managed paused mode changes preserve horizontal viewport");
            if(!accelerationPage)check(sameSeriesBits(wave->sourceSeries(),originalVolts),"managed paused time page restores all original voltage vertices bitwise");
            window.setView(1);events();
            check(sameSeriesBits(frequency->sourceSeries(),originalSpectrum),"managed paused acceleration to spectrum and reverse preserve every voltage FFT bin bitwise");
            check(child<QLabel>(*pane,"spectrumCursorReadout")->text()==voltageSpectrumCursor,"managed paused spectrum retains voltage cursor values and unit");
        }
        window.setView(2);auto* sensitivityDialog=openSensitivity(window);child<QDoubleSpinBox>(*sensitivityDialog,"accelerationSensitivity1")->setValue(3.75);child<QPushButton>(*sensitivityDialog,"applyAccelerationSensitivity")->click();events();
        (*acceleration.channel_calibration)[0].sensitivity_mv_per_m_s2=3.75;
        verifyVertices(wave->sourceSeries(),*raw,acceleration,"live managed paused sensitivity apply");verifyFrozenLiveCursor(*pane,*wave,*raw,acceleration,selected,"live managed paused sensitivity apply");
        window.setView(1);events();check(sameSeriesBits(frequency->sourceSeries(),originalSpectrum)&&child<QLabel>(*pane,"spectrumCursorReadout")->text()==voltageSpectrumCursor,"managed paused sensitivity apply leaves voltage FFT and its existing cursor bitwise unchanged");
        pane->endLive();
    }
}
void sensitivityOnly(Window& window,const QString& context) {
    auto* dialog=openSensitivity(window);auto* bar=child<QWidget>(*dialog,"accelerationSensitivitySettings");
    check(bar->isVisible()&&bar->findChildren<QDoubleSpinBox*>().size()==3&&bar->findChildren<QComboBox*>().isEmpty(),context+" exposes only three sensitivity numbers");
    for(const char* name:{"calibrationAmplitudeUnit","calibrationGain1","calibrationGain2","calibrationGain3","calibrationBias1","calibrationBias2","calibrationBias3"})
        check(!window.findChild<QWidget*>(name),context+" hides legacy conversion UI "+name);
    for(const QString& name:{QStringLiteral("accelerationSensitivity1"),QStringLiteral("accelerationSensitivity2"),QStringLiteral("accelerationSensitivity3"),QStringLiteral("applyAccelerationSensitivity")}){
        auto* control=child<QWidget>(*dialog,name);check(control->isVisible()&&dialog->rect().contains(QRect(control->mapTo(dialog,QPoint()),control->size()))&&control->height()>=44,context+" fits touch control "+name);
    }
}
void windowTests() {
    QTemporaryDir directory;check(directory.isValid(),"isolated writable sensitivity persistence fixture");
    const auto raw=fixture();const auto path=writeWindowFixture(*raw,directory.path());
    auto cfg=config(AmplitudeUnit::AccelerationSI);
    check(QDir().mkpath(directory.path()+"/.vibration-workbench"),"create only own legacy parameter directory");
    {QFile legacy(directory.path()+"/.vibration-workbench/calibration.json");const auto bytes=QJsonDocument(calibration_settings_json(cfg)).toJson();check(legacy.open(QIODevice::WriteOnly)&&legacy.write(bytes)==bytes.size(),"seed legacy SI preference and signed background gain/bias for compatibility regression");}
    const std::array<double,3> sensitivities{{2.5,4.5,8.5}};
    {
        Window window(nullptr,directory.path());showIsolated(window);
        check(child<QPushButton>(window,"viewTab0")->isChecked()&&child<QLabel>(window,"singleWaveUnit")->text().endsWith("V"),"Window startup chooses volts despite legacy saved SI preference");
        window.setView(2);events();check(child<QLabel>(window,"singleWaveUnit")->text().contains("m/s"),"empty acceleration page immediately labels SI without waiting for data");
        window.setView(0);events();check(child<QLabel>(window,"singleWaveUnit")->text().endsWith("V"),"empty time page immediately restores volts");
        window.openCapture(path);
        // No event pumping: LoadJob still owns its configuration snapshot.
        window.setView(2);auto* busyInput=child<QDoubleSpinBox>(window,"accelerationSensitivity1");auto* busyApply=child<QPushButton>(window,"applyAccelerationSensitivity");const auto busyValue=busyInput->value();
        check(window.isAnalyzing()&&!busyInput->isEnabled()&&!busyApply->isEnabled()&&!child<QPushButton>(window,"openAccelerationSensitivity")->isEnabled(),"loading disables sensitivity entry editing and apply before queued completion");
        busyInput->setValue(7.25);busyApply->click();
        check(child<QLabel>(window,"documentState")->text()==QStringLiteral("正在分析"),"disabled apply cannot replace loading state");
        {QFile file(window.property("calibrationSettingsPath").toString());check(file.open(QIODevice::ReadOnly),"read seeded calibration while load is busy");check(near(QJsonDocument::fromJson(file.readAll()).object().value("channel_calibration").toArray()[0].toObject().value("sensitivity_mv_per_m_s2").toDouble(),busyValue),"loading-time apply cannot overwrite persisted sensitivity");}
        busyInput->setValue(busyValue);waitWindow(window);check(busyInput->isEnabled()&&busyApply->isEnabled(),"completed loading restores sensitivity editing and apply");window.setView(0);events();
        check(child<QPushButton>(window,"viewTab0")->isChecked()&&child<QLabel>(window,"singleWaveUnit")->text().endsWith("V"),"loaded time page displays original volts after busy sensitivity guard");
        check(child<QPushButton>(window,"viewTab0")->text()==QStringLiteral("时域")&&child<QPushButton>(window,"viewTab1")->text()==QStringLiteral("频谱")&&child<QPushButton>(window,"viewTab2")->text()==QStringLiteral("加速度")&&child<QPushButton>(window,"viewTab3")->text()==QStringLiteral("检测"),"Window exposes ordered time spectrum acceleration detection tabs");
        auto* wave=child<SingleWaveformView>(window,"singleWaveformView");auto voltageCfg=cfg;voltageCfg.amplitude_unit=AmplitudeUnit::Voltage;
        verifyVertices(wave->sourceSeries(),*raw,voltageCfg,"Window time volts");verifyRms(*wave,analyze(*raw,voltageCfg).time_stats,"Window time volts");const auto originalVolts=wave->sourceSeries();
        window.setView(1);events();auto* frequency=child<SingleWaveformView>(window,"singleSpectrumView");const auto originalSpectrum=frequency->sourceSeries();
        window.setView(3);events();const auto originalFeatures=child<QTextBrowser>(window,"detectionFeatures")->toPlainText();
        wave->setSpeedSelected(true);window.setView(2);events();sensitivityOnly(window,"Window acceleration");
        check(!wave->showingSpeed()&&child<QLabel>(window,"singleWaveUnit")->text().contains("m/s"),"Acceleration tab leaves prior time-page RPM mode and displays SI vibration");
        for(int ch=0;ch<3;++ch)check(near(child<QDoubleSpinBox>(window,QString("accelerationSensitivity%1").arg(ch+1))->value(),effective_channel_calibration(cfg,ch).sensitivity_mv_per_m_s2),QString("Window loads independent legacy CH%1 sensitivity without exposing background calibration").arg(ch+1));
        verifyVertices(wave->sourceSeries(),*raw,cfg,"Window acceleration before editing");const auto previousAcceleration=wave->sourceSeries();
        for(int ch=0;ch<3;++ch)child<QDoubleSpinBox>(window,QString("accelerationSensitivity%1").arg(ch+1))->setValue(sensitivities[ch]);
        check(sameSeriesBits(wave->sourceSeries(),previousAcceleration),"Editing sensitivity values does not change the curve before explicit apply");
        child<QPushButton>(window,"applyAccelerationSensitivity")->click();events(40);
        check(child<QLabel>(window,"documentState")->text()==QStringLiteral("合成演示")&&child<QLabel>(window,"workbenchStatus")->text().contains(QStringLiteral("非实测数据")),"sensitivity apply preserves both synthetic source warnings");
        for(int ch=0;ch<3;++ch)(*cfg.channel_calibration)[ch].sensitivity_mv_per_m_s2=sensitivities[ch];
        verifyVertices(wave->sourceSeries(),*raw,cfg,"Window acceleration after apply");
        verifyRms(*wave,analyze(*raw,cfg).time_stats,"Window acceleration after apply");
        const auto y=wave->plot()->displayedYRange();check(wave->plot()->zeroCenteredY()&&y.y()>0&&near(y.x(),-y.y()),"Window acceleration viewport is zero-centered with original biased sample values");
        for(const auto& size:{QSize(1024,600),QSize(1280,720)}){window.resize(size);events(30);sensitivityOnly(window,QString("Window acceleration %1x%2").arg(size.width()).arg(size.height()));if(!evidenceDirectory.isEmpty())check(child<QDialog>(window,"accelerationSensitivityDialog")->grab().save(evidenceDirectory+QString("/window-sensitivity-dialog-%1x%2.png").arg(size.width()).arg(size.height())),"sensitivity dialog screenshot saved at physical layout size");}
        child<QDialog>(window,"accelerationSensitivityDialog")->reject();events();
        const auto settingsPath=window.property("calibrationSettingsPath").toString();QFile saved(settingsPath);check(saved.open(QIODevice::ReadOnly),"inline sensitivity apply saves at declared settings path");const auto object=QJsonDocument::fromJson(saved.readAll()).object();const auto channels=object.value("channel_calibration").toArray();
        check(object.value("amplitude_unit")=="V"&&!object.contains("calibration_confirmed")&&channels.size()==3,"saved sensitivity values retain original volts analysis and no physical confirmation claim");
        for(int ch=0;ch<3;++ch){const auto expected=effective_channel_calibration(cfg,ch);const auto stored=channels[ch].toObject();check(near(stored.value("sensitivity_mv_per_m_s2").toDouble(),sensitivities[ch])&&near(stored.value("analog_gain").toDouble(),expected.analog_gain)&&near(stored.value("voltage_bias_v").toDouble(),expected.voltage_bias_v),QString("saving CH%1 sensitivity preserves exact signed gain and supplied voltage bias").arg(ch+1));}
        settings(window,[&](QDialog& dialog){auto* tabs=child<QTabWidget>(dialog,"workbenchSettingsTabs");check(tabs->count()==2&&!dialog.findChild<QWidget*>("calibrationAmplitudeUnit")&&!dialog.findChild<QWidget*>("calibrationSensitivity1")&&!dialog.findChild<QWidget*>("calibrationGain1")&&!dialog.findChild<QWidget*>("calibrationBias1"),"Window settings retain analysis/detection and omit complex conversion page");check(!child<QCheckBox>(dialog,"analysisCalibration")->isChecked(),"synthetic records and sensitivity input do not imply actual calibration");dialog.reject();});
        window.setView(0);events();check(child<QLabel>(window,"singleWaveUnit")->text().endsWith("V")&&sameSeriesBits(wave->sourceSeries(),originalVolts),"Returning from acceleration restores every original voltage point bitwise");
        window.setView(1);events();check(sameSeriesBits(frequency->sourceSeries(),originalSpectrum),"Sensitivity-only editing leaves all original voltage FFT bins bitwise unchanged");
        window.setView(3);events();check(child<QTextBrowser>(window,"detectionFeatures")->toPlainText()==originalFeatures,"Acceleration page does not rescale original voltage detection features");
        check(exactRecords(load_capture(path.toUtf8().toStdString()).records,raw->records),"Window tab changes and parameter saving do not mutate original ADC records");
    }
    {
        Window reloaded(nullptr,directory.path());showIsolated(reloaded);reloaded.openCapture(path);waitWindow(reloaded);
        check(child<QPushButton>(reloaded,"viewTab0")->isChecked()&&child<QLabel>(reloaded,"singleWaveUnit")->text().endsWith("V"),"Restart after saving sensitivities returns to voltage time page");
        reloaded.setView(2);events();openSensitivity(reloaded);
        for(int ch=0;ch<3;++ch)check(near(child<QDoubleSpinBox>(reloaded,QString("accelerationSensitivity%1").arg(ch+1))->value(),sensitivities[ch]),QString("Restart restores independent CH%1 sensitivity").arg(ch+1));
        verifyVertices(child<SingleWaveformView>(reloaded,"singleWaveformView")->sourceSeries(),*raw,cfg,"restarted Window acceleration");
    }
    QTemporaryDir blocked;check(blocked.isValid(),"isolated failed sensitivity persistence fixture");const auto blocker=blocked.path()+"/.vibration-workbench";
    {QFile file(blocker);check(file.open(QIODevice::WriteOnly)&&file.write("test blocker")==12,"ordinary temporary file blocks only own parameter directory");}
    Window retry(nullptr,blocked.path());showIsolated(retry);retry.setView(2);events();auto* retryDialog=openSensitivity(retry);auto* input=child<QDoubleSpinBox>(*retryDialog,"accelerationSensitivity1");input->setValue(6.25);auto* apply=child<QPushButton>(*retryDialog,"applyAccelerationSensitivity");apply->click();events();
    check(!retry.property("calibrationStorageError").toString().isEmpty()&&near(input->value(),6.25)&&child<QWidget>(*retryDialog,"accelerationSensitivitySettings")->isVisible()&&!child<QLabel>(*retryDialog,"sensitivityDialogStatus")->text().isEmpty(),"unavailable storage retains editable sensitivity dialog with a visible error");
    check(QFile::remove(blocker),"remove only own temporary persistence blocker");apply->click();events();check(retry.property("calibrationStorageError").toString().isEmpty(),"storage repair retry applies inline sensitivity without reopening");
    {QFile saved(retry.property("calibrationSettingsPath").toString());check(saved.open(QIODevice::ReadOnly),"successful sensitivity retry creates settings file");const auto object=QJsonDocument::fromJson(saved.readAll()).object();const auto stored=object.value("channel_calibration").toArray()[0].toObject();check(near(stored.value("sensitivity_mv_per_m_s2").toDouble(),6.25)&&near(stored.value("analog_gain").toDouble(),-1.),"retry preserves negative default gain while saving sensitivity");}
    settings(retry,[&](QDialog& dialog){check(child<QLabel>(dialog,"analysisSettingsError")->text().isEmpty()&&near(input->value(),6.25),"reopened simple analysis settings clear stale storage error and retain saved sensitivity");dialog.reject();});
}
}
int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs); QApplication app(argc, argv); qa::configureFonts(app);
    qputenv("AX7_LIVE_DEMAND_ANALYSIS", "1");
    if (argc > 1) {
        evidenceDirectory = QFileInfo(QString::fromUtf8(argv[1])).absolutePath();
        if (!QDir().mkpath(evidenceDirectory)) return 2;
    }
    QJsonObject report{{"schema", "v55-calibration-whole-gui-integration-v1"}, {"scope", "Synthetic host UI/core numerical integration and isolated temporary parameter persistence only; no sensor, board, HDMI or calibration accuracy claim."}};
    try { const auto raw=fixture();paneTests(raw);windowLiveModeTests(raw);windowTests(); report["passed"] = true; }
    catch (const std::exception& exception) { report["passed"] = false; report["error"] = QString::fromUtf8(exception.what()); }
    report["checks"] = checks; const auto bytes = QJsonDocument(report).toJson();
    if (argc > 1) { QFile output(QString::fromUtf8(argv[1])); if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()) return 2; }
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stdout); return report["passed"].toBool() ? 0 : 1;
}
