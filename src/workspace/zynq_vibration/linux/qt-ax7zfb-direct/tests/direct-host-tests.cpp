// SPDX-License-Identifier: LGPL-3.0-only
#include "ax7zsinglesource.h"
#include "ax7zflipbuffer.h"
#include <QtCore/QCoreApplication>
#include <QtGui/QPainter>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (0)
using namespace Ax7zFb;
constexpr int W = 80, H = 48;
const QRect bounds(0, 0, W, H);
static int windowToken, storeToken, otherToken;
static QImage image(QRgb color)
{
    QImage result(W, H, QImage::Format_RGB32); result.fill(color); return result;
}
static void fill(QImage &target, const QRect &rect, QRgb color)
{
    QPainter p(&target); p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect, color); CHECK(p.end());
}
static bool equal(const QImage &a, const QImage &b)
{
    return a.size() == b.size() && a.format() == b.format() &&
        std::memcmp(a.constBits(), b.constBits(), W * H * 4) == 0;
}
static DirectCandidate candidate(const QImage &source)
{
    DirectCandidate c; c.enabled = true; c.windows = 1; c.visible = true;
    c.ordinaryWindow = true; c.screen = c.window = bounds; c.opacity = 1;
    c.cursorCanDraw = false; c.complete = true; c.windowToken = &windowToken;
    c.storeToken = &storeToken; c.source = source;
    return c;
}
static void gates()
{
    CHECK(!cursorCanDraw(false, false, 1));
    CHECK(!cursorCanDraw(true, true, 1));
    CHECK(!cursorCanDraw(true, false, 0));
    CHECK(cursorCanDraw(true, false, 1));
    CHECK(cursorCanDraw(true, false, -1)); // unknown device count is conservative.
    const QImage source = image(qRgb(20, 30, 40));
    CHECK(evaluateDirect(candidate(source)) == DirectReason::Eligible);
#define BAD(field,value,reason) do { auto c = candidate(source); c.field = value; CHECK(evaluateDirect(c) == DirectReason::reason); } while (0)
    BAD(enabled, false, Disabled); BAD(windows, 0, WindowCount); BAD(windows, 2, WindowCount);
    BAD(visible, false, Invisible); BAD(nativeChildren, true, NativeChild);
    BAD(ordinaryWindow, false, WindowType); BAD(window, QRect(1, 0, W, H), Geometry);
    BAD(screen, QRect(1, 1, W, H), Geometry); BAD(maskEmpty, false, Mask);
    BAD(opacity, .9999999, Opacity); BAD(cursorCanDraw, true, Cursor);
    BAD(storeToken, nullptr, NoStore); BAD(windowToken, nullptr, NoStore);
    BAD(source, QImage(), NoStore);
    BAD(source, QImage(W, H, QImage::Format_ARGB32_Premultiplied), Format);
    BAD(source, QImage(W, H, QImage::Format_RGB16), Format);
    BAD(source, QImage(W - 1, H, QImage::Format_RGB32), Size);
    BAD(windowDpr, 2, Dpr); BAD(complete, false, Incomplete);
    { auto c = candidate(source); c.source.setDevicePixelRatio(2); CHECK(evaluateDirect(c) == DirectReason::Dpr); }
    { std::vector<uchar> bytes((W * 4 + 16) * H); auto c = candidate(source);
      c.source = QImage(bytes.data(), W, H, W * 4 + 16, QImage::Format_RGB32);
      CHECK(evaluateDirect(c) == DirectReason::Stride); }
#undef BAD
    for (int i = 0; i < int(DirectReason::Count); ++i) CHECK(std::string(reasonName(DirectReason(i))) != "invalid");
}
static void coverage()
{
    QImage src = image(qRgb(1, 2, 3)); SourceCoverage valid;
    CHECK(!valid.complete(&storeToken, src));
    valid.observe(&storeToken, src, QRect(0, 0, W / 2, H));
    CHECK(!valid.complete(&storeToken, src));
    valid.observe(&storeToken, src, QRect(W / 2, 0, W / 2, H));
    CHECK(valid.complete(&storeToken, src));
    CHECK(!valid.complete(&otherToken, src));
    QImage replacement = image(qRgb(3, 2, 1));
    CHECK(!valid.complete(&storeToken, replacement));
    valid.observe(&storeToken, replacement, QRect(1, 1, 2, 2));
    CHECK(!valid.complete(&storeToken, replacement));
    valid.observe(&storeToken, replacement, bounds); CHECK(valid.complete(&storeToken, replacement));
    replacement.setDevicePixelRatio(2); CHECK(!valid.complete(&storeToken, replacement));
}
struct Harness {
    std::array<std::vector<quint32>, 2> pixels;
    std::array<QImage, 2> pages;
    QImage composite = image(qRgb(0, 0, 0));
    FlipBuffer flip; DirectMode mode; int pans = 0;
    Harness()
    {
        for (int i = 0; i < 2; ++i) {
            pixels[i].resize(W * H, 0x12345678);
            pages[i] = QImage(reinterpret_cast<uchar *>(pixels[i].data()), W, H, W * 4, QImage::Format_RGB32);
        }
        CHECK(flip.initialize(pages[0], pages[1], 0));
    }
    DirectDecision render(DirectCandidate c, QRegion damage, const QImage &reference, bool panOk = true)
    {
        const auto decision = mode.decide(c);
        if (decision.direct) { if (decision.fullSource) damage = bounds; }
        else {
            if (decision.fullComposite) damage = bounds;
            QPainter p(&composite); p.setCompositionMode(QPainter::CompositionMode_Source);
            for (const auto &r : damage) p.drawImage(r, reference, r);
            CHECK(p.end());
        }
        const auto result = flip.present(decision.direct ? c.source : composite, damage,
            [&](int) { ++pans; return panOk; });
        if (!panOk) { CHECK(result == FlipBuffer::PanFailed); return decision; }
        CHECK(result == (damage.isEmpty() ? FlipBuffer::NoChange : FlipBuffer::Submitted));
        mode.commit(decision);
        CHECK(equal(flip.frontImage(), reference));
        return decision;
    }
};
static void transitions()
{
    Harness h; QImage src = image(qRgb(50, 80, 110));
    { auto c = candidate(src); c.enabled = false; CHECK(!h.render(c, bounds, src).direct); }
    CHECK(h.render(candidate(src), QRect(1, 1, 1, 1), src).fullSource);
    fill(src, QRect(4, 3, 9, 8), qRgb(250, 20, 30));
    CHECK(!h.render(candidate(src), QRect(4, 3, 9, 8), src).fullSource);
    fill(src, QRect(50, 30, 7, 5), qRgb(10, 240, 40));
    h.render(candidate(src), QRect(50, 30, 7, 5), src);
    // Composite is deliberately stale during direct rendering. A small dialog
    // dirty region MUST expand to a full recompose on the first fallback.
    QImage dialog = src.copy(); fill(dialog, QRect(25, 13, 20, 18), qRgb(240, 210, 60));
    { auto c = candidate(src); c.windows = 2; const auto d = h.render(c, QRect(25, 13, 20, 18), dialog);
      CHECK(!d.direct && d.fullComposite); }
    QImage moved = src.copy(); fill(moved, QRect(29, 15, 20, 18), qRgb(240, 210, 60));
    { auto c = candidate(src); c.windows = 2; const auto d = h.render(c,
        QRegion(QRect(25, 13, 20, 18)) + QRect(29, 15, 20, 18), moved); CHECK(!d.fullComposite); }
    CHECK(h.render(candidate(src), QRect(29, 15, 20, 18), src).fullSource);
    QImage cursor = src.copy(); fill(cursor, QRect(8, 7, 3, 4), qRgb(0, 0, 0));
    { auto c = candidate(src); c.cursorCanDraw = true; CHECK(h.render(c, QRect(8, 7, 3, 4), cursor).fullComposite); }
    CHECK(h.render(candidate(src), QRect(8, 7, 3, 4), src).fullSource);
    // A new backing-store identity must never inherit the old complete frame.
    { auto c = candidate(src); c.storeToken = &otherToken; CHECK(h.render(c, QRect(0, 0, 1, 1), src).fullSource); }
    CHECK(h.render(candidate(src), QRegion(), src).fullSource);
    const int before = h.pans; h.render(candidate(src), QRegion(), src); CHECK(h.pans == before);
    // Alpha, masks, geometry, DPR, child windows and dialogs all retain the
    // original full-compositor fallback, then a complete direct re-entry.
    for (int reason = 0; reason < 7; ++reason) {
        auto c = candidate(src);
        if (reason == 0) c.opacity = .5;
        if (reason == 1) c.maskEmpty = false;
        if (reason == 2) c.window.translate(1, 0);
        if (reason == 3) c.windowDpr = 2;
        if (reason == 4) c.nativeChildren = true;
        if (reason == 5) c.ordinaryWindow = false;
        if (reason == 6) c.complete = false;
        CHECK(h.render(c, QRect(2, 2, 1, 1), src).fullComposite);
        CHECK(h.render(candidate(src), QRect(2, 2, 1, 1), src).fullSource);
    }
    fill(src, QRect(70, 2, 2, 2), qRgb(80, 90, 100));
    h.render(candidate(src), QRect(70, 2, 2, 2), src, false);
    CHECK(!h.flip.healthy() && h.flip.front() == -1);
    CHECK(h.mode.direct()); // failed PAN never commits a speculative mode.
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try { gates(); coverage(); transitions(); }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "direct-single host gates/coverage/pixels/transitions: " << checks << " checks passed\n";
    return 0;
}
