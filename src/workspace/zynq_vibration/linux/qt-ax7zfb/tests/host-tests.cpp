// SPDX-License-Identifier: LGPL-3.0-only
#include "ax7zflipbuffer.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <vector>
#include <stdexcept>
#include <random>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #condition); } while (0)

using Ax7zFb::FlipBuffer;
constexpr int W = 80, H = 48;
using Pixels = std::vector<quint32>;

static QImage view(Pixels &p) { return QImage(reinterpret_cast<uchar *>(p.data()), W, H, W * 4, QImage::Format_RGB32); }
static void paint(QImage &image, const QRegion &region, QRgb value)
{
    QPainter p(&image);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect &r : region) p.fillRect(r, value);
    CHECK(p.end());
}
static quint64 area(const QRegion &r)
{
    quint64 a = 0;
    for (const QRect &rect : r) a += quint64(rect.width()) * rect.height();
    return a;
}
static bool same(const Pixels &p, const QImage &image)
{
    return std::memcmp(p.data(), image.constBits(), W * H * 4) == 0;
}

static void runDamage(int initialFront)
{
    Pixels pages[2] = {Pixels(W * H, 0xffab12cd), Pixels(W * H, 0xff123456)};
    const Pixels initial[2] = {pages[0], pages[1]};
    QImage source(W, H, QImage::Format_RGB32);
    source.fill(0xff2a3b4c);
    FlipBuffer buffer;
    CHECK(buffer.initialize(view(pages[0]), view(pages[1]), initialFront));
    CHECK(buffer.front() == initialFront);
    CHECK(pages[0] == initial[0] && pages[1] == initial[1]); // init never writes front
    int hardwareFront = initialFront, pans = 0;
    QRegion debts[2] = {QRegion(source.rect()), QRegion(source.rect())};
    auto submit = [&](const QRegion &damage) {
        const QRegion clipped = damage.intersected(source.rect());
        const int beforeFront = hardwareFront;
        const Pixels before = pages[beforeFront];
        debts[0] += clipped; debts[1] += clipped;
        FlipBuffer::Work work;
        const auto result = buffer.present(source, damage, [&](int page) {
            CHECK(page == 1 - hardwareFront);
            CHECK(pages[beforeFront] == before); // all writes target only the back
            CHECK(same(pages[page], source)); // complete consistent page before PAN
            hardwareFront = page;
            ++pans;
            return true;
        }, &work);
        if (clipped.isEmpty()) {
            CHECK(result == FlipBuffer::NoChange && hardwareFront == beforeFront);
            CHECK(work.page == -1 && work.copied.isEmpty());
        } else {
            CHECK(result == FlipBuffer::Submitted && work.page == hardwareFront);
            CHECK(work.copied == debts[hardwareFront]); // exact region; no full fallback
            debts[hardwareFront] = QRegion();
            CHECK(buffer.front() == hardwareFront);
            CHECK(buffer.frontImage().constBits() == reinterpret_cast<const uchar *>(pages[hardwareFront].data()));
            CHECK(buffer.frontImage() == source); // screenshot references committed page
        }
        return work;
    };
    CHECK(submit(QRect(1, 1, 2, 3)).copied == QRegion(source.rect()));
    CHECK(submit(QRect(15, 7, 3, 3)).copied == QRegion(source.rect()));
    const QRegion d1(QRect(3, 3, 2, 2)), d2(QRect(40, 30, 2, 2));
    paint(source, d1, 0xffaabbcc);
    const auto firstSparse = submit(d1);
    CHECK(area(firstSparse.copied) < W * H);
    paint(source, d2, 0xffeeddcc);
    const auto secondSparse = submit(d2);
    CHECK(secondSparse.copied == (d1 + d2)); // debt retained on alternate page
    CHECK(area(secondSparse.copied) == 8);
    submit(QRegion());
    submit(QRect(-20, -20, 2, 2));

    std::mt19937 random(0x7a020 + initialFront);
    for (int i = 0; i < 1600; ++i) {
        QRegion damage;
        const int count = 1 + int(random() % 5);
        for (int j = 0; j < count; ++j)
            damage += QRect(int(random() % (W + 16)) - 8,
                            int(random() % (H + 16)) - 8,
                            1 + int(random() % 12), 1 + int(random() % 12));
        paint(source, damage, 0xff000000 | (random() & 0xffffff));
        submit(damage);
    }
    CHECK(pans > 1400);
    // A failed PAN might have switched hardware; the last fb var is unreliable.
    const int known = buffer.front();
    auto failure = buffer.present(source, QRect(2, 2, 3, 3), [&](int page) {
        CHECK(page == 1 - known); hardwareFront = page; return false;
    });
    CHECK(failure == FlipBuffer::PanFailed && !buffer.healthy() && buffer.front() == -1);
    const Pixels stopped[2] = {pages[0], pages[1]};
    paint(source, source.rect(), 0xff223344);
    int extraPans = 0;
    for (int i = 0; i < 20; ++i)
        CHECK(buffer.present(source, source.rect(), [&](int) { ++extraPans; return true; }) == FlipBuffer::Refused);
    CHECK(extraPans == 0 && pages[0] == stopped[0] && pages[1] == stopped[1]);
}

