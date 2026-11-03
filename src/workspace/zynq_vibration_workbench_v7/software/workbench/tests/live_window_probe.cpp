#include "window.hpp"
#include "analysis_pane.hpp"
#include "capture_page.hpp"
#include "plot_canvas.hpp"
#include "waveform_stack.hpp"
#include "vibration/core.hpp"
#include "test_fonts.hpp"
#include "ui_interaction.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextCodec>
#include <QTextLayout>
#include <QThread>
#include <QVariant>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace vibration;
using namespace vibration::workbench;
namespace {
QJsonArray checks, transitions;
QString scenario;
void check(bool passed, const QString& name) {
    checks.push_back(QJsonObject{{"scenario", scenario}, {"check", name}, {"passed", passed}});
    if (!passed) throw std::runtime_error(name.toStdString());
}
template<class T> T* find(QWidget& owner, const char* name) {
    auto* object = dynamic_cast<T*>(owner.findChild<QWidget*>(name));
    check(object != nullptr, QStringLiteral("control: ") + name); return object;
}
void events(int milliseconds = 30) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < milliseconds) { QApplication::processEvents(); QThread::msleep(1); }
}
bool until(const std::function<bool()>& condition, int milliseconds = 5000) {
    QElapsedTimer timer; timer.start();
    while (!condition() && timer.elapsed() < milliseconds) events(2);
    QApplication::processEvents(); return condition();
}
void click(QPushButton& button) {
    check(button.isVisible() && button.isEnabled(), "visible enabled action: " + button.objectName());
    const QPointF position = button.rect().center();
    QMouseEvent down(QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&button, &down); QApplication::sendEvent(&button, &up);
}
void put(QByteArray& out, int offset, quint64 value, int size) {
    for (int i = 0; i < size; ++i) out[offset + i] = char((value >> (i * 8)) & 255);
}
QJsonObject frame(const Capture& capture, const QString& job, int first, int sequence, const QString& generation = "1") {
    const int count = 2000; QByteArray bytes(count * 32, 0);
    for (int i = 0; i < count; ++i) {
        const auto& r = capture.records.at(std::size_t(first + i)); const int offset = i * 32;
        put(bytes, offset, r.index, 8); put(bytes, offset + 8, r.tick, 8);
        for (int channel = 0; channel < 3; ++channel) put(bytes, offset + 16 + channel * 2, quint16(r.raw[channel]), 2);
        put(bytes, offset + 22, r.flags, 2); put(bytes, offset + 24, r.speed_period_ticks, 4); put(bytes, offset + 28, r.speed_edge_count, 4);
    }
    return QJsonObject{{"schema", "vibration.capture.v2"}, {"type", "event"}, {"event", "preview"}, {"job_id", job},
        {"generation", generation}, {"frame_seq", sequence}, {"first_record", first}, {"record_count", count},
        {"committed_records", first + count}, {"requested_records", double(capture.records.size())},
        {"sample_rate_hz", int(capture.header.sample_rate_hz)}, {"clock_hz", 50000000}, {"ppr", 1}, {"adc_range_uv", 5000000},
        {"channel_map", 328449}, {"record_bytes", 32}, {"source", "test_fixture"}, {"hardware_confirmed", false}, {"synthetic", true},
        {"source_flags", 1}, {"validity_flags", 27}, {"raw_base64", QString::fromLatin1(bytes.toBase64())},
        {"crc32", double(crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size())))}, {"dropped_preview_records", 0}};
}
QJsonObject state(const QString& job, const QString& phase, const QString& path = {}, bool lost = false) {
    return QJsonObject{{"job_id", job}, {"state", phase}, {"records_completed", phase == "completed" ? 500000 : 20000},
        {"requested_records", 500000}, {"synthetic", true}, {"hardware_confirmed", false}, {"output_path", path},
        {"connection_lost", lost}, {"driver_stop_confirmed", false}};
}
QString hash(const QString& path) {
    QFile file(path); check(file.open(QIODevice::ReadOnly), "read hash input");
    return QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
}
void snapshot(Window& window, const QString& output, const QString& name) {
    events(40); check(window.grab().save(output + '/' + name + ".png"), "save actual Window: " + name);
}
void views(Window& window, const QString& output, const QString& prefix) {
    for (int view = 0; view < 3; ++view) {
        window.setView(view); events(view == 1 ? 220 : 50);
        snapshot(window, output, prefix + QStringList{"-time", "-spectrum", "-speed"}[view]);
    }
    window.setView(0);
}
void setupService(Window& window, CapturePage& page, const QString& service, const QString& directory,
                  int duration, const QStringList& fault = {}) {
    page.setServiceCommand(service, QStringList{"--test-fixture"} + fault); window.showAcquisition();
    check(until([&] { return page.property("captureAvailable").toBool(); }), "explicit host fixture capability ready");
    find<QComboBox>(page, "captureRate")->setCurrentIndex(0);
    find<QDoubleSpinBox>(page, "captureDuration")->setValue(duration);
    find<QDoubleSpinBox>(page, "capturePpr")->setValue(1);
    find<QLineEdit>(page, "captureDirectory")->setText(directory);
}
}
int main(int argc, char** argv) {
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8")); QApplication app(argc, argv);
    qa::configureFonts(app);
    QJsonObject report; bool passed = false; QString rootOutput;
    try {
        check(argc == 4, "fixture, service, evidence arguments");
        const QString fixture = QString::fromUtf8(argv[1]), service = QString::fromUtf8(argv[2]); rootOutput = QString::fromUtf8(argv[3]);
        check(QDir().mkpath(rootOutput), "evidence directory"); const auto before = hash(fixture); const auto capture = load_capture(fixture.toUtf8().toStdString());
        check(capture.source_metadata.synthetic && capture.records.size() >= 26000, "normally loaded explicit synthetic fixture");
        report["font_audit"] = qa::fontAudit(app.font()); report["font_families"] = report["font_audit"].toObject().value("families");
        for (const auto size : {QSize(1024, 600), QSize(1280, 720)}) {
            const QString output = rootOutput + QStringLiteral("/%1x%2").arg(size.width()).arg(size.height()); check(QDir().mkpath(output), "size evidence directory");
            QTemporaryDir captureOutput; check(captureOutput.isValid(), "private host fixture output directory");
            Window window; window.resize(size); window.show(); events();
            auto* page = find<CapturePage>(window, "capturePage"); auto* analysis = find<AnalysisPane>(window, "analysisPane");
            page->setStreaming(false);
            auto* stateLabel = find<QLabel>(window, "documentState"); auto* title = find<QLabel>(window, "documentTitle");
            auto* cancel = find<QPushButton>(window, "workbenchStart"); auto* detection = find<QLabel>(window, "detectionState");
            QObject::connect(page, &CapturePage::captureState, &window, [&](const QJsonObject& value) {
                transitions.push_back(QJsonObject{{"scenario", scenario}, {"state", value.value("state")}, {"connection_lost", value.value("connection_lost")},
                    {"synthetic", value.value("synthetic")}, {"output_path", value.value("output_path")}});
            });
            scenario = QStringLiteral("signal-handoff-%1").arg(size.width());
            window.openCapture(fixture); check(until([&] { return !window.isAnalyzing(); }), "offline source analysis completes");
            detection->setText(QStringLiteral("QA_STALE_RESULT_MUST_CLEAR"));
            const QString job = "signal_job_" + QString::number(size.width());
            page->captureState(state(job, "preparing"));
            check(detection->text() != "QA_STALE_RESULT_MUST_CLEAR", "preparing before first frame clears stale detection");
            check(!find<QPushButton>(window, "viewTab3")->isEnabled(), "detection blocked before any preview frame");
            page->captureState(state(job, "running")); page->liveFrame(frame(capture, job, 0, 1)); events(80);
            check(analysis->isLive() && title->toolTip() == QStringLiteral("实时采集"), "first validated signal switches actual Window to live");
            check(!find<QPushButton>(window, "workbenchSettings")->isEnabled() && !find<QPushButton>(window, "openRecord")->isEnabled() &&
                !find<QListWidget>(window, "workbenchRecords")->isEnabled(), "active settings/open/records locked");
            check(cancel->isVisible() && cancel->isEnabled() && cancel->text() == QStringLiteral("取消采集") && !find<QPushButton>(window, "workbenchExport")->isVisible(), "live cancel explicitly labelled and file export hidden");
            for (int i = 1; i < 10; ++i) { page->liveFrame(frame(capture, job, i * 2000, i + 1)); events(42); }
            views(window, output, "signal-live");
            window.setView(0); auto* follow = find<QPushButton>(window, "liveFollow"); auto* stack = find<WaveformStack>(window, "waveformStack");
            click(*follow); const auto frozen = stack->plot(0)->series()[0].points;
            page->liveFrame(frame(capture, job, 20000, 11)); events(80);
            check(analysis->isLive() && stack->plot(0)->series()[0].points == frozen, "Window pause freezes plot while raw ingestion remains live");
            click(*follow); events(80); check(stack->plot(0)->series()[0].points != frozen, "Window follow resumes fresh validated points");
            click(*cancel); page->liveFrame(frame(capture, job, 22000, 12)); events(50);
            check(!cancel->isEnabled() && analysis->isLive() && stateLabel->text() == QStringLiteral("正在取消"), "cancel request remains pending despite another live frame");
            page->captureState(state(job, "cancelled")); events();
            check(!analysis->isLive() && !cancel->isVisible() && stateLabel->text().contains(QStringLiteral("已取消")), "only terminal cancel stops preview state");
            const auto stopped = stack->plot(0)->series()[0].points; page->liveFrame(frame(capture, job, 24000, 13)); events(60);
            check(!analysis->isLive() && stack->plot(0)->series()[0].points == stopped, "late preview after terminal state is ignored");
            const QString lostJob = job + "_lost"; page->captureState(state(lostJob, "preparing")); page->liveFrame(frame(capture, lostJob, 0, 1, "2")); events(60);
            page->captureState(state(lostJob, "failed", {}, true)); events();
            check(!analysis->isLive() && stateLabel->text().contains(QStringLiteral("状态未知")) && !stateLabel->text().contains(QStringLiteral("已停止")), "connection loss does not claim driver stopped");
            snapshot(window, output, "signal-connection-lost");
            const QString failedJob = job + "_failed"; page->captureState(state(failedJob, "preparing")); page->liveFrame(frame(capture, failedJob, 0, 1, "3")); events(60);
            page->captureState(state(failedJob, "failed")); events(); check(stateLabel->text() == QStringLiteral("采集失败") && !analysis->isLive(), "explicit failure is not a saved record");
            const QString completedJob = job + "_completed"; page->captureState(state(completedJob, "preparing")); page->liveFrame(frame(capture, completedJob, 0, 1, "4")); events(60);
            page->captureState(state(completedJob, "completed", fixture));
            check(until([&] { return !window.isAnalyzing() && title->toolTip() == QFileInfo(fixture).fileName(); }), "completed signal opens and normally validates the full BIN");
            check(!analysis->isLive() && find<QPushButton>(window, "workbenchExport")->isEnabled() && stateLabel->text() == QStringLiteral("合成演示"), "file analysis replaces live V preview after completion");

            scenario = QStringLiteral("actual-fixture-chain-%1").arg(size.width());
            setupService(window, *page, service, captureOutput.path(), 3); snapshot(window, output, "capture-page-ready");
            QJsonObject lastTerminal; int nativeFrames = 0;
            QObject::connect(page, &CapturePage::liveFrame, &window, [&](const QJsonObject&) { ++nativeFrames; });
            QObject::connect(page, &CapturePage::captureState, &window, [&](const QJsonObject& value) {
                if (QStringList{"completed", "failed", "cancelled"}.contains(value.value("state").toString())) lastTerminal = value;
            });
            click(*find<QPushButton>(*page, "captureStart"));
            check(until([&] { return analysis->isLive(); }), "actual child raw preview reaches Window through CaptureClient and CapturePage");
            check(stateLabel->text().contains(QStringLiteral("合成")), "actual fixture live source remains explicitly synthetic");
            check(until([&] { return analysis->property("liveAcceptedFrames").toULongLong() >= 8; }), "actual rolling preview fills its quarter-second window");
            views(window, output, "service-live");
            check(until([&] { return lastTerminal.value("state") == "completed" && !window.isAnalyzing() && !analysis->isLive(); }, 10000), "actual service completion automatically hands off to file analysis");
            const QString saved = lastTerminal.value("output_path").toString(); const auto actual = load_capture(saved.toUtf8().toStdString());
            check(actual.integrity_verified && actual.source_metadata.synthetic && actual.records.size() == 150000 && nativeFrames > 5,
                  "actual saved synthetic BIN passes independent loader and source checks");
            const QString archive = output + "/completed-synthetic.BIN", meta = QFileInfo(saved).absolutePath() + '/' + QFileInfo(saved).completeBaseName() + ".meta.json";
            check(QFile::copy(saved, archive) && QFile::copy(meta, output + "/completed-synthetic.meta.json") && hash(saved) == hash(archive), "preserve hashed service output and synthetic sidecar");
            snapshot(window, output, "service-completed-file");
            lastTerminal = {}; setupService(window, *page, service, captureOutput.path(), 4);
            click(*find<QPushButton>(*page, "captureStart")); check(until([&] { return analysis->isLive(); }), "next actual service job becomes live");
            click(*cancel); check(analysis->isLive(), "actual cancel request alone does not assert stopped");
            check(until([&] { return lastTerminal.value("state") == "cancelled" && !analysis->isLive(); }), "actual service terminal cancelled stops preview");
            lastTerminal = {}; setupService(window, *page, service, captureOutput.path(), 4);
            click(*find<QPushButton>(*page, "captureStart")); check(until([&] { return analysis->isLive(); }), "actual connection-loss fixture is active");
            auto* process = page->findChild<QProcess*>();
            check(process && process->program() == service && process->arguments().contains("--test-fixture"), "only this explicitly synthetic child may be terminated");
            process->kill();
            check(until([&] { return !analysis->isLive() && stateLabel->text().contains(QStringLiteral("状态未知")); }), "actual child disconnect leaves driver state unknown in Window");
            snapshot(window, output, "service-connection-lost");
        }
        check(hash(fixture) == before, "original synthetic source file remains unchanged"); passed = true;
    } catch (const std::exception& error) { report["error"] = QString::fromUtf8(error.what()); }
    report["checks"] = checks; report["transitions"] = transitions; report["passed"] = passed;
    report["scope"] = "Whole new Window handoff signals plus real child --test-fixture transport and normal file loader. No board, driver, ADC sensor, physical display or fault-validation acceptance.";
    const auto json = QJsonDocument(report).toJson();
    if (!rootOutput.isEmpty()) { QFile file(rootOutput + "/live-window.json"); if (file.open(QIODevice::WriteOnly)) file.write(json); }
    std::fwrite(json.constData(), 1, std::size_t(json.size()), stdout); return passed ? 0 : 1;
}
