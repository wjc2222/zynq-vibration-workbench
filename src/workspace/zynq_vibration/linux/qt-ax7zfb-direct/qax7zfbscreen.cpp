// SPDX-License-Identifier: LGPL-3.0-only
#include "qax7zfbscreen.h"
#include <QtFbSupport/private/qfbcursor_p.h>
#include <QtFbSupport/private/qfbwindow_p.h>
#include <QtFbSupport/private/qfbbackingstore_p.h>
#include <QtGui/private/qguiapplication_p.h>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QDebug>
#include <QtGui/QPixmap>
#include <QtGui/QWindow>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/kd.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

QT_BEGIN_NAMESPACE
QAx7zFbScreen::QAx7zFbScreen(const QStringList &args) : mArgs(args) {}

QAx7zFbScreen::~QAx7zFbScreen()
{
    if (mStatsInterval) logStats("shutdown");
    if (mMapping) munmap(mMapping, Ax7zFb::BufferBytes);
    if (mFbFd >= 0) close(mFbFd);
    if (mTtyFd >= 0) {
        if (mOldTtyMode >= 0) ioctl(mTtyFd, KDSETMODE, mOldTtyMode);
        close(mTtyFd);
    }
}

bool QAx7zFbScreen::pan(int page)
{
    fb_var_screeninfo request = mVar;
    request.xoffset = 0;
    request.yoffset = page * Ax7zFb::Height;
    request.activate = FB_ACTIVATE_VBL;
    if (ioctl(mFbFd, FBIOPAN_DISPLAY, &request) != 0)
        return false;
    mVar.yoffset = request.yoffset;
    return true;
}

bool QAx7zFbScreen::initialize()
{
    const QByteArray direct = qgetenv("AX7ZFB_DIRECT_SINGLE");
    if (!direct.isEmpty() && direct != "0" && direct != "1") {
        qCritical("ax7zfb: AX7ZFB_DIRECT_SINGLE must be0 or1"); return false;
    }
    mDirectEnabled = direct == "1";
    mCursorExplicitlyHidden = qEnvironmentVariableIsSet("QT_QPA_FB_HIDECURSOR") &&
        qEnvironmentVariableIntValue("QT_QPA_FB_HIDECURSOR") != 0;
    QString fbDevice = QStringLiteral("/dev/fb0"), ttyDevice;
    bool graphics = true;
    for (const QString &arg : qAsConst(mArgs)) {
        if (arg.startsWith(QLatin1String("fb="))) fbDevice = arg.mid(3);
        else if (arg.startsWith(QLatin1String("tty="))) ttyDevice = arg.mid(4);
        else if (arg == QLatin1String("nographicsmodeswitch")) graphics = false;
        else { qCritical("ax7zfb: unsupported argument '%s'", qPrintable(arg)); return false; }
    }
    bool statsOk = false;
    const QByteArray stats = qgetenv("AX7ZFB_STATS_INTERVAL");
    if (!stats.isEmpty()) {
        mStatsInterval = stats.toInt(&statsOk);
        if (!statsOk || mStatsInterval < 0 || mStatsInterval > 1000000) {
            qCritical("ax7zfb: AX7ZFB_STATS_INTERVAL must be 0..1000000"); return false;
        }
    }
    const QByteArray name = QFile::encodeName(fbDevice);
    mFbFd = open(name.constData(), O_RDWR | O_CLOEXEC);
    if (mFbFd < 0) { qErrnoWarning(errno, "ax7zfb: open framebuffer"); return false; }
    if (flock(mFbFd, LOCK_EX | LOCK_NB) != 0) {
        qErrnoWarning(errno, "ax7zfb: framebuffer already owned"); return false;
    }
    fb_fix_screeninfo fixed = {};
    if (ioctl(mFbFd, FBIOGET_FSCREENINFO, &fixed) ||
        ioctl(mFbFd, FBIOGET_VSCREENINFO, &mVar)) {
        qErrnoWarning(errno, "ax7zfb: query framebuffer"); return false;
    }
    if (const char *error = Ax7zFb::validateMode(fixed, mVar)) {
        qCritical("ax7zfb: mode rejected: %s", error); return false;
    }
    // fix.id alone is not a driver capability/version handshake. Also require
    // the board's actual bound platform driver and DT compatible identity.
    struct stat deviceStat = {};
    if (fstat(mFbFd, &deviceStat) != 0 || !S_ISCHR(deviceStat.st_mode)) {
        qCritical("ax7zfb: framebuffer is not a character device"); return false;
    }
    const QString sysDevice = QStringLiteral("/sys/dev/char/%1:%2/device")
        .arg(major(deviceStat.st_rdev)).arg(minor(deviceStat.st_rdev));
    const QFileInfo driverInfo(sysDevice + QStringLiteral("/driver"));
    QFile compatible(sysDevice + QStringLiteral("/of_node/compatible"));
    if (QFileInfo(driverInfo.symLinkTarget()).fileName() != QStringLiteral("codex-ax7z020-fb") ||
        !compatible.open(QIODevice::ReadOnly) ||
        !compatible.readAll().split('\0').contains(QByteArray("codex,ax7z020-vdma-fb"))) {
        qCritical("ax7zfb: required codex-ax7z020-fb platform driver/DT identity absent"); return false;
    }
    // Reassert the software-reported front using the driver's CURRENT-pointer
    // acknowledgement before touching either page. This also recovers from a
    // previous process whose timed-out PAN left hardware and fb var divergent.
    if (!pan(mVar.yoffset / Ax7zFb::Height)) {
        qErrnoWarning(errno, "ax7zfb: initial front acknowledgement failed; refusing all writes");
        return false;
    }
    void *mapping = mmap(nullptr, Ax7zFb::BufferBytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, mFbFd, 0);
    if (mapping == MAP_FAILED) { qErrnoWarning(errno, "ax7zfb: mmap"); return false; }
    mMapping = static_cast<uchar *>(mapping);
    mDepth = 32;
    mFormat = QImage::Format_RGB32;
    mGeometry = QRect(0, 0, Ax7zFb::Width, Ax7zFb::Height);
    mPhysicalSize = QSizeF(325, 183); // same ~100 DPI fallback as LinuxFB
    if (!mFlip.initialize(QImage(mMapping, Ax7zFb::Width, Ax7zFb::Height, Ax7zFb::Stride, mFormat),
                          QImage(mMapping + Ax7zFb::PageBytes, Ax7zFb::Width, Ax7zFb::Height,
                                 Ax7zFb::Stride, mFormat), mVar.yoffset / Ax7zFb::Height))
        return false;
    initializeCompositor();
    // Compositor memory is fully initialized before the initial full-page copy.
    mScreenImage.fill(Qt::black);
    setDirty(mGeometry);
    mCursor = new QFbCursor(this);
    if (ttyDevice.isEmpty()) {
        for (const char *device : {"/dev/tty0", "/dev/tty", "/dev/console"}) {
            mTtyFd = open(device, O_RDWR | O_CLOEXEC);
            if (mTtyFd >= 0) break;
        }
    } else mTtyFd = open(QFile::encodeName(ttyDevice).constData(), O_RDWR | O_CLOEXEC);
    if (mTtyFd >= 0 && ioctl(mTtyFd, KDGETMODE, &mOldTtyMode) == 0 &&
        graphics && mOldTtyMode != KD_GRAPHICS)
        ioctl(mTtyFd, KDSETMODE, KD_GRAPHICS);
    if (mStatsInterval)
        qInfo("ax7zfb: two-page backend enabled; initial_front=%d stats_interval=%d",
              mFlip.front(), mStatsInterval);
    qInfo("ax7zfb: private direct-single candidate v1 enabled=%d default=0", int(mDirectEnabled));
    return true;
}

