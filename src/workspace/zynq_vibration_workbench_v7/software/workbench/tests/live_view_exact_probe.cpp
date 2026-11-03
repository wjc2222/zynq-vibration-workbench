#include "live_preview_model.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>

using namespace vibration;
using namespace vibration::workbench;
namespace {
struct Sample { Record record; bool gap = false; };
const QColor colors[]{QColor("#3769c7"), QColor("#268878"), QColor("#b78032")};

// Frozen pre-optimization view materializer. This intentionally retains its
// push_back construction and original expressions as an independent oracle.
LivePreviewView referenceView(const std::deque<Sample>& ring, quint32 rate, quint32 ppr,
                              bool confirmed, quint64 revision, double seconds, bool voltage, bool speedTrace) {
    LivePreviewView out; if (ring.empty()) return out;
    seconds = std::clamp(std::isfinite(seconds) ? seconds : .25, .02, 2.0);
    out.endTime = std::max(1.0 / rate, double(ring.back().record.index) / rate);
    out.startTime = std::max(0.0, out.endTime - seconds); out.rpm = confirmed; out.revision = revision;
    if (voltage) for (int channel = 0; channel < 3; ++channel) out.voltage.push_back({QStringLiteral("CH%1").arg(channel + 1), colors[channel], {}});
    out.speed = {out.rpm ? QStringLiteral("RPM") : QStringLiteral("Hz"), colors[0], {}};
    if (!voltage && !speedTrace) return out;
    const int reserve = int(std::min(ring.size(), std::size_t(std::ceil(seconds * rate)) + 32));
    for (auto& series : out.voltage) series.points.reserve(reserve);
    if (speedTrace) out.speed.points.reserve(reserve);
    std::size_t valid = 0, total = 0; double sum = 0, low = std::numeric_limits<double>::infinity(), high = -low;
    const double voltageScale = double(5000000) / 1e6 / 32768.0, nan = std::numeric_limits<double>::quiet_NaN();
    const auto firstIndex = quint64(std::max(0.0, std::ceil(out.startTime * rate - 1e-7)));
    const auto begin = std::lower_bound(ring.begin(), ring.end(), firstIndex,
        [](const auto& sample, quint64 index) { return sample.record.index < index; });
    for (auto iterator = begin; iterator != ring.end(); ++iterator) {
        const auto& sample = *iterator;
        const auto& r = sample.record; const double time = double(r.index) / rate; if (time < out.startTime) continue;
        if (sample.gap) {
            const double gapTime = std::max(out.startTime, time - .5 / rate);
            for (auto& series : out.voltage) series.points.push_back({gapTime, nan});
            if (speedTrace) out.speed.points.push_back({gapTime, nan});
        }
        if (voltage) for (int channel = 0; channel < 3; ++channel) out.voltage[channel].points.push_back({time, r.raw[channel] * voltageScale});
        if (!speedTrace) continue;
        ++total; const bool good = (r.flags & 2u) && !(r.flags & 4u) && r.speed_period_ticks;
        double speed = nan;
        if (good) { speed = double(50000000) / r.speed_period_ticks * (out.rpm ? 60.0 / ppr : 1.0); ++valid; sum += speed; low = std::min(low, speed); high = std::max(high, speed); }
        out.speed.points.push_back({time, speed});
    }
    out.validSpeedFraction = total ? double(valid) / total : 0;
    if (valid) { out.meanSpeed = sum / valid; out.minimumSpeed = low; out.maximumSpeed = high; }
    return out;
}
bool bits(double a, double b) { return std::memcmp(&a, &b, sizeof(double)) == 0; }
bool bits(const std::optional<double>& a, const std::optional<double>& b) {
    return bool(a) == bool(b) && (!a || bits(*a, *b));
}
bool sameSeries(const PlotSeries& a, const PlotSeries& b, quint64& comparedPoints) {
    if (a.name != b.name || a.color != b.color || a.visible != b.visible || a.points.size() != b.points.size()) return false;
    for (int i = 0; i < a.points.size(); ++i) {
        ++comparedPoints;
        if (!bits(a.points[i].x(), b.points[i].x()) || !bits(a.points[i].y(), b.points[i].y())) return false;
    }
    return true;
}
bool sameView(const LivePreviewView& a, const LivePreviewView& b, quint64& comparedPoints) {
    if (a.rpm != b.rpm || a.revision != b.revision || a.voltage.size() != b.voltage.size() ||
        !bits(a.startTime, b.startTime) || !bits(a.endTime, b.endTime) || !bits(a.validSpeedFraction, b.validSpeedFraction) ||
        !bits(a.meanSpeed, b.meanSpeed) || !bits(a.minimumSpeed, b.minimumSpeed) || !bits(a.maximumSpeed, b.maximumSpeed) ||
        !sameSeries(a.speed, b.speed, comparedPoints)) return false;
    for (int channel = 0; channel < a.voltage.size(); ++channel)
        if (!sameSeries(a.voltage[channel], b.voltage[channel], comparedPoints)) return false;
    return true;
}
void put(QByteArray& bytes, int at, quint64 value, int count) {
    for (int i = 0; i < count; ++i) bytes[at + i] = char(value >> (8 * i));
}
Record sample(quint64 index, quint32 rate) {
    Record r; r.index = index; r.tick = 123 + index * (50000000 / rate);
    for (int channel = 0; channel < 3; ++channel) {
        const quint16 value = quint16(index * 1103515245ULL + channel * 32767 + 12345);
        r.raw[channel] = std::int16_t(value < 32768 ? int(value) : int(value) - 65536);
    }
    r.flags = index % 5 < 3 ? 3 : index % 5 == 3 ? 5 : 1;
    r.speed_period_ticks = r.flags == 3 ? quint32(123456 + index % 3000) : 0;
    r.speed_edge_count = quint32(index % 1234); return r;
}
QJsonObject frame(quint64 first, int count, quint64 sequence, quint32 rate, quint32 ppr) {
    QByteArray raw(count * 32, 0);
    for (int i = 0; i < count; ++i) {
        const auto r = sample(first + i, rate); const int at = i * 32;
        put(raw, at, r.index, 8); put(raw, at + 8, r.tick, 8);
        for (int channel = 0; channel < 3; ++channel) put(raw, at + 16 + channel * 2, quint16(r.raw[channel]), 2);
        put(raw, at + 22, r.flags, 2); put(raw, at + 24, r.speed_period_ticks, 4); put(raw, at + 28, r.speed_edge_count, 4);
    }
    return {{"schema", "vibration.capture.v2"}, {"type", "event"}, {"event", "preview"}, {"job_id", "exact_view_fixture"},
        {"generation", "1"}, {"frame_seq", double(sequence)}, {"first_record", double(first)}, {"record_count", count},
        {"committed_records", double(first + count)}, {"requested_records", 2097024}, {"sample_rate_hz", int(rate)},
        {"clock_hz", 50000000}, {"ppr", int(ppr)}, {"adc_range_uv", 5000000}, {"channel_map", 328449}, {"record_bytes", 32},
        {"source", "test_fixture"}, {"hardware_confirmed", false}, {"synthetic", true}, {"source_flags", 1}, {"validity_flags", 27},
        {"raw_base64", QString::fromLatin1(raw.toBase64())}, {"dropped_preview_records", 0},
        {"crc32", double(crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()), std::size_t(raw.size())))}};
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); qputenv("AX7_LIVE_DEMAND_ANALYSIS", "1");
    bool passed = true; quint64 cases = 0, comparedPoints = 0; QJsonArray failures; QString error;
    for (quint32 rate : {5000u, 10000u, 25000u, 50000u}) for (bool confirmed : {false, true}) {
        LivePreviewModel model; std::deque<Sample> ring; const quint32 ppr = confirmed ? 7 : 3;
        quint64 revision = 0, expected = 0, sequence = 0;
        const auto checkViews = [&](const char* state) {
            const auto infoBefore = model.info();
            for (double seconds : {-.1, 0., .02, .020019, .25, .250013, 1., 2., 9.,
                    std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
                for (bool voltage : {false, true}) for (bool speed : {false, true}) {
                    ++cases;
                    const auto actual = model.view(seconds, voltage, speed);
                    const auto reference = referenceView(ring, rate, ppr, confirmed, revision, seconds, voltage, speed);
                    if (!sameView(actual, reference, comparedPoints)) {
                        passed = false;
                        if (failures.size() < 12) failures.append(QJsonObject{{"state", state}, {"rate", int(rate)}, {"confirmed", confirmed},
                            {"seconds", seconds}, {"voltage", voltage}, {"speed", speed}});
                    }
                }
            const auto after = model.info();
            passed &= infoBefore.acceptedRecords == after.acceptedRecords && infoBefore.previewDroppedRecords == after.previewDroppedRecords &&
                infoBefore.revision == after.revision && infoBefore.retainedRecords == after.retainedRecords;
        };
        const auto accept = [&](quint64 first, int count) {
            const auto event = frame(first, count, ++sequence, rate, ppr);
            const bool ok = sequence == 1 ? model.begin(event, confirmed, &error) : model.accept(event, &error);
            if (!ok) { passed = false; return; }
            for (int i = 0; i < count; ++i) ring.push_back({sample(first + i, rate), i == 0 && first > expected});
            while (!ring.empty() && (ring.size() > rate * 2 || ring.back().record.tick - ring.front().record.tick >= 100000000ULL)) ring.pop_front();
            expected = first + count; ++revision;
        };
        checkViews("empty"); accept(0, 7); checkViews("seven_records");
        accept(expected + 17, 4096); checkViews("gap_in_window");
        while (expected < rate * 3) {
            const int count = int(std::min<quint64>(4096, rate * 3 - expected));
            accept(expected + (sequence % 3 == 0 ? 13 : 0), count);
        }
        checkViews("trimmed_two_second_ring_with_gaps_and_missing_speed");
        model.end(); checkViews("ended_retained_window"); model.reset(); ring.clear(); revision = 0; checkViews("reset");
    }
    QJsonObject report{{"passed", passed}, {"view_cases", double(cases)}, {"bitwise_compared_points", double(comparedPoints)},
        {"comparison", "every QPointF x/y double bit, NaN bit pattern/location, series metadata, time range, revision, RPM flag, valid speed fraction and all optional speed summaries"},
        {"reference", "frozen pre-optimization push_back materializer; independently maintained accepted record ring"},
        {"scope", "synthetic model-only regression; no sensor, scanout or hardware access"}, {"runtime_cpu_architecture", QSysInfo::currentCpuArchitecture()},
        {"failures", failures}, {"error", error}};
    const auto bytes = QJsonDocument(report).toJson();
    if (argc > 1) { QFile file(QString::fromUtf8(argv[1])); if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return 2; }
    std::fwrite(bytes.constData(), 1, std::size_t(bytes.size()), stdout); return passed ? 0 : 1;
}
