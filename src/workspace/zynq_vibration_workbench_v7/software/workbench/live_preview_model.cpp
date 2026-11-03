#include "live_preview_model.hpp"
#include "stream_preview_contract.hpp"
#include "timing_stats.hpp"
#include "../acquisition/canonical_base64.hpp"
#include <QByteArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <future>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vibration::workbench {
namespace {
const QColor colors[]{QColor("#3769c7"), QColor("#268878"), QColor("#b78032")};
quint64 integer(const QJsonObject& object, const char* key, quint64 maximum = 4294967295ULL) {
    const auto value = object.value(QLatin1String(key)); const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || std::floor(number) != number || number > double(maximum))
        throw std::runtime_error(std::string("Invalid integer: ") + key);
    return quint64(number);
}
QString string(const QJsonObject& object, const char* key, int maximum = 128) {
    const auto value = object.value(QLatin1String(key));
    const auto text = value.toString();
    if (!value.isString() || text.isEmpty() || text.size() > maximum)
        throw std::runtime_error(std::string("Invalid string: ") + key);
    return text;
}
bool boolean(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isBool()) throw std::runtime_error(std::string("Invalid boolean: ") + key);
    return value.toBool();
}
quint16 u16(const unsigned char* p) { return quint16(p[0] | (quint16(p[1]) << 8)); }
quint32 u32(const unsigned char* p) { return quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 | quint32(p[3]) << 24; }
quint64 u64(const unsigned char* p) { return u32(p) | quint64(u32(p + 4)) << 32; }
std::int16_t signed16(const unsigned char* p) { const auto n = u16(p); return std::int16_t(n < 32768 ? int(n) : int(n) - 65536); }
struct Identity {
    QString job, generation, source;
    quint32 rate = 0, clock = 0, ppr = 0, range = 0, map = 0, sourceFlags = 0;
    quint64 requested = 0;
    bool hardware = false, synthetic = false, streaming = false;
    bool operator==(const Identity& other) const {
        return job == other.job && generation == other.generation && source == other.source && rate == other.rate &&
            clock == other.clock && ppr == other.ppr && range == other.range && map == other.map && sourceFlags == other.sourceFlags &&
            requested == other.requested && hardware == other.hardware && synthetic == other.synthetic && streaming == other.streaming;
    }
};
Identity identity(const QJsonObject& frame) {
    if (frame.value("schema") != QJsonValue("vibration.capture.v2") || frame.value("type") != QJsonValue("event") || frame.value("event") != QJsonValue("preview"))
        throw std::runtime_error("Invalid preview envelope");
    Identity id; id.job = string(frame, "job_id"); id.generation = string(frame, "generation", 20); id.source = string(frame, "source", 32);
    bool generationOk = false; const auto generation = id.generation.toULongLong(&generationOk);
    if (!generationOk || !generation || !QRegularExpression("^[1-9][0-9]{0,19}$").match(id.generation).hasMatch()) throw std::runtime_error("Invalid generation");
    id.rate = quint32(integer(frame, "sample_rate_hz")); id.clock = quint32(integer(frame, "clock_hz"));
    id.ppr = quint32(integer(frame, "ppr", 4096)); id.range = quint32(integer(frame, "adc_range_uv")); id.map = quint32(integer(frame, "channel_map"));
    id.sourceFlags = quint32(integer(frame, "source_flags")); id.requested = quint32(integer(frame, "requested_records", 2097024));
    id.hardware = boolean(frame, "hardware_confirmed"); id.synthetic = boolean(frame, "synthetic");
    if ((id.rate != 50000 && id.rate != 25000 && id.rate != 10000 && id.rate != 5000) || id.clock != 50000000 || !id.ppr ||
        id.range != 5000000 || id.map != 0x00050301 || integer(frame, "record_bytes") != 32 || !id.requested)
        throw std::runtime_error("Unsupported preview metadata");
    if (!((id.source == "adc_unconfirmed" && id.hardware && !id.synthetic && id.sourceFlags == 0) ||
          (id.source == "hardware_test" && id.hardware && id.synthetic && id.sourceFlags == 1) ||
          (id.source == "test_fixture" && !id.hardware && id.synthetic && id.sourceFlags == 1)))
        throw std::runtime_error("Inconsistent preview source declaration");
    return id;
}
Identity identity(const stream_contract::Preview& p){
    Identity id;id.job=p.job;id.generation=p.session;id.source=p.source;id.rate=p.rate;id.clock=50000000;id.ppr=p.ppr;id.range=5000000;id.map=0x50301;
    id.sourceFlags=p.synthetic?1:0;id.requested=p.total;id.hardware=p.hardware;id.synthetic=p.synthetic;id.streaming=true;return id;
}
}

