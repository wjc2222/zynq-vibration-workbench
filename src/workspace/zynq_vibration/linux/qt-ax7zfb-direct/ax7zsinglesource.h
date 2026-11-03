// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include <QtCore/QRect>
#include <QtGui/QImage>
#include <QtGui/QRegion>

namespace Ax7zFb {
inline bool cursorCanDraw(bool cursorExists, bool hiddenAtInitialization, int pointerCount)
{
    // Mirrors QFbCursor::updateMouseStatus visibility, rather than isOnScreen:
    // Qt retains that historical flag after a pointer device is removed.
    // Re-entry from compositing always replaces the entire frame below.
    return cursorExists && !hiddenAtInitialization && pointerCount != 0;
}
// Shared by the actual presenter and host pixel/fallback tests.
struct SourceCoverage {
    const void *store = nullptr;
    const uchar *bits = nullptr;
    QSize size;
    int format = QImage::Format_Invalid, stride = 0;
    qreal dpr = 0;
    QRegion valid;

    bool matches(const void *candidate, const QImage &image) const
    {
        return store == candidate && bits == image.constBits() && size == image.size() &&
               format == image.format() && stride == image.bytesPerLine() &&
               dpr == image.devicePixelRatio();
    }
    void observe(const void *candidate, const QImage &image, const QRegion &flushed)
    {
        if (image.isNull()) { *this = SourceCoverage(); return; }
        if (!matches(candidate, image)) {
            store = candidate; bits = image.constBits(); size = image.size();
            format = image.format(); stride = image.bytesPerLine();
            dpr = image.devicePixelRatio(); valid = QRegion();
        }
        valid += flushed.intersected(image.rect());
    }
    bool complete(const void *candidate, const QImage &image) const
    {
        return !image.isNull() && matches(candidate, image) &&
               QRegion(image.rect()).subtracted(valid).isEmpty();
    }
};

enum class DirectReason {
    Eligible, Disabled, WindowCount, Invisible, NativeChild, WindowType, Geometry, Mask,
    Opacity, Cursor, NoStore, Format, Size, Stride, Dpr, Incomplete, Count
};
inline const char *reasonName(DirectReason reason)
{
    static const char *const names[] = {"eligible", "disabled", "window_count", "invisible",
        "native_child", "window_type", "geometry", "mask", "opacity", "cursor", "no_store", "format",
        "size", "stride", "dpr", "incomplete"};
    const int i = int(reason);
    return i >= 0 && i < int(DirectReason::Count) ? names[i] : "invalid";
}

struct DirectCandidate {
    bool enabled = false;
    int windows = 0;
    bool visible = false, nativeChildren = false, ordinaryWindow = false, maskEmpty = true;
    QRect screen, window;
    qreal opacity = 1, windowDpr = 1;
    bool cursorCanDraw = true, complete = false;
    const void *windowToken = nullptr, *storeToken = nullptr;
    QImage source;
};

inline DirectReason evaluateDirect(const DirectCandidate &c)
{
    if (!c.enabled) return DirectReason::Disabled;
    if (c.windows != 1) return DirectReason::WindowCount;
    if (!c.visible) return DirectReason::Invisible;
    if (c.nativeChildren) return DirectReason::NativeChild;
    if (!c.ordinaryWindow) return DirectReason::WindowType;
    if (c.screen.topLeft() != QPoint(0, 0) || c.window != c.screen) return DirectReason::Geometry;
    if (!c.maskEmpty) return DirectReason::Mask;
    if (c.opacity != qreal(1)) return DirectReason::Opacity;
    if (c.cursorCanDraw) return DirectReason::Cursor;
    if (!c.windowToken || !c.storeToken || c.source.isNull()) return DirectReason::NoStore;
    if (c.source.format() != QImage::Format_RGB32 || c.source.hasAlphaChannel()) return DirectReason::Format;
    if (c.source.size() != c.screen.size()) return DirectReason::Size;
    if (c.source.bytesPerLine() != c.screen.width() * 4) return DirectReason::Stride;
    if (c.source.devicePixelRatio() != qreal(1) || c.windowDpr != qreal(1)) return DirectReason::Dpr;
    if (!c.complete) return DirectReason::Incomplete;
    return DirectReason::Eligible;
}

struct DirectKey {
    const void *window = nullptr, *store = nullptr;
    const uchar *bits = nullptr;
    QSize size;
    bool operator==(const DirectKey &other) const
    { return window == other.window && store == other.store && bits == other.bits && size == other.size; }
    bool operator!=(const DirectKey &other) const { return !(*this == other); }
};
inline DirectKey directKey(const DirectCandidate &c)
{
    DirectKey key;
    key.window = c.windowToken; key.store = c.storeToken;
    key.bits = c.source.constBits(); key.size = c.source.size();
    return key;
}

struct DirectDecision {
    DirectReason reason = DirectReason::Disabled;
    bool direct = false, fullSource = false, fullComposite = false;
    DirectKey key;
};
class DirectMode {
public:
    DirectDecision decide(const DirectCandidate &candidate) const
    {
        DirectDecision result;
        result.reason = evaluateDirect(candidate);
        result.direct = result.reason == DirectReason::Eligible;
        if (result.direct) {
            result.key = directKey(candidate);
            result.fullSource = !mDirect || result.key != mKey;
        } else result.fullComposite = mDirect;
        return result;
    }
    // Call only after presentation succeeds; PAN failure never changes ownership.
    void commit(const DirectDecision &decision)
    { mDirect = decision.direct; mKey = decision.direct ? decision.key : DirectKey(); }
    bool direct() const { return mDirect; }
private:
    bool mDirect = false;
    DirectKey mKey;
};
}
