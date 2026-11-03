// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QRegion>
#include <functional>
#include <linux/fb.h>
#include <cstring>

namespace Ax7zFb {
constexpr int Width = 1280;
constexpr int Height = 720;
constexpr int Stride = Width * 4;
constexpr int PageBytes = Stride * Height;
constexpr int BufferBytes = PageBytes * 2;

// This deliberately accepts only the reviewed fixed platform. No PUT_VSCREENINFO
// or generic framebuffer fallback is permitted in the explicitly selected plugin.
inline const char *validateMode(const fb_fix_screeninfo &f, const fb_var_screeninfo &v)
{
    const char id[16] = "AX7Z020-720p";
    if (std::memcmp(f.id, id, sizeof(id)) != 0)
        return "driver framebuffer identity is not AX7Z020-720p";
    if (f.type != FB_TYPE_PACKED_PIXELS || f.visual != FB_VISUAL_TRUECOLOR)
        return "framebuffer must be packed truecolor";
    if (v.xres != Width || v.yres != Height || v.bits_per_pixel != 32 ||
        v.xres_virtual != Width || v.yres_virtual != Height * 2)
        return "requires exactly 1280x720x32 with 1280x1440 virtual geometry";
    if (f.line_length != Stride || f.smem_len != BufferBytes ||
        f.ypanstep != Height || f.xpanstep != 0 || f.ywrapstep != 0)
        return "requires two exact pages, 5120-byte stride and 720-line ypanstep";
    if (v.xoffset || (v.yoffset != 0 && v.yoffset != Height) ||
        v.vmode != FB_VMODE_NONINTERLACED || v.rotate || v.grayscale || v.nonstd)
        return "unsupported offset, interlace, rotation or nonstandard mode";
    const fb_bitfield expected[4] = {{16, 8, 0}, {8, 8, 0}, {0, 8, 0}, {24, 0, 0}};
    const fb_bitfield actual[4] = {v.red, v.green, v.blue, v.transp};
    if (std::memcmp(expected, actual, sizeof(actual)))
        return "requires the reviewed XRGB8888 channel layout";
    if (v.pixclock != 13468 || v.left_margin != 220 || v.right_margin != 110 ||
        v.upper_margin != 20 || v.lower_margin != 5 ||
        v.hsync_len != 40 || v.vsync_len != 5 ||
        v.sync != (FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT))
        return "requires the fixed progressive 720p60 timing";
    return nullptr;
}

// Shared by the real backend and host regression tests. Every source change is
// added to BOTH pages' debt. A successful submit clears only the page just drawn.
// Therefore a page reused after two flips catches up with both intervening
// damages, while stable areas are never copied after initial page initialization.
class FlipBuffer {
public:
    enum Result { NoChange, Submitted, PanFailed, Refused };
    struct Work {
        int page = -1;
        QRegion copied;
    };

    bool initialize(const QImage &page0, const QImage &page1, int front)
    {
        if (front < 0 || front > 1 || page0.isNull() || page1.isNull() ||
            page0.size() != page1.size() || page0.format() != QImage::Format_RGB32 ||
            page1.format() != QImage::Format_RGB32 || page0.constBits() == page1.constBits())
            return false;
        mPages[0] = page0;
        mPages[1] = page1;
        mFront = front;
        mBounds = page0.rect();
        mDebt[0] = mDebt[1] = QRegion(mBounds);
        mHealthy = true;
        return true;
    }

    Result present(const QImage &source, const QRegion &touched,
                   const std::function<bool(int)> &pan, Work *work = nullptr)
    {
        if (work) *work = Work();
        if (!mHealthy || source.size() != mBounds.size() ||
            source.format() != QImage::Format_RGB32)
            return Refused;
        const QRegion damage = touched.intersected(mBounds);
        if (damage.isEmpty())
            return NoChange;
        mDebt[0] += damage;
        mDebt[1] += damage;
        const int back = 1 - mFront;
        const QRegion copy = mDebt[back];
        // A scoped painter and explicit end are essential: no CPU raster writes
        // to the candidate page may remain active when its PAN is requested.
        QPainter painter;
        if (!painter.begin(&mPages[back])) {
            mHealthy = false;
            return Refused;
        }
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        for (const QRect &rect : copy)
            painter.drawImage(rect, source, rect);
        if (!painter.end()) {
            mHealthy = false;
            return Refused;
        }
        if (work) { work->page = back; work->copied = copy; }
        if (!pan(back)) {
            // A timed-out ioctl may already have changed scanout. Neither page
            // can be assumed writable again, even if GET_VSCREENINFO is stale.
            mHealthy = false;
            return PanFailed;
        }
        mDebt[back] = QRegion();
        mFront = back;
        return Submitted;
    }

    bool healthy() const { return mHealthy; }
    int front() const { return mHealthy ? mFront : -1; }
    const QImage &frontImage() const { return mPages[mFront]; }

private:
    QImage mPages[2];
    QRegion mDebt[2];
    QRect mBounds;
    int mFront = -1;
    bool mHealthy = false;
};
}
