#include "window.hpp"
#include "analysis_pane.hpp"
#include "capture_client.hpp"
#include "capture_page.hpp"
#include "plot_canvas.hpp"
#include "waveform_stack.hpp"
#include "vibration/core.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSysInfo>
#include <QTextCodec>
#include <QTextLayout>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#if defined(Q_OS_UNIX)
#include <sys/resource.h>
#include <unistd.h>
#endif
using namespace vibration;
using namespace vibration::workbench;
namespace {
template<class T> T* find(QWidget& owner, const char* name) {
    auto* out = dynamic_cast<T*>(owner.findChild<QWidget*>(name));
    if (!out) throw std::runtime_error(std::string("Missing control: ") + name);
    return out;
}
void require(bool value, const QString& error) { if (!value) throw std::runtime_error(error.toStdString()); }
bool waitFor(const std::function<bool()>& done, int timeout) {
    if (done()) return true;
    QEventLoop loop; QTimer poll, deadline; poll.setInterval(5); deadline.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (done()) loop.quit(); });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(); deadline.start(timeout); loop.exec(); return done();
}
QJsonObject distribution(std::vector<qint64> data) {
    if (data.empty()) return {{"samples", 0}};
    std::sort(data.begin(), data.end());
    return {{"samples", int(data.size())}, {"p50_ms", data[(data.size() - 1) / 2] / 1e6},
        {"p95_ms", data[std::size_t((data.size() - 1) * .95)] / 1e6}, {"max_ms", data.back() / 1e6}};
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
double selfCpu() {
#if defined(Q_OS_UNIX)
    rusage usage{}; if (!getrusage(RUSAGE_SELF, &usage)) return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
#endif
    return 0;
}
double processCpu(qint64 pid) {
    const auto text = QString::fromLatin1(read(QStringLiteral("/proc/%1/stat").arg(pid)));
    const auto fields = text.mid(text.lastIndexOf(')') + 2).split(' ', Qt::SkipEmptyParts);
#if defined(Q_OS_UNIX)
    const auto hz = sysconf(_SC_CLK_TCK);
    if (fields.size() > 12 && hz > 0) return (fields[11].toDouble() + fields[12].toDouble()) / hz;
#endif
    return 0;
}
QJsonObject memory() {
    QJsonObject out;
    for (const auto& line : read("/proc/self/status").split('\n')) for (const auto key : {"VmRSS:", "VmHWM:"}) if (line.startsWith(key)) {
        const auto values = line.mid(int(QByteArray(key).size())).simplified().split(' '); out[QString::fromLatin1(key).chopped(1) + "_kib"] = values.isEmpty() ? 0 : values[0].toDouble();
    }
    return out;
}
QString sha256(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "Cannot read final file hash"); QCryptographicHash hash(QCryptographicHash::Sha256);
    require(hash.addData(&file), "Cannot hash complete file"); return QString::fromLatin1(hash.result().toHex());
}
struct FinalAnalysisProfile {
    AnalysisPane* analysis; QJsonObject& report;
    bool frozen=false;
    void freeze(){report["analysis_profile"]=analysis->performanceProfile();frozen=true;}
    ~FinalAnalysisProfile() { if(!frozen)report["analysis_profile"] = analysis->performanceProfile(); }
};
QJsonObject lifecycle(Window& window,CapturePage& page,CaptureClient& client,AnalysisPane& analysis,const QString& output,const QString& priorJob){
    require(client.serviceProcessInfo().value("worker_enabled").toBool(),"AX7_BOARD_LIFECYCLE requires explicit capture worker");
    QObject scope;QElapsedTimer clock;clock.start();int phase=0,late=0,invalid=0;QString error;
    QString jobs[2],generations[2];quint64 frames[2]{};qint64 firstNs[2]{},terminalNs[2]{};QJsonObject endings[2];
    QStringList retired{priorJob};
    QObject::connect(&client,&CaptureClient::liveFrame,&scope,[&](const QJsonObject& frame){
        const auto job=frame.value("job_id").toString();if(retired.contains(job)){++late;return;}
        if(jobs[phase].isEmpty()){jobs[phase]=job;generations[phase]=frame.value("generation").toString();firstNs[phase]=clock.nsecsElapsed();}
        if(job!=jobs[phase]||frame.value("generation")!=generations[phase]||frame.value("source")!="hardware_test"||!frame.value("hardware_confirmed").toBool()||!frame.value("synthetic").toBool()||frame.value("source_flags").toInt()!=1||frame.value("sample_rate_hz").toInt()!=50000||frame.value("ppr").toInt()!=1)++invalid;
        ++frames[phase];
    });
    QObject::connect(&page,&CapturePage::captureState,&scope,[&](const QJsonObject& state){
        if(state.contains("preview_error"))error=state.value("preview_error").toString();
        if(QStringList{"completed","failed","cancelled"}.contains(state.value("state").toString())){endings[phase]=state;terminalNs[phase]=clock.nsecsElapsed();}
    });
    QObject::connect(&client,&CaptureClient::connectionFailed,&scope,[&](const QString& why){error=why;});
    const QString cancelDir=output+"/lifecycle-cancel",restartDir=output+"/lifecycle-restart";
    for(const auto& path:{cancelDir,restartDir})require(!QFileInfo::exists(path)&&QDir().mkpath(path),"Lifecycle requires fresh /tmp directories");
    auto prepare=[&](int seconds,const QString& directory){
        window.showAcquisition();find<QComboBox>(page,"captureRate")->setCurrentIndex(0);find<QDoubleSpinBox>(page,"captureDuration")->setValue(seconds);find<QDoubleSpinBox>(page,"capturePpr")->setValue(1);find<QLineEdit>(page,"captureDirectory")->setText(directory);
        auto* start=find<QPushButton>(page,"captureStart");require(start->isVisible()&&start->isEnabled(),"Lifecycle actual start action unavailable");start->click();
    };
    const auto startBefore=clock.nsecsElapsed();prepare(40,cancelDir);
    require(waitFor([&]{return frames[0]>0||!error.isEmpty()||!endings[0].isEmpty();},8000)&&frames[0]>0&&error.isEmpty(),"Lifecycle40s preview did not start: "+error);
    require(waitFor([&]{return clock.nsecsElapsed()-firstNs[0]>=2000000000LL||!error.isEmpty()||!endings[0].isEmpty();},5000)&&endings[0].isEmpty()&&error.isEmpty(),"Lifecycle40s ended before cancel action");
    auto* cancel=find<QPushButton>(window,"workbenchStart");require(cancel->isVisible()&&cancel->isEnabled(),"Lifecycle actual visible cancel button unavailable");
    const auto cancelBefore=clock.nsecsElapsed();cancel->click();const auto cancelAfter=clock.nsecsElapsed();
    require(waitFor([&]{return !endings[0].isEmpty()||!error.isEmpty();},10000)&&error.isEmpty()&&endings[0].value("state")=="cancelled","Lifecycle cancel did not reach validated cancelled state: "+error);
    require(endings[0].value("output_path").toString().isEmpty()&&QDir(cancelDir).entryList({"*.BIN","*.meta.json"},QDir::Files).isEmpty(),"Cancelled lifecycle published a successful record");
    require(waitFor([&]{return !client.hasPending("cancel");},3000),"Lifecycle cancel response still pending");
    retired.append(jobs[0]);const auto cancelProfile=client.transportProfile();phase=1;
    const auto restartBefore=clock.nsecsElapsed();prepare(1,restartDir);
    require(waitFor([&]{return (!endings[1].isEmpty()&&!window.isAnalyzing())||!error.isEmpty();},15000)&&error.isEmpty()&&endings[1].value("state")=="completed","Lifecycle1s restart did not complete: "+error);
    require(frames[1]>0&&!jobs[0].isEmpty()&&jobs[0]!=jobs[1]&&generations[0].toULongLong()>0&&generations[1].toULongLong()>generations[0].toULongLong(),"Lifecycle restart identity/generation did not advance");
    require(invalid==0&&late==0,"Lifecycle invalid source or late old-job preview");
    const auto file=endings[1].value("output_path").toString();const auto data=load_capture(file.toUtf8().toStdString());
    require(data.integrity_verified&&data.records.size()==50000&&data.header.sample_rate_hz==50000&&data.header.pulses_per_rev==1&&data.source_metadata.synthetic,"Lifecycle restarted BIN integrity/count/source failed");
    const auto sidecar=QJsonDocument::fromJson(read(file.left(file.size()-4)+".meta.json")).object();
    require(sidecar.value("actual_source")=="hardware_self_test"&&sidecar.value("synthetic").toBool()&&sidecar.value("hardware_confirmed").toBool()&&!sidecar.value("calibration_confirmed").toBool()&&!sidecar.value("mechanical_vibration_confirmed").toBool(),"Lifecycle metadata source mismatch");
    retired.append(jobs[1]);waitFor([&]{return late>0;},100);require(late==0,"Lifecycle late frame after terminal");
    return {{"enabled",true},{"passed",true},{"scope","Separate lifecycle assertion phase after original10s+5shots; cumulative profiles are not performance comparison inputs. Qt button activation is not physical touch."},
        {"clock","QElapsedTimer monotonic ns; cancel around2s after first validated GUI preview, actual timestamps retained"},{"start_action_before_ns",double(startBefore)},{"first_preview_ns",double(firstNs[0])},{"cancel_action_before_ns",double(cancelBefore)},{"cancel_action_after_ns",double(cancelAfter)},{"cancel_terminal_observed_ns",double(terminalNs[0])},{"cancel_action_to_terminal_observed_ms",(terminalNs[0]-cancelAfter)/1e6},
        {"cancel_job",jobs[0]},{"cancel_generation",generations[0]},{"cancel_frames",double(frames[0])},{"cancel_terminal",endings[0]},{"cancel_no_BIN_or_sidecar",true},{"cancel_ACK_pending",false},{"restart_action_before_ns",double(restartBefore)},{"restart_first_preview_ns",double(firstNs[1])},{"restart_terminal_observed_ns",double(terminalNs[1])},{"restart_job",jobs[1]},{"restart_generation",generations[1]},{"restart_frames",double(frames[1])},{"restart_terminal",endings[1]},{"restart_records",50000},{"restart_file_sha256",sha256(file)},{"late_old_frames",late},{"source_errors",invalid},{"profile_after_cancel_cumulative",cancelProfile},{"profile_after_restart_cumulative",client.transportProfile()},{"analysis_after_lifecycle_cumulative",analysis.performanceProfile()}};
}
}
int main(int argc, char** argv) {
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8")); QApplication app(argc, argv);
    QJsonObject report{{"schema", "workbench.actual-arm-live-qa.v1"}, {"physical_machine_measurement", false},
        {"source", "hardware_self_test"}, {"fixture_fallback", false}, {"performance_threshold_prejudged", false}};
    QString output; bool outputValidated = false, passed = false; quint64 frames = 0; qint64 childPid = 0;
    try {
        require(argc == 2, "Usage: board_live_probe /tmp/NEW_EVIDENCE_DIRECTORY"); output = QDir::cleanPath(QString::fromUtf8(argv[1]));
        require(QDir::isAbsolutePath(output) && output.startsWith("/tmp/") && !QFileInfo::exists(output + "/board-live.json"), "Use a fresh absolute /tmp QA directory");
        require(QDir().mkpath(output), "Cannot create QA directory");
        outputValidated = true;
        report["cpu_architecture"] = QSysInfo::currentCpuArchitecture(); report["qpa_backend"] = QGuiApplication::platformName();
        require(QSysInfo::currentCpuArchitecture().startsWith("arm"), "This probe requires an actual ARM runtime; no host/fixture fallback");
        report["runtime_font_files_loaded"] = QJsonArray::fromStringList(qa::configureFonts(app));
        report["font_audit"] = qa::fontAudit(app.font()); report["font_families"] = report["font_audit"].toObject().value("families");
        const QString captureDirectory = output + "/capture-" + QString::number(QCoreApplication::applicationPid()) + '-' + QDateTime::currentDateTimeUtc().toString("hhmmsszzz");
        require(!QFileInfo::exists(captureDirectory) && QDir().mkpath(captureDirectory), "Cannot create a private new capture directory"); report["capture_directory"] = captureDirectory;
        const bool stageProfiling = qgetenv("AX7_BOARD_CAPTURE_PROFILE") != "0";
        qputenv("AX7_CAPTURE_PROFILE", stageProfiling ? "1" : "0");
        report["stage_profiling_enabled"] = stageProfiling;
        Window window; window.resize(1280, 720); window.show();
        auto* page = find<CapturePage>(window, "capturePage"); auto* analysis = find<AnalysisPane>(window, "analysisPane"); auto* stack = find<WaveformStack>(window, "waveformStack");
        auto* client = page->findChild<CaptureClient*>(); require(client, "Actual CaptureClient missing");
        FinalAnalysisProfile finalAnalysisProfile{analysis, report};
        const auto lifecycleStarted=std::make_shared<bool>(false);
        QObject::connect(client, &CaptureClient::transportProfileUpdated, &app, [&report,lifecycleStarted](const QJsonObject& profile) { report[*lifecycleStarted?"transport_profile_lifecycle_cumulative":"transport_profile"] = profile; });
        QJsonObject capabilities, terminal; QJsonArray sourceErrors, previewErrors, memoryTrace, screenshotIntervals;
        const bool deferPng = qEnvironmentVariableIntValue("AX7_BOARD_DEFER_PNG") != 0;
        report["screenshot_mode"] = deferPng ? "capture_qimage_then_encode_after_capture" : "synchronous_capture_and_png";
        struct DeferredShot { QString name; QImage image; int intervalIndex; };
        std::vector<DeferredShot> deferredShots; deferredShots.reserve(5);
        QElapsedTimer elapsed; elapsed.start(); qint64 firstNs = 0, lastFrameNs = 0, lastHeartbeatNs = 0;
        quint64 priorEnd = 0, skipped = 0, maximumRefreshes = 0; std::vector<qint64> frameIntervals, heartbeatAll, heartbeatWave, heartbeatMerged, rasterWave, rasterMerged;
        const double cpuStart = selfCpu(); double childCpuStart = 0;
        QObject::connect(client, &CaptureClient::responseReceived, &window, [&](const QString& command, const QJsonObject& data) { if (command == "capabilities") capabilities = data; });
        const auto originalFrames=QObject::connect(page, &CapturePage::liveFrame, &window, [&](const QJsonObject& frame) {
            const qint64 now = elapsed.nsecsElapsed(); if (!firstNs) firstNs = now; if (lastFrameNs) frameIntervals.push_back(now - lastFrameNs); lastFrameNs = now; ++frames;
            const bool valid = frame.value("source") == "hardware_test" && frame.value("hardware_confirmed").isBool() && frame.value("hardware_confirmed").toBool() &&
                frame.value("synthetic").isBool() && frame.value("synthetic").toBool() && frame.value("source_flags").toInt() == 1 &&
                frame.value("sample_rate_hz").toInt() == 50000 && frame.value("ppr").toInt() == 1;
            if (!valid) sourceErrors.append(QJsonObject{{"frame_seq", frame.value("frame_seq")}, {"source", frame.value("source")}});
            const auto first = quint64(frame.value("first_record").toDouble()); const auto count = quint64(frame.value("record_count").toDouble());
            skipped += std::max(first >= priorEnd ? first - priorEnd : quint64(0), quint64(frame.value("dropped_preview_records").toDouble())); priorEnd = first + count;
        });
        const auto originalStates=QObject::connect(page, &CapturePage::captureState, &window, [&](const QJsonObject& value) {
            if (value.contains("preview_error")) previewErrors.append(value.value("preview_error"));
            if (QStringList{"completed", "failed", "cancelled"}.contains(value.value("state").toString())) {
                terminal = value; report["transport_profile_at_terminal"] = client->transportProfile();
            }
        });
        page->setServiceCommand("/usr/bin/vibration_capture_service", {"--hardware-self-test"}); window.showAcquisition();
        require(waitFor([&] { return page->property("captureAvailable").toBool() || !page->property("captureDiagnostic").toString().isEmpty(); }, 8000), "Hardware capability response timed out");
        report["capabilities"] = capabilities;
        require(page->property("captureAvailable").toBool() && capabilities.value("backend") == "linux_device" && capabilities.value("hardware_confirmed").toBool() && capabilities.value("preview_available").toBool(),
            "Actual hardware SG preview unavailable: " + page->property("captureDiagnostic").toString());
        find<QComboBox>(*page, "captureRate")->setCurrentIndex(0); find<QDoubleSpinBox>(*page, "captureDuration")->setValue(10);
        find<QDoubleSpinBox>(*page, "capturePpr")->setValue(1); find<QLineEdit>(*page, "captureDirectory")->setText(captureDirectory);
        const auto processInfo = client->serviceProcessInfo(); report["service_process_info"] = processInfo;
        report["capture_worker_requested"] = QString::fromLatin1(qgetenv("AX7_CAPTURE_WORKER"));
        report["capture_worker_effective"] = processInfo.value("worker_enabled");
        require(processInfo.value("running").toBool() && processInfo.value("program") == "/usr/bin/vibration_capture_service" && processInfo.value("arguments").toArray() == QJsonArray{QStringLiteral("--hardware-self-test")} && processInfo.value("owner_thread_matches").toBool(), "Unexpected child command/owner; refusing alternate backend");
        childPid = qint64(processInfo.value("pid").toDouble()); childCpuStart = processCpu(childPid);
        const QString actualExecutable = QFileInfo(QStringLiteral("/proc/%1/exe").arg(childPid)).symLinkTarget();
        report["child_pid"] = double(childPid); report["child_executable"] = actualExecutable;
        report["child_executable_verified"] = childPid > 0 && actualExecutable == QFileInfo("/usr/bin/vibration_capture_service").canonicalFilePath() && actualExecutable == processInfo.value("canonical_program").toString();
        require(report.value("child_executable_verified").toBool(), "Actual child executable does not match the required service path");
        QVector<PlotCanvas*> canvases; for (auto* widget : window.findChildren<QWidget*>()) if (auto* canvas = dynamic_cast<PlotCanvas*>(widget)) canvases.push_back(canvas);
        report["plot_axes_mode_requested"] = QString::fromLatin1(qgetenv("AX7_PLOT_AXES_MODE"));
        report["plot_axes_mode_effective"] = canvases.isEmpty() ? QStringLiteral("none") : canvases.first()->axesMode();
        report["plot_direct_paint_requested"] = QString::fromLatin1(qgetenv("AX7_PLOT_DIRECT_PAINT"));
        report["plot_direct_paint_effective"] = !canvases.isEmpty() && canvases.first()->directPaintEnabled();
        report["plot_presentation_scope"] = "Default uses cached RGB32 canvas image then widget copy. Exact direct opt-in draws exposed pixels on QWidget without that copy; cursor-only strips are redrawn using cached M4 samples. widget_blit_outside_static includes cursor overlay/painter finish in both paths; direct has no canvas image blit. static build counts include any actual direct repaint, including cursor/scroll exposure.";
        report["axes_cache_counter_scope"] = "axes builds/hits count the background layer cache; QStaticText layout caching is selected by mode but is not included in these hit counters";
        QVector<quint64> previousBuilds(canvases.size()); bool initialShot = false, scrolled = false, restored = false, merged = false, mergeShot = false, frequencyShot = false, speedShot = false;
        QVector<quint64> previousM4Builds(canvases.size());
        QVector<quint64> previousAxesBuilds(canvases.size()), previousAxesHits(canvases.size());
        QVector<quint64> previousDirectPaints(canvases.size()), previousImageBlits(canvases.size());
        std::array<std::vector<qint64>, 8> plotStagesWave, plotStagesMerged;
        for (auto* group : {&plotStagesWave, &plotStagesMerged}) for (auto& samples : *group) samples.reserve(128);
        QVector<quint64> waveBuilds(canvases.size()), mergedBuilds(canvases.size());
        QVector<quint64> waveM4Builds(canvases.size()), mergedM4Builds(canvases.size());
        QVector<quint64> waveAxesBuilds(canvases.size()), mergedAxesBuilds(canvases.size()), waveAxesHits(canvases.size()), mergedAxesHits(canvases.size());
        QVector<quint64> waveDirectPaints(canvases.size()), mergedDirectPaints(canvases.size()), waveImageBlits(canvases.size()), mergedImageBlits(canvases.size());
        quint64 waveRefreshStart = 0, waveRefreshEnd = 0, mergedRefreshStart = 0, mergedRefreshEnd = 0; qint64 waveStart = 0, waveEnd = 0, mergedStart = 0, mergedEnd = 0;
        QTimer heartbeat, script, resources; heartbeat.setTimerType(Qt::PreciseTimer); heartbeat.setInterval(10); script.setInterval(40); resources.setInterval(500);
        QObject::connect(&heartbeat, &QTimer::timeout, &window, [&] {
            const auto now = elapsed.nsecsElapsed(); const double age = firstNs ? (now - firstNs) / 1e9 : -1;
            const auto delta = lastHeartbeatNs ? now - lastHeartbeatNs : 0; lastHeartbeatNs = now;
            if (!analysis->isLive()) return; if (delta) heartbeatAll.push_back(delta);
            qint64 raster = 0; std::array<qint64, 8> stages{};
            for (int i = 0; i < canvases.size(); ++i) {
                const auto stats = canvases[i]->renderStats();
                if (stats.staticBuilds != previousBuilds[i]) {
                    raster += stats.lastStaticNs;
                    const std::array<qint64, 8> values{stats.lastBoundsNs, stats.lastEnvelopeNs, stats.lastMapNs, stats.lastRasterNs, stats.lastAxesNs, stats.lastSetupNs, stats.lastOtherNs, stats.lastBlitNs};
                    for (std::size_t stage = 0; stage < stages.size(); ++stage) stages[stage] += values[stage];
                    if (age >= 1 && age < 4) {
                        waveBuilds[i] += stats.staticBuilds - previousBuilds[i]; waveM4Builds[i] += stats.reductions - previousM4Builds[i];
                        waveAxesBuilds[i] += stats.axesBuilds - previousAxesBuilds[i]; waveAxesHits[i] += stats.axesCacheHits - previousAxesHits[i];
                        waveDirectPaints[i] += stats.directPaints - previousDirectPaints[i]; waveImageBlits[i] += stats.cachedImageBlits - previousImageBlits[i];
                    }
                    if (age >= 5.5 && age < 7.5) {
                        mergedBuilds[i] += stats.staticBuilds - previousBuilds[i]; mergedM4Builds[i] += stats.reductions - previousM4Builds[i];
                        mergedAxesBuilds[i] += stats.axesBuilds - previousAxesBuilds[i]; mergedAxesHits[i] += stats.axesCacheHits - previousAxesHits[i];
                        mergedDirectPaints[i] += stats.directPaints - previousDirectPaints[i]; mergedImageBlits[i] += stats.cachedImageBlits - previousImageBlits[i];
                    }
                }
                previousBuilds[i] = stats.staticBuilds;
                previousM4Builds[i] = stats.reductions;
                previousAxesBuilds[i] = stats.axesBuilds; previousAxesHits[i] = stats.axesCacheHits;
                previousDirectPaints[i] = stats.directPaints; previousImageBlits[i] = stats.cachedImageBlits;
            }
            const auto refreshes = analysis->property("liveRenderedFrames").toULongLong();
            maximumRefreshes = std::max(maximumRefreshes, quint64(refreshes));
            if (age >= 1 && age < 4) { if (!waveStart) { waveStart = now; waveRefreshStart = refreshes; } waveEnd = now; waveRefreshEnd = refreshes; if (delta) heartbeatWave.push_back(delta); if (raster) { rasterWave.push_back(raster); for (std::size_t stage = 0; stage < stages.size(); ++stage) plotStagesWave[stage].push_back(stages[stage]); } }
            if (age >= 5.5 && age < 7.5) { if (!mergedStart) { mergedStart = now; mergedRefreshStart = refreshes; } mergedEnd = now; mergedRefreshEnd = refreshes; if (delta) heartbeatMerged.push_back(delta); if (raster) { rasterMerged.push_back(raster); for (std::size_t stage = 0; stage < stages.size(); ++stage) plotStagesMerged[stage].push_back(stages[stage]); } }
        });
        const auto shot = [&](const QString& name) {
            const auto begin = elapsed.nsecsElapsed(); const auto pixmap = window.grab(); QImage image;
            if (deferPng) image = pixmap.toImage();
            const auto grabEnd = elapsed.nsecsElapsed(); const bool captured = !pixmap.isNull();
            const bool saved = !deferPng && pixmap.save(output + '/' + name + ".png"); const auto end = elapsed.nsecsElapsed();
            screenshotIntervals.append(QJsonObject{{"name", name}, {"begin_elapsed_ns", double(begin)}, {"end_elapsed_ns", double(end)},
                {"duration_ms", (end - begin) / 1e6}, {"grab_begin_elapsed_ns", double(begin)}, {"grab_end_elapsed_ns", double(grabEnd)},
                {"grab_ms", (grabEnd - begin) / 1e6}, {"png_deferred", deferPng}, {"captured", captured}, {"saved", saved},
                {"encode_begin_elapsed_ns", deferPng ? QJsonValue() : QJsonValue(double(grabEnd))}, {"encode_end_elapsed_ns", deferPng ? QJsonValue() : QJsonValue(double(end))},
                {"encode_ms", deferPng ? 0.0 : (end - grabEnd) / 1e6}});
            if (deferPng && captured) deferredShots.push_back({name, std::move(image), screenshotIntervals.size() - 1});
            return deferPng ? captured : saved;
        };
        QObject::connect(&script, &QTimer::timeout, &window, [&] {
            if (!analysis->isLive() || !firstNs) return; const double age = (elapsed.nsecsElapsed() - firstNs) / 1e9;
            if (age >= .6 && !initialShot) initialShot = shot("01-live-default");
            if (age >= 4.1 && !scrolled) { scrolled = stack->verticalScrollBar()->maximum() > 0; stack->verticalScrollBar()->setValue(stack->verticalScrollBar()->maximum()); shot("02-live-bottom"); }
            if (age >= 4.35 && !restored) { stack->verticalScrollBar()->setValue(0); restored = true; }
            if (age >= 4.5 && !merged) merged = stack->moveChannel(0, 1, 0) && stack->moveChannel(2, 1, 2);
            if (age >= 5.1 && !mergeShot) mergeShot = merged && stack->panelOrder().size() == 2 && stack->panelChannels(1) == QVector<int>({0,1,2}) && shot("03-live-merged");
            if (age >= 7.8 && !frequencyShot) { window.setView(1); if (age >= 8.0) frequencyShot = shot("04-live-spectrum"); }
            if (age >= 8.4 && !speedShot) { window.setView(2); if (age >= 8.6) speedShot = shot("05-live-speed"); }
        });
        QObject::connect(&resources, &QTimer::timeout, &window, [&] {
            auto value = memory(); value["elapsed_ms"] = elapsed.elapsed(); value["elapsed_ns"] = double(elapsed.nsecsElapsed());
            value["ui_cpu_seconds"] = selfCpu(); value["service_cpu_seconds"] = processCpu(childPid); memoryTrace.append(value);
        });
        heartbeat.start(); script.start(); resources.start();
        auto* start = find<QPushButton>(*page, "captureStart"); require(start->isEnabled(), "Actual start action disabled"); start->click();
        const bool finished = waitFor([&] { return !terminal.isEmpty() && !window.isAnalyzing(); }, 60000);
        heartbeat.stop(); script.stop(); resources.stop();
        report["preview_frames"] = double(frames); report["preview_source_errors"] = sourceErrors; report["preview_validation_errors"] = previewErrors;
        report["first_preview_elapsed_ns"] = double(firstNs);
        report["screenshot_intervals"] = screenshotIntervals;
        report["observed_preview_records_skipped"] = double(skipped); report["preview_frame_intervals"] = distribution(frameIntervals);
        report["gui_heartbeat_all_live"] = distribution(heartbeatAll); report["gui_heartbeat_default_steady"] = distribution(heartbeatWave); report["gui_heartbeat_merged_steady"] = distribution(heartbeatMerged);
        report["renderer_static_default_steady"] = distribution(rasterWave); report["renderer_static_merged_steady"] = distribution(rasterMerged);
        QJsonObject defaultStages, mergedStages; const char* stageNames[]{"bounds", "m4", "map", "raster", "axes_and_background", "setup_outside_static", "other", "widget_blit_outside_static"};
        for (std::size_t stage = 0; stage < plotStagesWave.size(); ++stage) { defaultStages[stageNames[stage]] = distribution(plotStagesWave[stage]); mergedStages[stageNames[stage]] = distribution(plotStagesMerged[stage]); }
        report["renderer_parts_default_steady"] = defaultStages; report["renderer_parts_merged_steady"] = mergedStages;
        QJsonArray plotBuilds;
        for (int i = 0; i < canvases.size(); ++i) plotBuilds.append(QJsonObject{{"plot", canvases[i]->objectName()}, {"axes_mode", canvases[i]->axesMode()},
            {"direct_paint", canvases[i]->directPaintEnabled()},
            {"default_builds", double(waveBuilds[i])}, {"merged_builds", double(mergedBuilds[i])}, {"default_m4_calls", double(waveM4Builds[i])}, {"merged_m4_calls", double(mergedM4Builds[i])},
            {"default_axes_builds", double(waveAxesBuilds[i])}, {"default_axes_cache_hits", double(waveAxesHits[i])},
            {"merged_axes_builds", double(mergedAxesBuilds[i])}, {"merged_axes_cache_hits", double(mergedAxesHits[i])},
            {"default_direct_paints", double(waveDirectPaints[i])}, {"merged_direct_paints", double(mergedDirectPaints[i])},
            {"default_cached_image_blits", double(waveImageBlits[i])}, {"merged_cached_image_blits", double(mergedImageBlits[i])}});
        report["steady_plot_build_counts"] = plotBuilds;
        report["profile_scope"] = stageProfiling ?
            "AX7_CAPTURE_PROFILE=1; original 10s operation schedule unchanged. Model/analysis lifetime nested wall timings use bounded last-256 percentiles. Renderer parts sum latest changed canvases per heartbeat; setup and widget blit are outside static timing. Worker queue-age sorting is bounded to 256 observations and may run in live profile callbacks. GUI-process CPU includes its worker and FFT threads." :
            "AX7_CAPTURE_PROFILE=0 explicit diagnostic control. Original operation schedule, adoption/heartbeat/CPU measurements, source validation and file checks remain active. Optional model/transport and fine renderer timing plus live profile forwarding are disabled. Worker structural counters, queue-age/main-drain accounting and coarse renderer timing remain active; profile snapshots may still sort age samples. Zero or absent fine-stage values do not mean zero cost or latency. GUI-process CPU includes its worker and FFT threads.";
        report["default_data_adoption_hz"] = waveEnd > waveStart ? (waveRefreshEnd - waveRefreshStart) * 1e9 / (waveEnd - waveStart) : 0;
        report["merged_data_adoption_hz"] = mergedEnd > mergedStart ? (mergedRefreshEnd - mergedRefreshStart) * 1e9 / (mergedEnd - mergedStart) : 0;
        report["qt_data_refreshes"] = double(maximumRefreshes); report["terminal"] = terminal;
        const double wall = elapsed.nsecsElapsed() / 1e9, uiCpu = selfCpu() - cpuStart, serviceCpu = processCpu(childPid) - childCpuStart;
        report["ui_process_cpu_seconds"] = uiCpu; report["service_process_cpu_seconds"] = serviceCpu;
        report["ui_cpu_percent_one_core_basis"] = wall > 0 ? uiCpu / wall * 100 : 0;
        report["service_cpu_percent_one_core_basis"] = wall > 0 ? serviceCpu / wall * 100 : 0;
        report["wall_seconds"] = wall; report["memory_samples"] = memoryTrace; report["memory_final"] = memory();
        report["shots"] = QJsonObject{{"default", initialShot}, {"scroll", scrolled}, {"merge_persists", mergeShot}, {"spectrum", frequencyShot}, {"speed", speedShot}};
        require(finished && terminal.value("state") == "completed", "Actual finite hardware capture did not complete");
        require(frames > 0 && sourceErrors.isEmpty() && previewErrors.isEmpty(), "Actual hardware preview missing or source/validation failed");
        const QString path = terminal.value("output_path").toString(); const auto result = load_capture(path.toUtf8().toStdString());
        require(result.integrity_verified && result.records.size() == 500000 && result.header.sample_rate_hz == 50000 && result.header.pulses_per_rev == 1 && result.source_metadata.synthetic,
            "Final actual hardware-self-test BIN failed independent loader/count/PPR/source checks");
        report["file"] = path; report["records"] = double(result.records.size()); report["file_sha256"] = sha256(path);
        require(initialShot && scrolled && mergeShot && frequencyShot && speedShot, "A required live view/scroll/merge observation was not completed during acquisition");
        bool allDeferredSaved = true; double deferredEncodeMs = 0;
        for (auto& image : deferredShots) {
            const auto begin = elapsed.nsecsElapsed(); const bool saved = image.image.save(output + '/' + image.name + ".png"); const auto end = elapsed.nsecsElapsed();
            auto interval = screenshotIntervals[image.intervalIndex].toObject(); interval["saved"] = saved;
            interval["encode_begin_elapsed_ns"] = double(begin); interval["encode_end_elapsed_ns"] = double(end); interval["encode_ms"] = (end - begin) / 1e6;
            screenshotIntervals[image.intervalIndex] = interval; deferredEncodeMs += (end - begin) / 1e6; allDeferredSaved = allDeferredSaved && saved;
            image.image = {};
        }
        report["screenshot_intervals"] = screenshotIntervals; report["post_capture_png_encode_ms"] = deferredEncodeMs;
        require(allDeferredSaved, "Deferred PNG output failed after capture");
        report["plot_shared_m4_requested"]=QString::fromLatin1(qgetenv("AX7_PLOT_SHARED_M4"));
        report["plot_shared_m4_effective"]=!canvases.isEmpty()&&canvases.first()->sharedM4Enabled();
        if(!canvases.isEmpty())report["plot_shared_m4_profile_global_once"]=canvases.first()->sharedM4Profile();
        report["plot_shared_m4_counter_scope"]="One GUI-process global snapshot from first canvas; never summed across canvases. Per-canvas rebuild counters remain in steady_plot_build_counts.";
        report["lifecycle_requested"]=qgetenv("AX7_BOARD_LIFECYCLE")=="1";
        if(qgetenv("AX7_BOARD_LIFECYCLE")=="1"){
            QObject::disconnect(originalFrames);QObject::disconnect(originalStates);finalAnalysisProfile.freeze();
            report["transport_profile_performance_phase"]=client->transportProfile();*lifecycleStarted=true;
            report["run_kind"]="lifecycle_assertions_with_original10s_evidence_not_performance_comparison";
            report["lifecycle"]=lifecycle(window,*page,*client,*analysis,output,terminal.value("job_id").toString());
        }
        passed = true;
    } catch (const std::exception& error) { report["error"] = QString::fromUtf8(error.what()); }
    if (childPid > 0) {
        const bool exited = waitFor([&] { return !QFileInfo::exists(QStringLiteral("/proc/%1").arg(childPid)); }, 1500);
        report["child_process_exit_observed"] = exited;
        report["child_release_state"] = exited ? "exited" : "unknown";
        if (!exited) { passed = false; if (!report.contains("error")) report["error"] = "Service process still exists after Window scope exit; device release is unknown"; }
    } else report["child_release_state"] = "not_observed";
    report["passed"] = passed;
    report["measurement_scope"] = "Actual ARM Qt/driver hardware-self-test only. Performance is measured, not presumed to meet 25fps. Qt raster/data adoption is not physical panel scanout, touch acceptance, sensor validity or fault-classification evidence. Screenshot mode and grab/encode intervals are explicit: inspect overlap with steady windows before attributing stalls. Deferred PNG output occurs after capture/CPU measurement; QImage grabs can still briefly block the GUI.";
    if (outputValidated) { QFile file(output + "/board-live.json"); if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) file.write(QJsonDocument(report).toJson()); }
    const auto summary = QJsonDocument(QJsonObject{{"passed", passed}, {"preview_frames", double(frames)}, {"report", outputValidated ? output + "/board-live.json" : QString{}}, {"error", report.value("error")}}).toJson(QJsonDocument::Compact);
    std::fwrite(summary.constData(), 1, std::size_t(summary.size()), stdout); std::fputc('\n', stdout); return passed ? 0 : 1;
}
