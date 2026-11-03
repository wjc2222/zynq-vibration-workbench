#include "main_window.hpp"
#include "vibration/detection.hpp"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QThread>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#endif

namespace {
QJsonArray checks;
void check(bool passed, const QString& name) {
    checks.append(QJsonObject{{"check",name},{"passed",passed}});
    if (!passed) throw std::runtime_error(name.toStdString());
}
bool wait(QApplication& app, vibration::gui::MainWindow& window) {
    QElapsedTimer timer;timer.start();
    while(window.isAnalyzing()&&timer.elapsed()<30000){app.processEvents();QThread::msleep(3);}
    app.processEvents();return !window.isAnalyzing();
}
QJsonObject readJson(const QString& path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
QByteArray bytes(const QString& path) {QFile file(path);file.open(QIODevice::ReadOnly);return file.readAll();}
}
int main(int argc,char** argv) {
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
#endif
    QApplication app(argc,argv);
    QFont font;font.setFamilies({"Microsoft YaHei UI","Noto Sans CJK SC","WenQuanYi Zen Hei","sans-serif"});font.setPixelSize(14);app.setFont(font);
    if(argc!=6)return 2;
    const QString source=QString::fromUtf8(argv[1]),synthetic=QString::fromUtf8(argv[2]),bad=QString::fromUtf8(argv[3]);
    const QDir out(QString::fromUtf8(argv[4]));QDir().mkpath(out.path());
    const auto size=QString::fromUtf8(argv[5]).split('x');
    QJsonObject report;
    try {
        vibration::AnalysisConfig config;config.signal_kind=vibration::SignalKind::VibrationConfirmed;config.calibration_confirmed=true;config.spectrum_max_hz=400;
        auto baseline=vibration::make_baseline_draft(vibration::analyze_file(source.toUtf8().toStdString(),config),"software-qa-bench","example_experimental_gui");
        baseline.rules={{"example_normal","normal",{{"ac_rms",1,"absolute",0,1000000}}}};
        const auto profile=out.filePath("example-experimental.profile");
        vibration::write_detection_text(profile.toUtf8().toStdString(),vibration::detection_profile_text(baseline),{});
        const auto profileBefore=bytes(profile),sourceBefore=bytes(source),syntheticBefore=bytes(synthetic);
        const auto invalid=out.filePath("invalid.profile");{QFile file(invalid);file.open(QIODevice::WriteOnly);file.write("unknown_key=true\n");}
        vibration::gui::MainWindow window;
        if(size.size()==2)window.resize(size[0].toInt(),size[1].toInt());
        window.show();window.setPage("diagnosis");app.processEvents();
        auto* state=window.findChild<QLabel*>("detectionState");
        auto* tabs=window.findChild<QTabWidget*>("detectionTabs");
        auto* quality=window.findChild<QLabel*>("detectionQuality");
        auto* evidence=window.findChild<QTextBrowser*>("detectionEvidence");
        check(state&&tabs&&quality&&evidence,"diagnosis has detection widgets");
        check(tabs->count()>=2,"diagnosis contains detection and feature tabs");
        const auto shot=[&](const QString& name,int tab=0){window.setPage("diagnosis");tabs->setCurrentIndex(tab);app.processEvents();QThread::msleep(50);app.processEvents();check(window.grab().save(out.filePath(name+".png")),"screenshot "+name);};
        const auto exported=[&](const QString& name){const auto path=out.filePath(name+".json");QString error;check(window.saveResult(path,false,&error),"export "+name+": "+error);return readJson(path);};
        check(window.setDetectionSetupId("software-qa-bench"),"setup accepted");
        window.openCapture(source);check(wait(app,window),"initial analysis completes");
        shot("01-no-profile");
        auto json=exported("no-profile");
        report.insert("no_profile",json);
        check(window.loadDetectionProfile(profile),"valid experimental profile accepted");
        shot("02-quality-gated");
        report.insert("uncalibrated",exported("uncalibrated"));
        check(window.setDetectionMeasurementConditions(vibration::SignalKind::VibrationConfirmed,true),"explicit measurement declaration accepted");
        check(window.isAnalyzing(),"declaration starts reanalysis");
        QString busyError;
        check(!window.loadDetectionProfile(invalid,&busyError),"busy profile mutation rejected");
        check(!window.setDetectionSetupId("busy-wrong-setup",&busyError),"busy setup mutation rejected");
        check(!window.setDetectionMeasurementConditions(vibration::SignalKind::Unknown,false,&busyError),"busy measurement mutation rejected");
        check(!window.saveResult(out.filePath("busy-result.json"),false,&busyError),"busy export rejected");
        check(!window.saveDetectionBaselineDraft(out.filePath("busy.profile"),"example_busy",&busyError),"busy baseline rejected");
        check(wait(app,window),"declaration reanalysis completes");
        json=exported("candidate");report.insert("candidate",json);shot("03-candidate");
        check(json.value("detection").toObject().value("profile_snapshot").toString().toUtf8()==profileBefore,"export embeds full canonical profile snapshot");
        const auto csv=out.filePath("candidate.csv");QString csvError;
        check(window.saveResult(csv,true,&csvError),"candidate CSV export: "+csvError);
        report.insert("candidate_csv",QString::fromUtf8(bytes(csv)));
        shot("04-features",1);tabs->setCurrentIndex(0);
        auto zeroProfile=baseline;
        for(auto& feature:zeroProfile.baseline)if(feature.channel==1&&feature.feature=="ac_rms")feature.value=0;
        zeroProfile.rules.push_back({"example_unavailable_ratio","imbalance",{{"ac_rms",1,"ratio",0,1000000}}});
        const auto zeroPath=out.filePath("example-experimental-zero-ratio.profile");
        vibration::write_detection_text(zeroPath.toUtf8().toStdString(),vibration::detection_profile_text(zeroProfile),{});
        check(window.loadDetectionProfile(zeroPath),"zero ratio profile structurally accepted");
        report.insert("zero_baseline_conflict",exported("zero-baseline-conflict"));
        check(window.loadDetectionProfile(profile),"profile restored after zero ratio gate");
        QString error;
        const auto draft=out.filePath("observed-single-record-draft.profile");
        check(window.saveDetectionBaselineDraft(draft,"example_observed_only",&error),"baseline snapshot saved: "+error);
        const auto parsed=vibration::load_detection_profile(draft.toUtf8().toStdString());
        check(parsed.rules.empty()&&parsed.status=="draft","baseline has no rules or normal label");
        for (const auto& protectedPath:{source,source.left(source.size()-4)+".meta.json",profile}){
            check(!window.saveResult(protectedPath,false,&error),"protected JSON rejected: "+protectedPath);
            check(!window.saveResult(protectedPath,true,&error),"protected CSV rejected: "+protectedPath);
            check(!window.saveDetectionBaselineDraft(protectedPath,"example_protected",&error),"protected baseline rejected: "+protectedPath);
        }
        check(window.setDetectionSetupId("different-setup"),"setup change accepted");
        report.insert("incompatible_setup",exported("incompatible-setup"));
        check(window.setDetectionSetupId("software-qa-bench"),"setup restored");
        check(!window.loadDetectionProfile(invalid,&error)&&!error.isEmpty(),"bad profile rejected with explanation");
        shot("05-invalid-profile");report.insert("invalid_profile",exported("invalid-profile"));
        check(window.loadDetectionProfile(profile),"profile restored after parse failure");
        window.openCapture(synthetic);check(wait(app,window),"synthetic analysis completes");
        check(!window.setDetectionMeasurementConditions(vibration::SignalKind::VibrationConfirmed,true,&error),"synthetic promotion rejected");
        check(wait(app,window),"synthetic declaration analysis completes");
        shot("06-synthetic-gated");report.insert("synthetic",exported("synthetic"));
        window.openCapture(bad);check(wait(app,window),"invalid capture analysis completes");
        shot("07-invalid-capture");
        check(!window.saveResult(out.filePath("invalid-capture.json"),false,&error),"invalid capture cannot export stale result");
        check(!window.saveDetectionBaselineDraft(out.filePath("invalid-capture.profile"),"example_invalid",&error),"invalid capture cannot draft stale baseline");
        window.clearDetectionProfile();
        window.openCapture(source);check(wait(app,window),"reload completes");
        report.insert("cleared_profile",exported("cleared-profile"));
        check(bytes(source)==sourceBefore&&bytes(synthetic)==syntheticBefore&&bytes(profile)==profileBefore,"protected inputs unchanged");
        check(window.width()==size[0].toInt()&&window.height()==size[1].toInt(),"native requested window dimensions");
        report.insert("detection_state_text",state->text());
        report.insert("result","passed");
    } catch(const std::exception& error){report.insert("result","failed");report.insert("error",QString::fromUtf8(error.what()));}
    report.insert("checks",checks);
    const auto output=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(output.constData(),1,static_cast<std::size_t>(output.size()),stdout);
    return report.value("result")=="passed"?0:1;
}
