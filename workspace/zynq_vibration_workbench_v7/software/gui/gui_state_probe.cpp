#include "main_window.hpp"
#include "plot_widget.hpp"
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QStackedWidget>
#include <QDir>
#include <filesystem>

#include <QApplication>
#include <QElapsedTimer>
#include <QDialog>
#include <QCommandLineParser>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QPixmap>
#include <QTextBrowser>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QMouseEvent>
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#endif
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace {
using WavePlots = std::array<vibration::gui::PlotWidget*, 4>;
WavePlots wavePlots(vibration::gui::MainWindow& window) {
    return {{window.findChild<vibration::gui::PlotWidget*>("waveCH1Plot"),
             window.findChild<vibration::gui::PlotWidget*>("waveCH2Plot"),
             window.findChild<vibration::gui::PlotWidget*>("waveCH3Plot"),
             window.findChild<vibration::gui::PlotWidget*>("waveformPlot")}};
}
bool cleared(const WavePlots& plots) {
    return std::all_of(plots.begin(), plots.end(), [](const auto* plot) {
        return plot && plot->series().empty() && !plot->cursorX() && !plot->explicitXRange();
    });
}
bool sharedRange(const WavePlots& plots, double start, double end) {
    return std::all_of(plots.begin(), plots.end(), [=](const auto* plot) {
        const auto range = plot->explicitXRange();
        return range && std::abs(range->x()-start)<1e-10 && std::abs(range->y()-end)<1e-10;
    });
}
bool sameCursor(const WavePlots& plots, double time) {
    return std::all_of(plots.begin(), plots.end(), [=](const auto* plot) {
        return plot->cursorX() && std::abs(*plot->cursorX()-time)<1e-12;
    });
}
QJsonArray plotGeometry(const WavePlots& plots, QWidget& window) {
    QJsonArray result;
    for (const auto* plot : plots) {
        const auto origin=plot->mapTo(&window,QPoint());
        result.append(QJsonObject{{"name",plot->objectName()},{"x",origin.x()},{"y",origin.y()},
            {"width",plot->width()},{"height",plot->height()}, {"visible",plot->isVisible()},
            {"plot_area_width",plot->plotArea().width()},{"plot_area_height",plot->plotArea().height()},
            {"x_axis_visible",plot->xAxisVisible()}});
    }
    return result;
}
}