struct LivePreviewModel::Impl {
    struct Sample { Record record; bool gap = false; };
    struct TaskResult { LivePreviewSpectrum spectrum; quint64 epoch = 0, segment = 0, demandEpoch = 0; qint64 workerNs = 0; };
    std::deque<Sample> ring;
    std::optional<Identity> id;
    LivePreviewInfo info;
    LivePreviewSpectrum spectrum;
    std::future<TaskResult> task;
    std::chrono::steady_clock::time_point lastSubmission{};
    quint64 epoch = 1, segment = 0, frameSequence = 0, expectedIndex = 0, anchor = 0, lastSpectrumInput = 0;
    bool haveAnchor = false;
    const bool demandAnalysis = qgetenv("AX7_LIVE_DEMAND_ANALYSIS") == "1";
    bool spectrumDemand = false;
    quint64 demandEpoch = 1;
    quint64 fftReaped = 0, fftDiscarded = 0;
    const bool profiling = qEnvironmentVariableIntValue("AX7_CAPTURE_PROFILE") != 0;
    TimingSamples acceptTotal, metadata, base64, crc, recordValidation, ringUpdate;
    TimingSamples viewTotal, viewVoltage, viewSpeed, viewBoth, viewEmpty;
    TimingSamples viewPrepare, viewSeek, viewFill;
    TimingSamples pollTotal, fftSnapshot, fftLaunch, fftWorker;

    bool reject(const QString& message, QString* error, bool quarantine = false) {
        ++info.rejectedFrames; info.error = message;
        if (quarantine) { info.quarantined = true; ++epoch; spectrum = {}; spectrum.error = message; }
        if (error) *error = message;
        return false;
    }
};

LivePreviewModel::LivePreviewModel() : impl_(std::make_unique<Impl>()) {}
LivePreviewModel::~LivePreviewModel() = default;

