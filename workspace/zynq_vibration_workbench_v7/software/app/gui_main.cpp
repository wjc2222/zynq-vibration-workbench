#include "main_window.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QFont>
#include <QPixmap>
#include <QTimer>
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QTextCodec>
#endif
#include <cmath>
#include <cstdio>

int main(int argc, char** argv) {
#if defined(Q_OS_LINUX) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Linux capture paths are UTF-8, including argv under an initramfs C locale.
    // Qt 6 already uses UTF-8; Windows keeps its native path handling.
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName("vibration_gui");
    QApplication::setApplicationVersion("0.1.0");
    QFont font;
    font.setFamilies({"Microsoft YaHei UI", "Microsoft YaHei", "Noto Sans CJK SC", "WenQuanYi Micro Hei", "WenQuanYi Zen Hei", "sans-serif"});
    font.setPixelSize(14);
    font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferVerticalHinting);
    app.setFont(font);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Zynq 三通道振动采集与分析工作台"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument("capture", "Optional CAPDDR2 BIN file", "[capture]");
    QCommandLineOption screenshot("screenshot", "Save a software-rendered screenshot after analysis and exit", "png");
    parser.addOption(screenshot);
    QCommandLineOption fullscreen("fullscreen", "Use the entire attached display");
    parser.addOption(fullscreen);
    QCommandLineOption directory("directory", "Preselect a BIN directory without opening or analyzing files", "path");
    parser.addOption(directory);
    QCommandLineOption ppr("ppr", "Explicit pulses per revolution when the file declares unknown PPR", "number");
    parser.addOption(ppr);
    QCommandLineOption page("page", "Open workspace: files, capture, waveform, spectrum, speed, diagnosis, features", "name", "files");
    parser.addOption(page);
    QCommandLineOption detectionProfile("detection-profile", "Load a strict experimental detection profile", "path");
    parser.addOption(detectionProfile);
    QCommandLineOption setupId("setup-id", "Explicit current bench and installation identifier", "id");
    parser.addOption(setupId);
    QCommandLineOption size("size", "Initial window dimensions, e.g. 1280x720", "WxH", "1024x600");
    parser.addOption(size);
    parser.process(app);

    vibration::AnalysisConfig initialConfig;
    if (parser.isSet(ppr)) {
        bool valid = false;
        const double value = parser.value(ppr).toDouble(&valid);
        if (!valid || !std::isfinite(value) || value < 1e-6 || value > 1e9) {
            std::fputs("Invalid --ppr: expected a finite number in [1e-6, 1e9].\n", stderr);
            return 2;
        }
        initialConfig.ppr_override = value;
    }
    vibration::gui::MainWindow window(initialConfig);
    QString detectionError;
    if(parser.isSet(setupId)&&!window.setDetectionSetupId(parser.value(setupId),&detectionError)){
        std::fprintf(stderr,"Invalid --setup-id: %s\n",detectionError.toUtf8().constData());return 2;
    }
    if(parser.isSet(detectionProfile)&&!window.loadDetectionProfile(parser.value(detectionProfile),&detectionError)){
        std::fprintf(stderr,"Invalid --detection-profile: %s\n",detectionError.toUtf8().constData());return 2;
    }
    if (parser.isSet(directory)) window.setDataDirectory(parser.value(directory));
    if (!window.setPage(parser.value(page))) {
        std::fputs("Invalid --page.\n", stderr); return 2;
    }
    const auto dimensions = parser.value(size).split('x');
    bool widthOk=false, heightOk=false;
    const int width=dimensions.size()==2?dimensions[0].toInt(&widthOk):0;
    const int height=dimensions.size()==2?dimensions[1].toInt(&heightOk):0;
    if(!widthOk||!heightOk||width<1024||height<600||width>7680||height>4320){
        std::fputs("Invalid --size: expected dimensions from 1024x600 to 7680x4320.\n",stderr);return 2;
    }
    window.resize(width,height);
    if (parser.isSet(fullscreen)) window.showFullScreen(); else window.show();
    if (!parser.positionalArguments().isEmpty()) window.openCapture(parser.positionalArguments().first());
    if (parser.isSet(screenshot)) {
        auto* timer = new QTimer(&app);
        timer->setInterval(150);
        QObject::connect(timer, &QTimer::timeout, &app, [&window, &app, timer, path = parser.value(screenshot)] {
            if (window.isAnalyzing()) return;
            timer->stop();
            const bool saved = window.grab().save(path, "PNG");
            app.exit(saved ? 0 : 2);
        });
        timer->start();
        QTimer::singleShot(60000, &app, [&app] { app.exit(3); });
    }
    return app.exec();
}