// Exercise repeated loads through the real asynchronous GUI, including a bad
// file following a good file. The Python driver owns temporary fixture files.
int main(int argc, char** argv) {
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Match the production entry point's UTF-8 Linux capture-path contract.
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
#endif
    QApplication app(argc, argv);
    QFont font;
    font.setFamilies({"Microsoft YaHei UI", "Microsoft YaHei", "Noto Sans CJK SC", "WenQuanYi Micro Hei", "WenQuanYi Zen Hei", "sans-serif"});
    font.setPixelSize(14);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferVerticalHinting);
    app.setFont(font);
    QCommandLineParser parser;
    QCommandLineOption ppr("ppr", "Explicit pulses per revolution", "number");
    parser.addOption(ppr);
    QCommandLineOption directory("directory", "Preselect directories without opening files", "path");
    parser.addOption(directory);
    parser.addPositionalArgument("capture", "Capture files", "[capture...]");
    parser.process(app);
    vibration::AnalysisConfig initialConfig;
    if (parser.isSet(ppr)) {
        bool valid = false;
        const auto value = parser.value(ppr).toDouble(&valid);
        if (!valid || !std::isfinite(value) || value < 1e-6 || value > 1e9) return 2;
        initialConfig.ppr_override = value;
    }
    vibration::gui::MainWindow window(initialConfig);
    const auto testSize=qEnvironmentVariable("VIBRATION_GUI_TEST_SIZE").split('x');
    if(testSize.size()==2) window.resize(testSize[0].toInt(),testSize[1].toInt());
    window.show();
    window.setPage("waveform");app.processEvents();
    const auto plots=wavePlots(window);
    if(std::any_of(plots.begin(),plots.end(),[](const auto* plot){return !plot;}))return 4;
    const bool initialCleared=cleared(plots);
    const auto screenshotDirectory=qEnvironmentVariable("VIBRATION_GUI_FOUR_SCREENSHOT_DIR");
    const auto screenshot=[&](const QString& name){
        if(screenshotDirectory.isEmpty())return;
        window.setPage("waveform");app.processEvents();
        window.grab().save(QDir(screenshotDirectory).filePath(name+".png"),"PNG");
    };
    screenshot("empty");window.setPage("files");app.processEvents();
    QJsonArray states;
    for (const auto& path : parser.values(directory)) {
        window.setDataDirectory(path);
        app.processEvents();
        QJsonObject state;
        state.insert("directory", path);
        state.insert("directory_label", window.findChild<QLabel*>("directoryLabel")->text());
        QJsonArray listedPaths, listedText;
        auto* files = window.findChild<QListWidget*>("captureFiles");
        for (int i = 0; i < files->count(); ++i) {
            listedText.append(files->item(i)->text());
            const auto listed = files->item(i)->data(Qt::UserRole).toString();
            if (!listed.isEmpty()) listedPaths.append(listed);
        }
        state.insert("listed_paths", listedPaths);
        state.insert("listed_text", listedText);
        state.insert("is_analyzing", window.isAnalyzing());
        state.insert("banner", window.findChild<QLabel*>("analysisBanner")->text());
        state.insert("export_enabled", window.findChild<QPushButton*>("exportResult")->isEnabled());
        state.insert("demo_enabled", window.findChild<QPushButton*>("openDemo")->isEnabled());
        states.append(state);
    }
    bool first = true;
    for (const auto& path : parser.positionalArguments()) {
        window.openCapture(path);
        const bool clearedWhileLoading=cleared(plots);
        QElapsedTimer elapsed;
        elapsed.start();
        int eventLoopPumps = 0;
        while (window.isAnalyzing() && elapsed.elapsed() < 30000) {
            app.processEvents();
            ++eventLoopPumps;
            QThread::msleep(5);
        }
        app.processEvents();
        if (window.isAnalyzing()) return 3;
        QJsonObject state;
        state.insert("path", path);
        state.insert("four_plots_initially_cleared",initialCleared);
        state.insert("four_plots_cleared_while_loading",clearedWhileLoading);
        state.insert("four_plots_cleared_after_error",!window.findChild<QPushButton*>("exportResult")->isEnabled()&&cleared(plots));
        state.insert("file_name", window.findChild<QLabel*>("currentFile")->text());
        state.insert("speed_stats", window.findChild<QLabel*>("speedStats")->text());
        state.insert("process_lc_all", qEnvironmentVariable("LC_ALL"));
        state.insert("process_lang", qEnvironmentVariable("LANG"));
        state.insert("analysis_elapsed_ms", elapsed.elapsed());
        state.insert("initial_full_preview_build_ns", window.property("wavePreviewLastNs").toLongLong());
        state.insert("event_loop_pumps", eventLoopPumps);
        state.insert("banner", window.findChild<QLabel*>("analysisBanner")->text());
        QTextDocument metrics;
        metrics.setHtml(window.findChild<QLabel*>("analysisMetrics")->text());
        state.insert("metrics", metrics.toPlainText());
        QTextDocument features;
        features.setHtml(window.findChild<QLabel*>("analysisFeatures")->text());
        state.insert("features", features.toPlainText());
        state.insert("sample_info", window.findChild<QLabel*>("sampleInfo")->text());
        state.insert("calibration_visible", window.findChild<QLabel*>("calibrationState")->isVisible());
        state.insert("diagnosis", window.findChild<QLabel*>("analysisDiagnosis")->text());
        state.insert("evidence", window.findChild<QTextBrowser*>("analysisEvidence")->toPlainText());
        state.insert("export_enabled", window.findChild<QPushButton*>("exportResult")->isEnabled());
        state.insert("width", window.width());
        state.insert("height", window.height());
        int minimumButtonHeight=1000;
        bool navigationWorks=true;
        const QStringList pageNames{"files","waveform","spectrum","speed","diagnosis"};
        for(int i=0;i<5;++i){navigationWorks=window.setPage(pageNames[i])&&navigationWorks;app.processEvents();navigationWorks=navigationWorks&&window.findChild<QStackedWidget*>("workspacePages")->currentIndex()==i;for(const auto* button:window.findChildren<QPushButton*>())if(button->isVisible())minimumButtonHeight=std::min(minimumButtonHeight,button->height());}
        window.setPage("files");app.processEvents();
        state.insert("navigation_works",navigationWorks);state.insert("minimum_button_height",minimumButtonHeight);
        if(window.findChild<QPushButton*>("exportResult")->isEnabled()) {
            QTemporaryDir temporary;
            const auto jsonPath=temporary.filePath("analysis.json"), csvPath=temporary.filePath("summary.csv");
            QString error;
            const bool jsonOk=window.saveResult(jsonPath,false,&error),csvOk=window.saveResult(csvPath,true,&error);
            QFile jf(jsonPath);jf.open(QIODevice::ReadOnly);const auto analysis=QJsonDocument::fromJson(jf.readAll()).object();
            QFile cf(csvPath);cf.open(QIODevice::ReadOnly);state.insert("csv",QString::fromUtf8(cf.readAll()));
            state.insert("exports_valid",jsonOk&&csvOk&&analysis.contains("gui_source_metadata")&&analysis.value("diagnosis").toObject().value("classification")=="not_classified");
            const QFileInfo input(path);const auto sidecar=input.absolutePath()+'/'+input.completeBaseName()+".meta.json";
            state.insert("protected_exports_rejected",!window.saveResult(path,false,&error)&&!window.saveResult(sidecar,true,&error));
            const auto link=temporary.filePath("source-link.json");QFile::link(path,link);state.insert("symlink_export_rejected",!window.saveResult(link,false,&error));
            const auto hardlink=temporary.filePath("source-hardlink.json");std::error_code linkError;
            std::filesystem::create_hard_link(std::filesystem::u8path(path.toUtf8().toStdString()),std::filesystem::u8path(hardlink.toUtf8().toStdString()),linkError);
            state.insert("hardlink_export_rejected",linkError ? QJsonValue(QJsonValue::Null) : QJsonValue(!window.saveResult(hardlink,true,&error)));
            state.insert("hardlink_error_code",linkError.value());state.insert("hardlink_error",QString::fromStdString(linkError.message()));
            window.setPage("spectrum");app.processEvents();
            auto* spectrum=window.findChild<vibration::gui::PlotWidget*>("spectrumPlot");spectrum->selectAtX(33.21);app.processEvents();
            const auto frequencies=analysis.value("spectra").toArray()[0].toObject().value("frequency_hz").toArray();
            double nearest=frequencies[0].toDouble();for(const auto& value:frequencies)if(std::abs(value.toDouble()-33.21)<std::abs(nearest-33.21))nearest=value.toDouble();
            state.insert("fft_cursor_exact",window.findChild<QLabel*>("frequencyCursor")->text().startsWith(QString::number(nearest,'f',3)+" Hz"));
            const auto visible=spectrum->grab().toImage();auto* ch=window.findChild<QCheckBox*>("spectrumChannel1");ch->click();app.processEvents();const auto hidden=spectrum->grab().toImage();ch->click();app.processEvents();state.insert("channel_toggle_changes_plot",visible!=hidden&&visible==spectrum->grab().toImage());
            window.setPage("waveform");app.processEvents();
            auto* start=window.findChild<QDoubleSpinBox*>("rangeStart");auto* end=window.findChild<QDoubleSpinBox*>("rangeEnd");
            state.insert("four_plot_geometry",plotGeometry(plots,window));
            bool correctSeries=plots[3]->series().size()==3;
            for(int axis=0;axis<3;++axis)correctSeries=correctSeries&&plots[axis]->series().size()==1&&plots[axis]->series()[0].name.startsWith(QString("CH%1").arg(axis+1))&&plots[axis]->series()[0].points==plots[3]->series()[axis].points&&plots[axis]->series()[0].points.constData()==plots[3]->series()[axis].points.constData();
            state.insert("four_plots_correct_series",correctSeries);
            if(first)screenshot("full");
            const auto beforeRangeBuilds=window.property("wavePreviewBuilds").toULongLong();
            start->setValue(.05);end->setValue(.051);window.findChild<QPushButton*>("applyRange")->click();app.processEvents();
            state.insert("four_plots_one_preview_per_range",window.property("wavePreviewBuilds").toULongLong()==beforeRangeBuilds+1);
            state.insert("four_plots_range_shared",sharedRange(plots,start->value(),end->value()));
            if(first)screenshot("zoom");
            auto* waveform=window.findChild<vibration::gui::PlotWidget*>("waveformPlot");waveform->selectAtX(.050011);app.processEvents();
            const auto capture=vibration::load_capture(path.toUtf8().toStdString());const auto index=std::size_t(std::llround(.050011*capture.header.sample_rate_hz));
            const double multiplier=(double(capture.header.adc_range_uv)/1e6/32768.0)/(analysis.value("configuration").toObject().value("analog_gain").toDouble()*analysis.value("configuration").toObject().value("sensor_sensitivity_v_per_g").toDouble());
            const auto cursor=window.findChild<QLabel*>("waveCursor")->text();
            bool rawExact=cursor.startsWith(QString::number(double(index)/capture.header.sample_rate_hz,'f',5)+" s");
            for(int axis=0;axis<3;++axis)rawExact=rawExact&&cursor.contains(QString("CH%1 %2").arg(axis+1).arg(QString::number(capture.records[index].raw[axis]*multiplier,'f',5)));
            state.insert("raw_cursor_exact",rawExact);
            bool allCursorExact=true;
            for(auto* plot:plots){plot->selectAtX(.050011);app.processEvents();allCursorExact=allCursorExact&&sameCursor(plots,double(index)/capture.header.sample_rate_hz)&&window.findChild<QLabel*>("waveCursor")->text()==cursor;}
            state.insert("four_plots_raw_cursor_sync_exact",allCursorExact);
            if(first){waveform->selectAtX(.0505);app.processEvents();screenshot("cursor");waveform->selectAtX(.050011);app.processEvents();}
            const auto localCapture=qEnvironmentVariable("VIBRATION_GUI_LOCAL_SCREENSHOT");
            if(first&&!localCapture.isEmpty())window.grab().save(localCapture,"PNG");
            const auto drag=[&](vibration::gui::PlotWidget* plot,double fraction){
                const auto area=plot->plotArea();const QPointF begin=area.center(),finish(area.left()+area.width()*fraction,area.center().y());
                QMouseEvent press(QEvent::MouseButtonPress,begin,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(plot,&press);
                for(int i=0;i<100;++i){QMouseEvent move(QEvent::MouseMove,begin+(finish-begin)*(i/100.0),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(plot,&move);}
                QMouseEvent release(QEvent::MouseButtonRelease,finish,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(plot,&release);app.processEvents();
            };
            drag(waveform,.034);const auto draggedIndex=std::size_t(std::llround(.050034*capture.header.sample_rate_hz));const auto draggedText=window.findChild<QLabel*>("waveCursor")->text();
            bool dragExact=draggedText.startsWith(QString::number(double(draggedIndex)/capture.header.sample_rate_hz,'f',5)+" s");
            for(int axis=0;axis<3;++axis)dragExact=dragExact&&draggedText.contains(QString("CH%1 %2").arg(axis+1).arg(QString::number(capture.records[draggedIndex].raw[axis]*multiplier,'f',5)));
            state.insert("raw_drag_cursor_exact",dragExact);
            bool allDragExact=true;
            for(auto* plot:plots){drag(plot,.034);allDragExact=allDragExact&&sameCursor(plots,double(draggedIndex)/capture.header.sample_rate_hz)&&window.findChild<QLabel*>("waveCursor")->text()==draggedText;}
            state.insert("four_plots_drag_cursor_sync_exact",allDragExact);
            const auto previewBuilds=window.property("wavePreviewBuilds").toULongLong();QElapsedTimer sameRange;sameRange.start();window.findChild<QPushButton*>("applyRange")->click();app.processEvents();
            state.insert("same_range_apply_and_event_pump_ns",sameRange.nsecsElapsed());
            state.insert("same_range_avoids_preview_scan",previewBuilds==window.property("wavePreviewBuilds").toULongLong());
            start->setValue(.06);end->setValue(.061);window.findChild<QPushButton*>("applyRange")->click();app.processEvents();
            const auto afterB=window.property("wavePreviewBuilds").toULongLong();start->setValue(.05);end->setValue(.051);window.findChild<QPushButton*>("applyRange")->click();app.processEvents();
            state.insert("range_a_b_a_reuses_preview",afterB==previewBuilds+1&&afterB==window.property("wavePreviewBuilds").toULongLong());
            bool exploreTargets=true;for(const char* id:{"waveZoomIn","waveZoomOut","wavePanLeft","wavePanRight","fullRange"}){const auto* button=window.findChild<QPushButton*>(id);exploreTargets=exploreTargets&&button&&button->width()>=44&&button->height()>=44;}
            state.insert("exploration_touch_targets",exploreTargets);
            for(int i=0;i<20;++i)window.findChild<QPushButton*>("waveZoomIn")->click();app.processEvents();
            bool boundaries=end->value()>start->value()&&end->value()-start->value()>=1.0/capture.header.sample_rate_hz-1e-9;
            for(int i=0;i<20;++i)window.findChild<QPushButton*>("waveZoomOut")->click();
            for(int i=0;i<20;++i)window.findChild<QPushButton*>("wavePanRight")->click();
            boundaries=boundaries&&end->value()<=analysis.value("duration_s").toDouble()+1e-5;
            for(int i=0;i<20;++i)window.findChild<QPushButton*>("wavePanLeft")->click();
            boundaries=boundaries&&start->value()==0;state.insert("zoom_pan_boundaries",boundaries);
            auto* waveChannel=window.findChild<QCheckBox*>("waveChannel1");const bool checked=waveChannel->isChecked();const QPointF corner(waveChannel->width()-2,waveChannel->height()-2);
            bool navigationSync=true;
            for(const char* id:{"waveZoomIn","waveZoomOut","wavePanRight","wavePanLeft","fullRange"}){window.findChild<QPushButton*>(id)->click();app.processEvents();navigationSync=navigationSync&&sharedRange(plots,start->value(),end->value());}
            state.insert("four_plots_navigation_range_sync",navigationSync);
            bool overlayOnly=true;
            const auto buildsBeforeToggle=window.property("wavePreviewBuilds").toULongLong();
            for(int axis=0;axis<3;++axis){
                auto* box=window.findChild<QCheckBox*>(QString("waveChannel%1").arg(axis+1));
                box->setChecked(false);app.processEvents();
                overlayOnly=overlayOnly&&!plots[3]->series()[axis].visible;
                for(int single=0;single<3;++single)overlayOnly=overlayOnly&&plots[single]->isVisible()&&plots[single]->series().size()==1&&plots[single]->series()[0].visible&&plots[single]->series()[0].points==plots[3]->series()[single].points;
                box->setChecked(true);app.processEvents();overlayOnly=overlayOnly&&plots[3]->series()[axis].visible;
            }
            state.insert("four_plots_checkboxes_overlay_only",overlayOnly&&buildsBeforeToggle==window.property("wavePreviewBuilds").toULongLong());
            for(int axis=0;axis<3;++axis)window.findChild<QCheckBox*>(QString("waveChannel%1").arg(axis+1))->setChecked(false);
            app.processEvents();drag(waveform,.634);
            const auto hiddenOverlayTime=double(std::llround((start->value()+(end->value()-start->value())*.634)*capture.header.sample_rate_hz))/capture.header.sample_rate_hz;
            state.insert("hidden_overlay_expected_cursor_s",hiddenOverlayTime);
            state.insert("hidden_overlay_actual_cursor_s",waveform->cursorX()?QJsonValue(*waveform->cursorX()):QJsonValue(QJsonValue::Null));
            state.insert("four_plots_hidden_overlay_cursor_sync",sameCursor(plots,hiddenOverlayTime));
            for(int axis=0;axis<3;++axis)window.findChild<QCheckBox*>(QString("waveChannel%1").arg(axis+1))->setChecked(true);
            app.processEvents();
            QMouseEvent checkPress(QEvent::MouseButtonPress,corner,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(waveChannel,&checkPress);
            QMouseEvent checkRelease(QEvent::MouseButtonRelease,corner,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(waveChannel,&checkRelease);
            state.insert("channel_full_touch_rect",waveChannel->width()>=44&&waveChannel->height()>=44&&waveChannel->isChecked()!=checked);waveChannel->setChecked(checked);
            window.setPage("spectrum");app.processEvents();drag(spectrum,33.21/400.0);
            state.insert("fft_drag_cursor_exact",window.findChild<QLabel*>("frequencyCursor")->text().startsWith(QString::number(nearest,'f',3)+" Hz"));
            window.setPage("waveform");app.processEvents();
            const auto jsonBefore=analysis.value("spectra");const auto unchangedPath=temporary.filePath("after-range.json");window.saveResult(unchangedPath,false,&error);QFile after(unchangedPath);after.open(QIODevice::ReadOnly);const auto afterJson=QJsonDocument::fromJson(after.readAll()).object();state.insert("range_keeps_analysis",jsonBefore==afterJson.value("spectra")&&analysis.value("time_stats")==afterJson.value("time_stats"));
            window.findChild<QPushButton*>("fullRange")->click();window.setPage("files");app.processEvents();
        }
        bool detailsOpened = false, detailsTouchTargets = false;
        auto* details = window.findChild<QPushButton*>("analysisDetails");
        if (details->isEnabled()) {
            QTimer::singleShot(60, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                detailsOpened = dialog->findChild<QTextBrowser*>("detailsContent") != nullptr;
                detailsTouchTargets = true;
                for (const auto* button : dialog->findChildren<QPushButton*>())
                    detailsTouchTargets = detailsTouchTargets && button->height() >= 44;
                const auto capture = qEnvironmentVariable("VIBRATION_GUI_DETAILS_SCREENSHOT");
                if (first && !capture.isEmpty()) dialog->grab().save(capture, "PNG");
                dialog->accept();
            });
            details->click();
        }
        state.insert("details_opened", detailsOpened);
        state.insert("details_touch_targets", detailsTouchTargets);
        const auto parameterCapture = qEnvironmentVariable("VIBRATION_GUI_PARAMETERS_SCREENSHOT");
        if (first && !parameterCapture.isEmpty()) {
            QTimer::singleShot(60, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                dialog->grab().save(parameterCapture, "PNG");
                dialog->reject();
            });
            window.findChild<QPushButton*>("analysisParameters")->click();
        }
        states.append(state);
        first = false;
    }
    if(!parser.positionalArguments().isEmpty()) {
        auto* closing=new vibration::gui::MainWindow(initialConfig);closing->show();closing->openCapture(parser.positionalArguments().first());delete closing;app.processEvents();
    }
    const auto output = QJsonDocument(states).toJson(QJsonDocument::Compact);
    std::fwrite(output.constData(), 1, std::size_t(output.size()), stdout);
    return 0;
}