static void modeChecks()
{
    fb_fix_screeninfo f = {};
    fb_var_screeninfo v = {};
    std::strcpy(f.id, "AX7Z020-720p");
    f.type = FB_TYPE_PACKED_PIXELS; f.visual = FB_VISUAL_TRUECOLOR;
    f.line_length = Ax7zFb::Stride; f.smem_len = Ax7zFb::BufferBytes;
    f.ypanstep = Ax7zFb::Height;
    v.xres = v.xres_virtual = Ax7zFb::Width;
    v.yres = Ax7zFb::Height; v.yres_virtual = Ax7zFb::Height * 2;
    v.bits_per_pixel = 32;
    v.red = {16, 8, 0}; v.green = {8, 8, 0}; v.blue = {0, 8, 0}; v.transp = {24, 0, 0};
    v.pixclock = 13468; v.left_margin = 220; v.right_margin = 110;
    v.upper_margin = 20; v.lower_margin = 5; v.hsync_len = 40; v.vsync_len = 5;
    v.sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT;
    CHECK(Ax7zFb::validateMode(f, v) == nullptr);
    v.yoffset = 720;
    CHECK(Ax7zFb::validateMode(f, v) == nullptr);
    const fb_var_screeninfo valid = v;
    const fb_fix_screeninfo validFixed = f;
#define BAD_VAR(field, value) v = valid; v.field = value; CHECK(Ax7zFb::validateMode(f, v) != nullptr)
    BAD_VAR(xres, 1279); BAD_VAR(yres, 721); BAD_VAR(xres_virtual, 2560);
    BAD_VAR(yres_virtual, 720); BAD_VAR(bits_per_pixel, 24); BAD_VAR(xoffset, 1);
    BAD_VAR(yoffset, 1); BAD_VAR(vmode, FB_VMODE_YWRAP); BAD_VAR(vmode, FB_VMODE_INTERLACED);
    BAD_VAR(rotate, 1); BAD_VAR(grayscale, 1); BAD_VAR(nonstd, 1);
    BAD_VAR(red.offset, 0); BAD_VAR(green.msb_right, 1); BAD_VAR(transp.length, 8);
    BAD_VAR(pixclock, 13467); BAD_VAR(sync, 0); BAD_VAR(vsync_len, 6);
    v = valid;
#define BAD_FIX(field, value) f = validFixed; f.field = value; CHECK(Ax7zFb::validateMode(f, v) != nullptr)
    BAD_FIX(line_length, 5116); BAD_FIX(smem_len, Ax7zFb::BufferBytes - 4);
    BAD_FIX(smem_len, Ax7zFb::BufferBytes + 4); BAD_FIX(ypanstep, 1);
    BAD_FIX(ypanstep, 0); BAD_FIX(xpanstep, 1); BAD_FIX(ywrapstep, 720);
    BAD_FIX(type, FB_TYPE_PLANES); BAD_FIX(visual, FB_VISUAL_PSEUDOCOLOR);
    f = validFixed; f.id[0] = 'X'; CHECK(Ax7zFb::validateMode(f, v) != nullptr);

    Pixels p(W * H), other(W * H);
    FlipBuffer buffer;
    CHECK(!buffer.initialize(view(p), view(other), -1));
    CHECK(!buffer.initialize(view(p), view(other), 2));
    CHECK(!buffer.initialize(view(p), view(p), 0));
    CHECK(!buffer.initialize(QImage(), view(other), 0));
    CHECK(!buffer.initialize(QImage(W, H, QImage::Format_ARGB32), view(other), 0));
    CHECK(!buffer.initialize(QImage(W + 1, H, QImage::Format_RGB32), view(other), 0));
    CHECK(buffer.present(view(p), QRegion(QRect(0, 0, W, H)), [](int) { return true; }) == FlipBuffer::Refused);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        modeChecks(); runDamage(0); runDamage(1);
        std::cout << "PASS: strict mode guards, initial fronts 0/1, both initial full debts, exact sparse two-page damage debt, 3200 randomized updates, front-only screenshots, no writes after ambiguous PAN failure\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