void LivePreviewModel::reset() {
    auto& d = *impl_; ++d.epoch; d.segment = 0; d.ring.clear(); d.id.reset(); d.info = {}; d.spectrum = {};
    d.frameSequence = d.expectedIndex = d.anchor = d.lastSpectrumInput = 0; d.haveAnchor = false; d.lastSubmission = {};
    if (d.demandAnalysis) { d.spectrumDemand = false; ++d.demandEpoch; }
    // A bounded old task is allowed to finish; its epoch cannot publish into a
    // new job. Replacing a running std::future here would block the GUI.
}
bool LivePreviewModel::begin(const QJsonObject& frame, bool confirmed, QString* error) {
    reset(); auto& d = *impl_; d.info.active = true; d.info.pprConfirmed = confirmed;
    if (!accept(frame, error)) { d.info.active = false; return false; }
    return true;
}
bool LivePreviewModel::accept(const QJsonObject& frame, QString* error) {
    auto& d = *impl_; ScopedTiming total(d.acceptTotal, d.profiling); TimingLap phase(d.profiling);
    if (!d.info.active) return d.reject(QStringLiteral("实时预览尚未开始"), error);
    if (d.info.quarantined) return d.reject(QStringLiteral("预览信源已隔离；完整采集由采集服务独立处理"), error);
    try {
        std::optional<stream_contract::Preview> stream;
        if(frame.value("schema")=="vibration.stream.v1")stream=stream_contract::decode(frame);
        const auto id = stream?identity(*stream):identity(frame);
        if (d.id && !(*d.id == id)) return d.reject(QStringLiteral("预览代际、信源或采样元数据变化，当前任务预览已隔离"), error, true);
        const auto sequence = stream?stream->sequence:integer(frame, "frame_seq", 9007199254740991ULL);
        const quint64 first = stream?stream->first:integer(frame, "first_record"), count = stream?stream->count:integer(frame, "record_count", 4096);
        const auto committed = stream?first+count:integer(frame, "committed_records"), dropped = stream?0:integer(frame, "dropped_preview_records");
        const auto validity = stream?27:integer(frame, "validity_flags");
        if (!sequence || sequence <= d.frameSequence || !count || first + count > committed || ((!stream||id.requested)&&committed > id.requested) ||
            (!stream&&((validity & 27u) != 27u || (validity & ~31u) || ((validity & 4u) && committed != id.requested))) ||
            (d.frameSequence && first < d.expectedIndex)) return d.reject(QStringLiteral("预览序号、范围或有效标志不一致"), error);
        phase.mark(d.metadata);
        QByteArray bytes=stream?stream->raw:QByteArray();
        if (!stream&&!transport::decodeCanonicalBase64(string(frame, "raw_base64", 174764), int(count * 32), bytes))
            return d.reject(QStringLiteral("预览载荷长度、编码或 CRC 校验失败"), error);
        phase.mark(d.base64);
        if (!stream&&crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size())) != integer(frame, "crc32"))
            return d.reject(QStringLiteral("预览载荷长度、编码或 CRC 校验失败"), error);
        phase.mark(d.crc);
        std::vector<Record> records; records.reserve(std::size_t(count));
        const auto step = quint64(id.clock / id.rate); quint64 candidateAnchor = 0;
        for (quint64 i = 0; i < count; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(bytes.constData()) + i * 32;
            Record r; r.index = u64(p); r.tick = u64(p + 8); r.raw = {signed16(p + 16), signed16(p + 18), signed16(p + 20)};
            r.flags = u16(p + 22); r.speed_period_ticks = u32(p + 24); r.speed_edge_count = u32(p + 28);
            if (r.index != first + i || !(r.flags & 1u) || (r.flags & ~31u) ||
                ((r.flags & 2u) && (!r.speed_period_ticks || (r.flags & 4u))) ||
                r.index > std::numeric_limits<quint64>::max() / step || r.tick < r.index * step)
                return d.reject(QStringLiteral("预览原始记录的序号或标志无效"), error);
            const auto recordAnchor = r.tick - r.index * step;
            if ((!i && d.haveAnchor && recordAnchor != d.anchor) || (i && recordAnchor != candidateAnchor))
                return d.reject(QStringLiteral("预览原始时间戳不连续"), error);
            candidateAnchor = recordAnchor; records.push_back(r);
        }
        phase.mark(d.recordValidation);
        if (!d.id) {
            d.id = id; d.info.jobId = id.job; d.info.generation = id.generation; d.info.source = id.source;
            d.info.sampleRate = id.rate; d.info.clockHz = id.clock; d.info.ppr = id.ppr; d.info.adcRangeUv = qint32(id.range);
            d.info.synthetic = id.synthetic; d.info.hardwareConfirmed = id.hardware; d.info.maximumRecords = std::size_t(id.rate) * 2;
        }
        const quint64 observedGap = first >= d.expectedIndex ? first - d.expectedIndex : 0;
        const quint64 gap = std::max(observedGap, dropped);
        if (gap) { ++d.segment; d.spectrum = {}; d.info.previewDroppedRecords += gap; }
        for (std::size_t i = 0; i < records.size(); ++i) d.ring.push_back({records[i], i == 0 && gap != 0});
        d.anchor = candidateAnchor; d.haveAnchor = true; d.frameSequence = sequence; d.expectedIndex = first + count;
        while (!d.ring.empty() && (d.ring.size() > d.info.maximumRecords ||
            d.ring.back().record.tick - d.ring.front().record.tick >= quint64(id.clock) * 2)) d.ring.pop_front();
        ++d.info.acceptedFrames; d.info.acceptedRecords += count; ++d.info.revision; d.info.retainedRecords = d.ring.size(); d.info.error.clear();
        phase.mark(d.ringUpdate);
        pollSpectrum(); return true;
    } catch (const std::exception& exception) { return d.reject(QString::fromUtf8(exception.what()), error); }
}
void LivePreviewModel::end(const QString& reason) { auto& d = *impl_; d.info.active = false; d.info.error = reason; ++d.epoch; setSpectrumDemand(false); }
bool LivePreviewModel::isLive() const { return impl_->info.active; }
LivePreviewInfo LivePreviewModel::info() const { return impl_->info; }
LivePreviewSpectrum LivePreviewModel::spectrum() const { return impl_->spectrum; }