void QAx7zFbScreen::recordSourceFlush(QFbWindow *window, const QRegion &region)
{
    if (!mDirectEnabled || !window || !window->backingStore()) return;
    auto *store = window->backingStore();
    store->lock();
    {
        const QImage image = store->image();
        mSourceCoverage[window].observe(store, image, region);
    }
    store->unlock();
}

void QAx7zFbScreen::invalidateSource(QFbWindow *window)
{
    mSourceCoverage.remove(window);
}

void QAx7zFbScreen::removeWindow(QFbWindow *window)
{
    invalidateSource(window);
    QFbScreen::removeWindow(window);
}

QRegion QAx7zFbScreen::doRedraw()
{
    if (!mFlip.healthy()) qFatal("ax7zfb: front unknown; refusing framebuffer writes");
    Ax7zFb::DirectCandidate candidate;
    candidate.enabled = mDirectEnabled;
    candidate.screen = mGeometry;
    QFbBackingStore *lockedStore = nullptr;
    if (mDirectEnabled) {
        candidate.windows = mWindowStack.size();
        if (candidate.windows == 1) {
            auto *platform = mWindowStack.first();
            auto *window = platform->window();
            candidate.windowToken = window;
            candidate.visible = window->isVisible() && window->isExposed();
            candidate.window = platform->geometry();
            candidate.ordinaryWindow = window->type() == Qt::Window;
            candidate.maskEmpty = window->mask().isEmpty();
            candidate.opacity = window->opacity();
            candidate.windowDpr = window->devicePixelRatio();
            for (QWindow *child : window->findChildren<QWindow *>())
                if (child->isVisible()) candidate.nativeChildren = true;
            const int pointers = QGuiApplicationPrivate::inputDeviceManager()->deviceCount(
                QInputDeviceManager::DeviceTypePointer);
            candidate.cursorCanDraw = Ax7zFb::cursorCanDraw(mCursor != nullptr,
                mCursorExplicitlyHidden, pointers);
            if (platform->backingStore()) {
                lockedStore = platform->backingStore();
                lockedStore->lock();
                candidate.storeToken = lockedStore;
                candidate.source = lockedStore->image();
                const auto found = mSourceCoverage.constFind(platform);
                candidate.complete = found != mSourceCoverage.constEnd() &&
                    found.value().complete(lockedStore, candidate.source);
            }
        }
    }
    const auto decision = mDirectMode.decide(candidate);
    ++mDirectReasons[int(decision.reason)];
    QRegion touched;
    if (decision.direct) {
        touched = decision.fullSource ? QRegion(QRect(QPoint(0, 0), mGeometry.size())) :
            mRepaintRegion.intersected(QRect(QPoint(0, 0), mGeometry.size()));
        mRepaintRegion = QRegion();
    } else {
        // Unlock before the baseline compositor acquires the same store mutex.
        candidate.source = QImage();
        if (lockedStore) { lockedStore->unlock(); lockedStore = nullptr; }
        if (decision.fullComposite)
            mRepaintRegion += QRect(QPoint(0, 0), mGeometry.size());
        QElapsedTimer composite;
        if (mStatsInterval) composite.start();
        touched = QFbScreen::doRedraw();
        if (mStatsInterval) mCompositeNs += composite.nsecsElapsed();
    }
    QElapsedTimer clock;
    if (mStatsInterval) clock.start();
    qint64 panNs = 0;
    Ax7zFb::FlipBuffer::Work work;
    const auto result = mFlip.present(decision.direct ? candidate.source : mScreenImage, touched, [&](int page) {
        QElapsedTimer panClock;
        if (mStatsInterval) panClock.start();
        const bool ok = pan(page);
        if (mStatsInterval) panNs = panClock.nsecsElapsed();
        return ok;
    }, mStatsInterval ? &work : nullptr);
    candidate.source = QImage();
    if (lockedStore) lockedStore->unlock();
    if (result == Ax7zFb::FlipBuffer::PanFailed)
        qFatal("ax7zfb: PAN failed (%s); front unknown; refusing all further writes", strerror(errno));
    if (result == Ax7zFb::FlipBuffer::Refused)
        qFatal("ax7zfb: compositor/page state rejected; refusing all further writes");
    // Neither logical mode nor source identity advances across an ambiguous PAN.
    mDirectMode.commit(decision);
    if (result == Ax7zFb::FlipBuffer::Submitted) {
        if (decision.direct) { ++mDirectSubmits; if (decision.fullSource) ++mDirectFull; }
        else { ++mCompositeSubmits; if (decision.fullComposite) ++mCompositeFull; }
    }
    if (result == Ax7zFb::FlipBuffer::Submitted && mStatsInterval) {
        ++mSubmits;
        for (const QRect &r : work.copied) mPixels += quint64(r.width()) * r.height();
        mRects += work.copied.rectCount();
        mPanNs += panNs;
        mMaxPanNs = qMax(mMaxPanNs, panNs);
        mCopyNs += clock.nsecsElapsed() - panNs;
        if (mSubmits % mStatsInterval == 0) logStats("interval");
    }
    return touched;
}

