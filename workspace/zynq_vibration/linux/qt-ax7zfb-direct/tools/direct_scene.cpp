// SPDX-License-Identifier: LGPL-3.0-only
// Deterministic synthetic pixel fixture; never starts the acquisition service.
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QFileInfo>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/private/qinputdevicemanager_p_p.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

class Canvas final : public QWidget {
public:
    Canvas(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags()) : QWidget(parent, flags)
    { setAttribute(Qt::WA_OpaquePaintEvent); }
    bool auxiliary = false;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this); p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), auxiliary ? QColor("#dae5ff") : QColor("#f8f9fb"));
        p.setPen(QColor("#152340")); QFont f = font(); f.setPixelSize(auxiliary ? 20 : 24); p.setFont(f);
        p.drawText(QRect(24, 16, width() - 48, 48), Qt::AlignLeft | Qt::AlignVCenter,
            auxiliary ? "SECOND NATIVE WINDOW" : "SYNTHETIC QPA PIXEL FIXTURE");
        if (auxiliary) {
            for (int i = 0; i < 5; ++i) p.fillRect(24 + i * 48, 84, 30, 70, QColor::fromHsv(i * 45, 180, 210));
            return;
        }
        static const QRgb colors[] = {0xff3769c7,0xff268878,0xffb78032,0xffb84768,0xff777f8c,0xff263e70};
        for (int i = 0; i < 6; ++i) p.fillRect(30 + i * 196, 84, 176, 70, QColor::fromRgb(colors[i]));
        p.setPen(QColor("#d3d7df"));
        for (int x = 36; x < width() - 24; x += 40) p.drawLine(x, 200, x, height() - 52);
        for (int y = 200; y < height() - 32; y += 40) p.drawLine(36, y, width() - 24, y);
        p.setPen(QPen(QColor("#3769c7"), 2));
        for (int x = 36; x < width() - 28; ++x) {
            const int y = 340 + ((x / 16) % 7 - 3) * 24;
            p.drawPoint(x, y);
        }
        f.setPixelSize(14); p.setFont(f); p.setPen(QColor("#343638"));
        p.drawText(36, height() - 24, QString::fromUtf8("固定字体像素对照  CH1 0123456789  /  No sensor measurement"));
    }
};
class FixedDialog final : public QDialog {
public:
    explicit FixedDialog(QWidget *parent) : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    { setAttribute(Qt::WA_OpaquePaintEvent); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this); p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), QColor("#fff1c7")); p.setPen(QPen(QColor("#705622"), 3));
        p.drawRect(rect().adjusted(2,2,-3,-3));
        p.drawText(rect().adjusted(24,24,-24,-24), Qt::AlignCenter,
            "DIALOG FALLBACK\nDeterministic native overlay");
    }
};
static int runScene(int argc, char **argv)
{
    QString control; bool pointerFixture = false;
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "--control-file" && i + 1 < argc) control = QString::fromLocal8Bit(argv[++i]);
        if (QByteArray(argv[i]) == "--pointer-fixture") pointerFixture = true;
    }
    const QFileInfo info(control), parent(info.absolutePath());
    if (!control.startsWith("/tmp/") || !info.isFile() || info.isSymLink() ||
        info.size() > 32 || !parent.isDir() || parent.isSymLink() ||
        parent.ownerId() != 0 || (parent.permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther))) {
        std::fprintf(stderr, "Use an existing control file under a new root-owned private /tmp directory\n"); return 2;
    }
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != "ax7zfb") { std::fprintf(stderr, "Explicit ax7zfb backend required\n"); return 2; }
    const QRect fixedGeometry = QGuiApplication::primaryScreen()->geometry();
    if (fixedGeometry != QRect(0,0,1280,720)) { std::fprintf(stderr, "Fixed1280x720 screen required\n"); return 2; }
    for (const char *name : {"segoeui.ttf","seguisb.ttf","msyh.ttc","msyhbd.ttc"})
        QFontDatabase::addApplicationFont(QString("/usr/share/fonts/truetype/workbench/") + name);
    QFont font("Segoe UI,Microsoft YaHei UI,Microsoft YaHei,Noto Sans CJK SC,sans-serif");
    font.setPixelSize(14); app.setFont(font);
    Canvas main; main.setObjectName("directPixelMain"); main.showFullScreen(); main.setGeometry(fixedGeometry);
    FixedDialog dialog(&main); dialog.resize(420,230);
    Canvas second(nullptr, Qt::Window | Qt::FramelessWindowHint); second.auxiliary = true; second.resize(330,210);
    auto *manager = QGuiApplicationPrivate::inputDeviceManager();
    const int originalPointers = manager->deviceCount(QInputDeviceManager::DeviceTypePointer);
    QString last; quint64 serial = 0;
    QTimer poll; poll.setInterval(80);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        const QByteArray name = QFile::encodeName(control);
        const int fd = open(name.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { app.exit(3); return; }
        char data[33] = {}; const ssize_t n = read(fd, data, 32); close(fd);
        if (n <= 0 || n >= 32) return;
        const QString command = QString::fromLatin1(data, int(n)).trimmed();
        if (command == last) return;
        if (command == "quit") { app.quit(); return; }
        const QStringList allowed{"base","dialog","multi","cursor","opacity","mask","offset"};
        if (!allowed.contains(command) || (command == "cursor" && !pointerFixture)) {
            std::fprintf(stderr, "Rejected synthetic scene command\n"); app.exit(4); return;
        }
        last = command; const quint64 current = ++serial;
        dialog.hide(); second.hide(); main.setMask(QRegion()); main.setWindowOpacity(1.0);
        main.showFullScreen(); main.setGeometry(fixedGeometry); main.raise();
        if (pointerFixture) QInputDeviceManagerPrivate::get(manager)->setDeviceCount(
            QInputDeviceManager::DeviceTypePointer, command == "cursor" ? 1 : 0);
        if (command == "dialog") { dialog.setGeometry(180,160,420,230); dialog.show(); dialog.raise(); }
        if (command == "multi") { second.setGeometry(710,350,330,210); second.show(); second.raise(); }
        if (command == "cursor") { main.setCursor(Qt::ArrowCursor); QCursor::setPos(120,120); }
        if (command == "opacity") main.setWindowOpacity(.5);
        if (command == "mask") main.setMask(QRegion(QRect(0,0,1180,720)));
        if (command == "offset") { main.showNormal(); main.setGeometry(20,10,1240,700); }
        main.update(); dialog.update(); second.update();
        // A second paint also lets a removed cursor retire through the original
        // fallback before the strict no-cursor path becomes eligible again.
        QTimer::singleShot(80, &main, [&] { main.update(); });
        QTimer::singleShot(450, &main, [&, current, command] {
            if (current != serial) return;
            const QRect expected = command == "offset" ? QRect(20,10,1240,700) : fixedGeometry;
            if (main.geometry() != expected) {
                std::fprintf(stderr, "Scene geometry verification failed for%s\n", qPrintable(command));
                app.exit(5); return;
            }
            const auto bytes = QJsonDocument(QJsonObject{{"schema","ax7z020.qpa-static-fixture.v1"},
                {"fixture_revision",3},{"geometry_verified",true},
                {"event","scene_ready"},{"scene",command},{"serial",double(current)},
                {"pid",double(getpid())},{"synthetic_pixel_fixture",true},{"acquisition_started",false},
                {"pointer_fixture",pointerFixture},{"pointer_count",manager->deviceCount(QInputDeviceManager::DeviceTypePointer)},
                {"direct_opt_in",qgetenv("AX7ZFB_DIRECT_SINGLE") == "1"},
                {"geometry",QString("%1,%2,%3,%4").arg(main.geometry().x()).arg(main.geometry().y()).arg(main.width()).arg(main.height())}}).toJson(QJsonDocument::Compact);
            std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout); std::fputc('\n', stdout); std::fflush(stdout);
        });
    });
    poll.start(); const int result = app.exec();
    if (pointerFixture) QInputDeviceManagerPrivate::get(manager)->setDeviceCount(QInputDeviceManager::DeviceTypePointer, originalPointers);
    return result;
}
int main(int argc, char **argv)
{
    // Qt windows/application are fully destroyed before this diagnostic exit
    // marker; the controller also requires the exact process to disappear.
    const int result = runScene(argc, argv);
    std::fprintf(stdout, "AX7ZFB_SCENE_EXIT code=%d pid=%ld\n", result, long(getpid()));
    std::fflush(stdout);
    return result;
}