void LivePreviewModel::setSpectrumDemand(bool wanted) {
    auto& d = *impl_;
    if (!d.demandAnalysis || d.spectrumDemand == wanted) return;
    d.spectrumDemand = wanted; ++d.demandEpoch;
    if (wanted) d.spectrum = {};
    // A new visible session must sample the latest continuous raw segment,
    // rather than showing an old hidden job or waiting out its rate limit.
    d.lastSubmission = {}; d.lastSpectrumInput = 0;
}

LivePreviewView LivePreviewModel::view(double seconds, bool voltage, bool speedTrace) const {
    auto& d = *impl_; ScopedTiming totalTiming(d.viewTotal, d.profiling);
    ScopedTiming kind(voltage ? (speedTrace ? d.viewBoth : d.viewVoltage) : (speedTrace ? d.viewSpeed : d.viewEmpty), d.profiling);
    TimingLap phase(d.profiling);
    LivePreviewView out; if (!d.id || d.ring.empty()) return out;
    seconds = std::clamp(std::isfinite(seconds) ? seconds : .25, .02, 2.0);
    out.endTime = std::max(1.0 / d.id->rate, double(d.ring.back().record.index) / d.id->rate);
    out.startTime = std::max(0.0, out.endTime - seconds); out.rpm = d.info.pprConfirmed; out.revision = d.info.revision;
    if (voltage) for (int channel = 0; channel < 3; ++channel) out.voltage.push_back({QStringLiteral("CH%1").arg(channel + 1), colors[channel], {}});
    out.speed = {out.rpm ? QStringLiteral("RPM") : QStringLiteral("Hz"), colors[0], {}};
    if (!voltage && !speedTrace) return out;
    const int reserve = int(std::min(d.ring.size(), std::size_t(std::ceil(seconds * d.id->rate)) + 32));
    for (auto& series : out.voltage) series.points.reserve(reserve);
    if (speedTrace) out.speed.points.reserve(reserve);
    phase.mark(d.viewPrepare);
    std::size_t valid = 0, total = 0; double sum = 0, low = std::numeric_limits<double>::infinity(), high = -low;
    const double voltageScale = double(d.id->range) / 1e6 / 32768.0, nan = std::numeric_limits<double>::quiet_NaN();
    const auto firstIndex = quint64(std::max(0.0, std::ceil(out.startTime * d.id->rate - 1e-7)));
    const auto begin = std::lower_bound(d.ring.begin(), d.ring.end(), firstIndex,
        [](const auto& sample, quint64 index) { return sample.record.index < index; });
    phase.mark(d.viewSeek);
    for (auto iterator = begin; iterator != d.ring.end(); ++iterator) {
        const auto& sample = *iterator;
        const auto& r = sample.record; const double time = double(r.index) / d.id->rate; if (time < out.startTime) continue;
        if (sample.gap) {
            const double gapTime = std::max(out.startTime, time - .5 / d.id->rate);
            for (auto& series : out.voltage) series.points.push_back({gapTime, nan});
            if (speedTrace) out.speed.points.push_back({gapTime, nan});
        }
        if (voltage) for (int channel = 0; channel < 3; ++channel) out.voltage[channel].points.push_back({time, r.raw[channel] * voltageScale});
        if (!speedTrace) continue;
        ++total; const bool good = (r.flags & 2u) && !(r.flags & 4u) && r.speed_period_ticks;
        double speed = nan;
        if (good) { speed = double(d.id->clock) / r.speed_period_ticks * (out.rpm ? 60.0 / d.id->ppr : 1.0); ++valid; sum += speed; low = std::min(low, speed); high = std::max(high, speed); }
        out.speed.points.push_back({time, speed});
    }
    out.validSpeedFraction = total ? double(valid) / total : 0;
    if (valid) { out.meanSpeed = sum / valid; out.minimumSpeed = low; out.maximumSpeed = high; }
    phase.mark(d.viewFill);
    return out;
}