void QAx7zFbScreen::logStats(const char *reason) const
{
    qInfo("ax7zfb: stats reason=%s submits=%llu front=%d copy_pixels=%llu copy_rects=%llu copy_us=%lld pan_us=%lld pan_max_us=%lld",
          reason, static_cast<unsigned long long>(mSubmits), mFlip.front(),
          static_cast<unsigned long long>(mPixels), static_cast<unsigned long long>(mRects),
          static_cast<long long>(mCopyNs / 1000), static_cast<long long>(mPanNs / 1000),
          static_cast<long long>(mMaxPanNs / 1000));
    qInfo("ax7zfb: direct_stats enabled=%d direct_submits=%llu composite_submits=%llu entry_full=%llu fallback_full=%llu compositor_us=%lld",
          int(mDirectEnabled), static_cast<unsigned long long>(mDirectSubmits),
          static_cast<unsigned long long>(mCompositeSubmits), static_cast<unsigned long long>(mDirectFull),
          static_cast<unsigned long long>(mCompositeFull), static_cast<long long>(mCompositeNs / 1000));
    if (mDirectEnabled)
        for (int i = 0; i < int(Ax7zFb::DirectReason::Count); ++i)
            if (mDirectReasons[i]) qInfo("ax7zfb: direct_gate reason=%s updates=%llu", Ax7zFb::reasonName(Ax7zFb::DirectReason(i)),
                static_cast<unsigned long long>(mDirectReasons[i]));
}

QPixmap QAx7zFbScreen::grabWindow(WId wid, int x, int y, int width, int height) const
{
    if (!mFlip.healthy()) return QPixmap();
    const QImage &front = mFlip.frontImage();
    if (!wid) {
        if (width < 0) width = front.width() - x;
        if (height < 0) height = front.height() - y;
        return QPixmap::fromImage(front.copy(x, y, width, height));
    }
    if (QFbWindow *window = windowForId(wid)) {
        const QRect geometry = window->geometry();
        if (width < 0) width = geometry.width() - x;
        if (height < 0) height = geometry.height() - y;
        return QPixmap::fromImage(front.copy(QRect(geometry.topLeft() + QPoint(x, y),
                                          QSize(width, height)).intersected(geometry)));
    }
    return QPixmap();
}
QT_END_NAMESPACE