void LivePreviewModel::pollSpectrum() {
    auto& d = *impl_; ScopedTiming total(d.pollTotal, d.profiling);
    if (d.task.valid()) {
        if (d.task.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto result = d.task.get();
        ++d.fftReaped;
        if (d.profiling) d.fftWorker.add(result.workerNs);
        if (d.info.active && !d.info.quarantined && result.epoch == d.epoch && result.segment == d.segment &&
            (!d.demandAnalysis || (d.spectrumDemand && result.demandEpoch == d.demandEpoch))) {
            result.spectrum.revision = ++d.info.fftCompleted; d.spectrum = std::move(result.spectrum);
        } else ++d.fftDiscarded;
    }
    const auto now = std::chrono::steady_clock::now();
    if ((d.demandAnalysis && !d.spectrumDemand) || !d.info.active || d.info.quarantined || !d.id || d.ring.size() < 16 || d.info.revision == d.lastSpectrumInput ||
        (d.lastSubmission.time_since_epoch().count() && now - d.lastSubmission < std::chrono::milliseconds(200))) return;
    TimingLap phase(d.profiling);
    std::vector<Record> snapshot; snapshot.reserve(8192);
    for (auto it = d.ring.rbegin(); it != d.ring.rend() && snapshot.size() < 8192; ++it) {
        snapshot.push_back(it->record); if (it->gap) break;
    }
    if (snapshot.size() < 16) return;
    std::reverse(snapshot.begin(), snapshot.end());
    const auto id = *d.id; const auto epoch = d.epoch, segment = d.segment, demandEpoch = d.demandEpoch, inputRevision = d.info.revision;
    phase.mark(d.fftSnapshot);
    d.lastSubmission = now; d.lastSpectrumInput = d.info.revision; ++d.info.fftSubmitted;
    d.task = std::async(std::launch::async, [snapshot = std::move(snapshot), id, epoch, segment, demandEpoch, inputRevision, profiling = d.profiling]() mutable {
        Impl::TaskResult result; result.epoch = epoch; result.segment = segment; result.demandEpoch = demandEpoch;
        result.spectrum.inputRevision = inputRevision;
        QElapsedTimer elapsed; if (profiling) elapsed.start();
        try {
            result.spectrum.channels = analyze_voltage_spectra(snapshot, id.rate, id.clock, qint32(id.range), 8192, 2000.0);
            result.spectrum.startTime = double(snapshot.front().index) / id.rate; result.spectrum.endTime = double(snapshot.back().index) / id.rate;
            result.spectrum.available = true;
        } catch (const std::exception& error) { result.spectrum.error = QString::fromUtf8(error.what()); }
        if (profiling) result.workerNs = elapsed.nsecsElapsed();
        return result;
    });
    phase.mark(d.fftLaunch);
}
QJsonObject LivePreviewModel::performanceProfile() const {
    const auto& d = *impl_;
    return {{"enabled", d.profiling}, {"demand_analysis", d.demandAnalysis}, {"spectrum_demand", d.spectrumDemand},
        {"spectrum_policy", d.demandAnalysis ? "visible_following_only" : "automatic_5hz"}, {"fft_pending", d.task.valid()},
        {"fft_submitted", double(d.info.fftSubmitted)}, {"fft_completed", double(d.info.fftCompleted)},
        {"lifetime_fft_reaped", double(d.fftReaped)}, {"lifetime_fft_discarded", double(d.fftDiscarded)},
        {"raw_revision", double(d.info.revision)}, {"spectrum_input_revision", double(d.spectrum.inputRevision)},
        {"scope", "lifetime wall timings; nested stages overlap; recent percentiles bounded to 256 observations"},
        {"base64_decoder", "strict_single_pass_utf16; base64_and_canonical includes field materialization and decode"},
        {"accept_total", d.acceptTotal.json()}, {"metadata", d.metadata.json()}, {"base64_and_canonical", d.base64.json()},
        {"crc", d.crc.json()}, {"record_validation", d.recordValidation.json()}, {"ring_update", d.ringUpdate.json()},
        {"view_total", d.viewTotal.json()}, {"view_voltage", d.viewVoltage.json()}, {"view_speed", d.viewSpeed.json()},
        {"view_both", d.viewBoth.json()}, {"view_empty", d.viewEmpty.json()}, {"poll_total", d.pollTotal.json()},
        {"view_point_buffers", "reserved_push_back_original; presized experiment reverted after ARM regression"},
        {"view_prepare", d.viewPrepare.json()}, {"view_seek", d.viewSeek.json()}, {"view_fill", d.viewFill.json()},
        {"fft_snapshot", d.fftSnapshot.json()}, {"fft_launch", d.fftLaunch.json()}, {"fft_worker", d.fftWorker.json()}};
}
} // namespace vibration::workbench
