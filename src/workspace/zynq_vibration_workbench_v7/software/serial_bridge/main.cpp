// Linux console bridge. Acquisition, its service protocol and raw validation
// remain owned by the existing CaptureClient/Transport and LivePreviewModel.
#include "capture_client.hpp"
#include "live_preview_model.hpp"
#include "raw_window_stats.hpp"
#include <vibration/core.hpp>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <future>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
using vibration::workbench::CaptureClient;
using vibration::workbench::LivePreviewModel;
const QString schema = QStringLiteral("vibration.serial.v1");
const QString recordRoot = QStringLiteral("/tmp/vibration-serial-records");
constexpr int maxWire = 131072, maxDecoded = 2097152, maxRequest = 8192;
constexpr qint64 maxFile = 64LL * 1024 * 1024, maxRootBytes = 256LL * 1024 * 1024;
constexpr int maxRecords = 64, maxReliable = 64, maxQueueBytes = 1048576;
struct Failure : std::runtime_error {
    QString code;
    Failure(QString c, QString m) : std::runtime_error(m.toUtf8().constData()), code(std::move(c)) {}
};
[[noreturn]] void fail(const QString& code, const QString& message) { throw Failure(code, message); }
void require(bool ok, const QString& code, const QString& message) { if (!ok) fail(code, message); }
bool integer(const QJsonValue& value, double lo, double hi) {
    const auto number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= lo && number <= hi && std::floor(number) == number;
}
void keys(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        require(allowed.contains(it.key()), "INVALID_REQUEST", "Unknown field: " + it.key());
}
// QJsonDocument validates syntax before this bounded duplicate/depth pass.
class UniqueKeys {
    const QByteArray& bytes_; int position_ = 0;
    void whitespace() { while (position_ < bytes_.size() && QByteArray(" \t\r\n").contains(bytes_[position_])) ++position_; }
    QString string(bool decode) {
        const int start = position_++;
        while (position_ < bytes_.size()) {
            const int end = bytes_.indexOf('"', position_); if (end < 0) break;
            int slash = 0; for (int i = end - 1; i >= position_ && bytes_[i] == '\\'; --i) ++slash;
            position_ = end + 1; if (!(slash & 1)) break;
        }
        return decode ? QJsonDocument::fromJson("[" + bytes_.mid(start, position_ - start) + "]").array()[0].toString() : QString();
    }
    void value(int depth) {
        require(depth <= 16, "INVALID_JSON", "JSON nesting limit exceeded"); whitespace(); if (position_ >= bytes_.size()) return;
        const char c = bytes_[position_];
        if (c == '{') {
            ++position_; whitespace(); std::set<QString> seen;
            while (position_ < bytes_.size() && bytes_[position_] != '}') {
                whitespace(); require(seen.insert(string(true)).second, "DUPLICATE_KEY", "Duplicate JSON key");
                whitespace(); ++position_; value(depth + 1); whitespace();
                if (position_ < bytes_.size() && bytes_[position_] == ',') { ++position_; continue; } break;
            }
            ++position_;
        } else if (c == '[') {
            ++position_; whitespace();
            while (position_ < bytes_.size() && bytes_[position_] != ']') {
                value(depth + 1); whitespace(); if (position_ < bytes_.size() && bytes_[position_] == ',') { ++position_; continue; } break;
            }
            ++position_;
        } else if (c == '"') string(false);
        else while (position_ < bytes_.size() && !QByteArray(",]} \r\n\t").contains(bytes_[position_])) ++position_;
    }
public:
    explicit UniqueKeys(const QByteArray& b) : bytes_(b) {}
    void check() { value(0); }
};
QJsonObject parse(const QByteArray& bytes) {
    QJsonParseError error; auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "INVALID_JSON", "Expected a UTF-8 JSON object");
    UniqueKeys(bytes).check(); return document.object();
}
bool terminal(const QString& state) { return state == "completed" || state == "cancelled" || state == "failed"; }
QJsonValue numeric(double n) { return std::isfinite(n) ? QJsonValue(n) : QJsonValue(QJsonValue::Null); }

struct Fd {
    int value = -1;
    explicit Fd(int n = -1) : value(n) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete; Fd& operator=(const Fd&) = delete;
};
bool same(const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
        a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
void directorySafe(const QString& path) {
    struct stat s{}; const auto name = QFile::encodeName(path);
    require(::lstat(name.constData(), &s) == 0 && S_ISDIR(s.st_mode) && s.st_uid == ::geteuid() && !(s.st_mode & 0022),
            "UNSAFE_STORAGE", "Managed directory is missing, writable by others, a symlink or owned by another user");
}
struct File {
    QString path, name, kind, sha; std::shared_ptr<Fd> fd; struct stat identity{};
    qint64 bytes() const { return identity.st_size; }
    bool unchanged(bool pathToo = true) const {
        struct stat current{}, pathState{};
        return ::fstat(fd->value, &current) == 0 && same(identity, current) &&
            (!pathToo || (::lstat(QFile::encodeName(path).constData(), &pathState) == 0 && S_ISREG(pathState.st_mode) && same(identity, pathState)));
    }
    QJsonObject json() const { return {{"kind", kind}, {"name", name}, {"bytes", double(bytes())}, {"sha256", sha}}; }
};
File openFile(const QString& path, const QString& kind, qint64 limit) {
    File out; out.path = path; out.name = QFileInfo(path).fileName(); out.kind = kind;
    out.fd = std::make_shared<Fd>(::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    require(out.fd->value >= 0 && ::fstat(out.fd->value, &out.identity) == 0 && S_ISREG(out.identity.st_mode) &&
        out.identity.st_uid == ::geteuid() && out.identity.st_nlink == 1 && out.bytes() > 0 && out.bytes() <= limit,
        "INVALID_RECORD", "Capture pair must contain owned, bounded, ordinary non-symlink files");
    QCryptographicHash hash(QCryptographicHash::Sha256); QByteArray block(65536, '\0'); qint64 offset = 0;
    while (offset < out.bytes()) {
        const auto count = ::pread(out.fd->value, block.data(), std::size_t(std::min(qint64(block.size()), out.bytes() - offset)), offset);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, "FILE_ERROR", "Unable to hash complete capture pair"); hash.addData(block.constData(), int(count)); offset += count;
    }
    require(out.unchanged(), "RECORD_CHANGED", "Capture changed while opening"); out.sha = QString::fromLatin1(hash.result().toHex()); return out;
}
QByteArray readAt(const File& file, qint64 offset, int length) {
    require(file.unchanged(), "RECORD_CHANGED", "Capture pair identity changed"); QByteArray bytes(length, '\0'); int got = 0;
    while (got < length) {
        const auto n = ::pread(file.fd->value, bytes.data() + got, std::size_t(length - got), offset + got);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "FILE_ERROR", "Capture pair read failed"); got += int(n);
    }
    require(file.unchanged(), "RECORD_CHANGED", "Capture pair changed during read"); return bytes;
}
struct Record {
    QString id, job, session; File bin, meta; QJsonObject metadata;
    QJsonObject json() const { return {{"record_id", id}, {"job_id", job}, {"bytes", double(bin.bytes())}, {"synthetic", metadata.value("synthetic")},
        {"hardware_confirmed", metadata.value("hardware_confirmed")}, {"actual_source", metadata.value("actual_source")},
        {"files", QJsonArray{bin.json(), meta.json()}}, {"scope", "complete_record"}}; }
};
std::shared_ptr<Record> validateRecord(const QString& path, const QString& job, vibration::Capture* loaded = nullptr) {
    const QFileInfo fi(path); const QString dir = fi.absolutePath(), session = QFileInfo(dir).fileName(), stem = fi.completeBaseName();
    require(QRegularExpression("^[0-9a-f]{32}$").match(session).hasMatch() && fi.suffix().compare("bin", Qt::CaseInsensitive) == 0 &&
        QRegularExpression("^[A-Za-z0-9_-]{1,160}$").match(stem).hasMatch() && QFileInfo(dir).absolutePath() == recordRoot,
        "INVALID_RECORD", "Capture is outside the managed record root");
    directorySafe(recordRoot); directorySafe(dir);
    auto record = std::make_shared<Record>(); record->id = session + '/' + stem; record->job = job; record->session = session;
    record->bin = openFile(path, "bin", maxFile); record->meta = openFile(dir + '/' + stem + ".meta.json", "meta", 65536);
    record->metadata = parse(readAt(record->meta, 0, int(record->meta.bytes())));
    const auto& meta = record->metadata; const auto actual = meta.value("actual_source").toString();
    require(meta.value("schema") == "vibration.capture.v2" && meta.value("synthetic").isBool() && meta.value("hardware_confirmed").isBool() &&
        ((actual == "host_fixture" && meta.value("synthetic").toBool() && !meta.value("hardware_confirmed").toBool() && meta.value("backend") == "test_fixture") ||
         (actual == "hardware_self_test" && meta.value("synthetic").toBool() && meta.value("hardware_confirmed").toBool() && meta.value("backend") == "linux_device") ||
         (actual == "adc" && !meta.value("synthetic").toBool() && meta.value("hardware_confirmed").toBool() && meta.value("backend") == "linux_device")),
        "INVALID_SOURCE", "Original source sidecar is missing or inconsistent");
    auto capture = vibration::load_capture(path.toUtf8().toStdString());
    require(capture.integrity_verified && capture.source_metadata.status != "invalid_or_unreadable_sidecar" &&
        capture.source_metadata.synthetic == meta.value("synthetic").toBool(), "INVALID_SOURCE", "Core and original sidecar source declarations differ");
    const auto parameters = meta.value("capture_parameters").toObject();
    require(meta.value("payload_crc32").toDouble(-1) == capture.header.payload_crc32 &&
        meta.value("driver_hw_status").toDouble(-1) == capture.header.hw_status && meta.value("driver_dma_status").toDouble(-1) == capture.header.dma_status &&
        parameters.value("sample_rate_hz").toDouble(-1) == capture.header.sample_rate_hz &&
        parameters.value("pulses_per_rev").toDouble(-1) == capture.header.pulses_per_rev &&
        parameters.value("requested_records").toDouble(-1) == capture.header.requested_records &&
        parameters.value("duration_seconds").toDouble(-1) * capture.header.sample_rate_hz == capture.header.requested_records &&
        parameters.value("output_directory").toString() == dir,
        "METADATA_MISMATCH", "Original source sidecar is not bound to the same capture header and acquisition plan");
    require(record->bin.unchanged() && record->meta.unchanged(), "RECORD_CHANGED", "Capture changed during full integrity validation");
    if (loaded) *loaded = std::move(capture);
    return record;
}

// First/min/max/last per bucket. A bucket containing a gap is represented by
// its missing span and boundaries; finite fragments inside it are hidden.
// This deliberately sacrifices fragments rather than inventing continuity.
QJsonArray reducePoints(const QVector<QPointF>& points, int limit) {
    QJsonArray output; if (points.isEmpty()) return output;
    const int buckets = std::max(1, limit / 6), stride = std::max(1, (points.size() + buckets - 1) / buckets);
    for (int begin = 0; begin < points.size(); begin += stride) {
        const int end = std::min(points.size(), begin + stride); int low = begin, high = begin, firstGap = -1, lastGap = -1;
        for (int i = begin; i < end; ++i) {
            if (!std::isfinite(points[i].y())) { if (firstGap < 0) firstGap = i; lastGap = i; }
            else { if (!std::isfinite(points[low].y()) || points[i].y() < points[low].y()) low = i;
                   if (!std::isfinite(points[high].y()) || points[i].y() > points[high].y()) high = i; }
        }
        std::set<int> indices{begin, end - 1};
        if (firstGap >= 0) { indices.insert(firstGap); indices.insert(lastGap); if (firstGap > begin) indices.insert(firstGap - 1); if (lastGap + 1 < end) indices.insert(lastGap + 1); }
        else { indices.insert(low); indices.insert(high); }
        for (const auto i : indices) output.append(QJsonArray{numeric(points[i].x()), numeric(points[i].y())});
    }
    return output;
}
std::vector<int> spectrumIndices(const QJsonArray& values, int limit) {
    std::set<int> keep; const int size = values.size(); if (!size) return {};
    if (size <= limit) { for (int i = 0; i < size; ++i) keep.insert(i); }
    else {
        keep.insert(0); keep.insert(size - 1); const int buckets = std::max(1, (limit - 2) / 2);
        for (int b = 0; b < buckets; ++b) {
            const int first = 1 + (size - 2) * b / buckets, end = 1 + (size - 2) * (b + 1) / buckets; int low = first, high = first;
            for (int i = first + 1; i < end; ++i) { if (values[i].toDouble() < values[low].toDouble()) low = i; if (values[i].toDouble() > values[high].toDouble()) high = i; }
            keep.insert(low); keep.insert(high);
        }
    }
    return {keep.begin(), keep.end()};
}
QJsonObject decimateSpectrum(QJsonObject object, int limit) {
    const auto amplitudes = object.value("peak_amplitude").toArray(); const auto indices = spectrumIndices(amplitudes, limit);
    for (const auto& key : QStringList{"frequency_hz", "peak_amplitude", "psd"}) {
        const auto input = object.value(key).toArray(); if (input.size() != amplitudes.size()) continue;
        QJsonArray reduced; for (auto i : indices) reduced.append(input[i]); object[key] = reduced;
    }
    object["original_bin_count"] = amplitudes.size(); object["display_decimated"] = int(indices.size()) < amplitudes.size();
    object["display_decimation"] = "per_bucket_min_max_peak_preserving"; return object;
}
struct Task {
    QString kind, id, command, job, error, code; quint64 epoch = 0;
    std::shared_ptr<Record> record; std::vector<std::shared_ptr<Record>> records; QJsonObject result; int rejected = 0;
};
Task analyzeTask(const QString& path, const QString& job, const QString& id, quint64 epoch) {
    Task out; out.kind = "analysis"; out.id = id; out.command = "analyze_record"; out.job = job; out.epoch = epoch;
    try {
        vibration::Capture capture; out.record = validateRecord(path, job, &capture);
        vibration::AnalysisConfig config; config.waveform_points = 320; // Complete-record FFT stays at the core default65536.
        const auto analyzed = vibration::analyze(capture, config);
        out.result = parse(QByteArray::fromStdString(vibration::to_json(analyzed)));
        QJsonArray spectra; for (const auto& item : out.result.value("spectra").toArray()) spectra.append(decimateSpectrum(item.toObject(), 256));
        out.result["spectra"] = spectra;
        out.result["display_decimated"] = true; out.result["display_scope"] = "complete_record";
        out.result["display_decimation_note"] = "Only transmitted spectrum display arrays are reduced after full shared-core analysis; order bands, statistics and diagnosis retain full-resolution inputs.";
        require(out.record->bin.unchanged() && out.record->meta.unchanged(), "RECORD_CHANGED", "Capture changed during complete analysis");
    } catch (const Failure& e) { out.code = e.code; out.error = QString::fromUtf8(e.what()); }
      catch (const std::exception& e) { out.code = "ANALYSIS_FAILED"; out.error = QString::fromUtf8(e.what()); }
    return out;
}
struct Encoded {
    QByteArray base64, sha;
    int bytes() const { return base64.size() + sha.size() + 256; }
};
// File status flags are shared with the parent shell's inherited open-file
// description. stty restores termios only, so restore O_NONBLOCK ourselves.
class ConsoleFlags {
    std::array<int, 2> original_{};
    void restore() noexcept {
        for (int fd = 0; fd < 2; ++fd) if (original_[std::size_t(fd)] >= 0) (void)::fcntl(fd, F_SETFL, original_[std::size_t(fd)]);
    }
public:
    ConsoleFlags() {
        for (int fd = 0; fd < 2; ++fd) original_[std::size_t(fd)] = ::fcntl(fd, F_GETFL, 0);
        require(original_[0] >= 0 && original_[1] >= 0, "IO_ERROR", "Cannot inspect console file flags");
        for (int fd = 0; fd < 2; ++fd) {
            if (::fcntl(fd, F_SETFL, original_[std::size_t(fd)] | O_NONBLOCK) != 0) {
                restore(); fail("IO_ERROR", "Cannot configure nonblocking console");
            }
        }
    }
    ~ConsoleFlags() { restore(); }
};
Encoded encode(const QJsonObject& object) {
    const auto raw = QJsonDocument(object).toJson(QJsonDocument::Compact);
    require(raw.size() <= maxDecoded, "PACKET_TOO_LARGE", "Uncompressed response exceeds protocol limit");
    Encoded value{qCompress(raw, 6).toBase64(), QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex()};
    require(value.bytes() <= maxWire, "PACKET_TOO_LARGE", "Encoded response exceeds protocol line limit"); return value;
}

class Bridge final : public QObject {
    ConsoleFlags consoleFlags_;
    CaptureClient client_; LivePreviewModel model_;
    QSocketNotifier input_{STDIN_FILENO, QSocketNotifier::Read, this}, output_{STDOUT_FILENO, QSocketNotifier::Write, this};
    QTimer timer_{this}, previewTimer_{this}; QByteArray ingress_, writing_;
    std::deque<Encoded> reliable_; std::optional<Encoded> preview_; QSet<QString> requestIds_;
    QHash<QString, std::shared_ptr<Record>> records_; std::future<Task> task_;
    QHash<QString, std::pair<QString, QString>> pending_; // Existing service facade correlates by command.
    QString session_, sessionDir_, activeJob_, generation_, downloadId_; QJsonObject caps_, plan_;
    quint64 seq_ = 0, epoch_ = 1; int writeOffset_ = 0, queueBytes_ = 0, rejectedRecovery_ = 0;
    qint64 lastInput_ = QDateTime::currentMSecsSinceEpoch(), closingAt_ = 0, lastStatusTime_ = 0, lastPreviewRejectTime_ = 0;
    quint64 previewRejectCount_ = 0;
    QString lastStatusState_;
    bool active_ = false, startPending_ = false, closing_ = false, connected_ = false, discardingInput_ = false, recoveryDone_ = false, ignoreOutgoing_ = false, cancelAfterStart_ = false;
    QString exitReason_; int exitCode_ = 0, baud_ = 115200;
public:
    Bridge(const QString& service, const QStringList& arguments, int baud) : baud_(baud) {
        std::signal(SIGPIPE, SIG_IGN); std::signal(SIGINT, SIG_IGN);
        struct stat root{};
        if (::lstat(QFile::encodeName(recordRoot).constData(), &root) != 0) {
            require(errno == ENOENT && ::mkdir(QFile::encodeName(recordRoot).constData(), 0700) == 0, "FILE_ERROR", "Cannot create managed RAM record root");
        }
        directorySafe(recordRoot); session_ = QUuid::createUuid().toString(QUuid::Id128); sessionDir_ = recordRoot + '/' + session_;
        require(::mkdir(QFile::encodeName(sessionDir_).constData(), 0700) == 0, "FILE_ERROR", "Cannot create private RAM capture directory");
        output_.setEnabled(false);
        connect(&input_, &QSocketNotifier::activated, this, [this](int) { readInput(); });
        connect(&output_, &QSocketNotifier::activated, this, [this](int) { writeOutput(); });
        timer_.setInterval(25); connect(&timer_, &QTimer::timeout, this, [this] { tick(); }); timer_.start();
        previewTimer_.setInterval(baud_ == 115200 ? 1000 : 250);
        connect(&previewTimer_, &QTimer::timeout, this, [this] { publishPreview(); }); previewTimer_.start();
        connect(&client_, &CaptureClient::responseReceived, this, [this](const QString& command, const QJsonObject& data) { serviceResponse(command, data); });
        connect(&client_, &CaptureClient::requestRejected, this, [this](const QString& command, const QString& code, const QString& message) {
            if (command == "start") {
                startPending_ = false; cancelAfterStart_ = false;
                if (pending_.contains("cancel")) { const auto request = pending_.take("cancel"); error(request.first, request.second, "START_FAILED", "Capture did not start; no cancellation is needed"); }
            }
            if (pending_.contains(command)) { const auto request = pending_.take(command); error(request.first, request.second, code, message); }
            else event("service_error", {{"command", command}, {"code", code}, {"message", message}});
        });
        connect(&client_, &CaptureClient::jobUpdated, this, [this](const QJsonObject& status) { jobStatus(status); });
        connect(&client_, &CaptureClient::liveFrame, this, [this](const QJsonObject& frame) {
            if (!active_ || frame.value("job_id") != activeJob_) return;
            QString detail; const bool ok = model_.isLive() ? model_.accept(frame, &detail) : model_.begin(frame, true, &detail);
            if (ok) { generation_ = frame.value("generation").toString(); model_.setSpectrumDemand(true); }
            else rejectPreview(detail, model_.info().quarantined);
        });
        connect(&client_, &CaptureClient::previewRejected, this, [this](const QString& reason, bool quarantined) {
            rejectPreview(reason, quarantined);
        });
        connect(&client_, &CaptureClient::connectionFailed, this, [this](const QString& reason) {
            connected_ = false; startPending_ = false; active_ = false; model_.end(reason); preview_.reset();
            const auto requests = pending_; pending_.clear(); for (const auto& request : requests) error(request.first, request.second, "SERVICE_LOST", reason);
            if (!closing_) event("connection_lost", {{"reason", reason}});
        });
        client_.setCommand(service, arguments); client_.connectService(); recover();
    }
    ~Bridge() override { client_.stop(); }
private:
    void recover() {
        task_ = std::async(std::launch::async, [] {
            Task out; out.kind = "recovery";
            try {
                const auto sessions = QDir(recordRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
                require(sessions.size() <= 256, "RECOVERY_LIMIT", "Too many managed session directories; preserve records and archive them explicitly");
                for (const auto& session : sessions) {
                    if (!QRegularExpression("^[0-9a-f]{32}$").match(session).hasMatch()) continue;
                    const auto dir = recordRoot + '/' + session;
                    try { directorySafe(dir); } catch (...) { ++out.rejected; continue; }
                    const auto files = QDir(dir).entryList({"*.bin", "*.BIN"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
                    for (const auto& file : files) {
                        require(out.records.size() < maxRecords, "RECOVERY_LIMIT", "More complete records than bounded registry supports; no files were deleted");
                        try { out.records.push_back(validateRecord(dir + '/' + file, {})); } catch (...) { ++out.rejected; }
                    }
                }
            } catch (const Failure& e) { out.code = e.code; out.error = QString::fromUtf8(e.what()); }
              catch (const std::exception& e) { out.code = "RECOVERY_FAILED"; out.error = QString::fromUtf8(e.what()); }
            return out;
        });
    }
    void enqueue(const QJsonObject& object, bool preview = false) {
        if (ignoreOutgoing_) return;
        try {
            auto encoded = encode(object);
            if (preview) { preview_ = std::move(encoded); output_.setEnabled(true); return; }
            if (reliable_.size() >= maxReliable || queueBytes_ + encoded.bytes() > maxQueueBytes) {
                close("Reliable output queue overflow; owned acquisition is being cancelled", 3, false); return;
            }
            queueBytes_ += encoded.bytes(); reliable_.push_back(std::move(encoded)); output_.setEnabled(true);
        } catch (const Failure& e) {
            if (preview) return;
            close(QString::fromUtf8(e.what()), 3, false);
        }
    }
    void response(const QString& id, const QString& command, const QJsonObject& data) { enqueue({{"id", id}, {"command", command}, {"ok", true}, {"data", data}}); }
    void error(const QString& id, const QString& command, const QString& code, const QString& message) { enqueue({{"id", id}, {"command", command}, {"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}}); }
    void event(const QString& name, const QJsonObject& data) { enqueue({{"event", name}, {"data", data}}); }
    void writeOutput() {
        int budget = 16384;
        while (budget > 0) {
            if (writing_.isEmpty()) {
                std::optional<Encoded> next;
                if (!reliable_.empty()) { next = std::move(reliable_.front()); queueBytes_ -= next->bytes(); reliable_.pop_front(); }
                else if (preview_) { next = std::move(preview_); preview_.reset(); }
                else { output_.setEnabled(false); return; }
                writing_ = QJsonDocument(QJsonObject{{"schema", schema}, {"type", "packet"}, {"seq", double(++seq_)}, {"codec", "qcompress-base64"},
                    {"payload", QString::fromLatin1(next->base64)}, {"sha256", QString::fromLatin1(next->sha)}}).toJson(QJsonDocument::Compact) + '\n';
                writeOffset_ = 0;
            }
            const auto count = ::write(STDOUT_FILENO, writing_.constData() + writeOffset_, std::size_t(std::min(budget, writing_.size() - writeOffset_)));
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                close("Serial output disconnected", 2, false); return;
            }
            if (!count) return;
            writeOffset_ += int(count); budget -= int(count);
            if (writeOffset_ == writing_.size()) { writing_.clear(); writeOffset_ = 0; }
        }
    }
    void readInput() {
        int budget = 32768; std::array<char, 4096> chunk{};
        while (budget > 0 && !closing_) {
            const auto n = ::read(STDIN_FILENO, chunk.data(), std::min(int(chunk.size()), budget));
            if (n < 0) { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) break; close("Serial input failed", 2, false); return; }
            if (!n) { close("Serial input EOF", 0, false); return; }
            budget -= int(n);
            for (int i = 0; i < n; ++i) {
                const char c = chunk[std::size_t(i)];
                if (c == '\n') {
                    if (discardingInput_) { discardingInput_ = false; error({}, {}, "LINE_TOO_LONG", "Request line exceeds8192 bytes"); }
                    else if (!ingress_.trimmed().isEmpty()) request(ingress_);
                    ingress_.clear();
                } else if (!discardingInput_) {
                    if (ingress_.size() >= maxRequest) { ingress_.clear(); discardingInput_ = true; } else ingress_.append(c);
                }
                if (closing_) return;
            }
        }
    }
    void request(const QByteArray& bytes) {
        QString id, command;
        try {
            const auto object = parse(bytes); id = object.value("id").toString(); command = object.value("command").toString();
            require(object.value("schema") == schema && object.value("id").isString() &&
                QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(id).hasMatch() && object.value("command").isString(), "INVALID_REQUEST", "Unsupported schema, request id or command type");
            require(!requestIds_.contains(id), "DUPLICATE_REQUEST", "Request id was already used");
            require(requestIds_.size() < 65536, "REQUEST_LIMIT", "Session request limit reached; reconnect bridge"); requestIds_.insert(id);
            const auto allowed = QStringList{"schema", "id", "command"};
            lastInput_ = QDateTime::currentMSecsSinceEpoch();
            if (command == "hello") {
                keys(object, allowed); response(id, command, {{"version", "1.1"}, {"baud", baud_}, {"preview_interval_ms", previewTimer_.interval()}, {"servicecaps", caps_}, {"ready", connected_},
                    {"recordroot", recordRoot}, {"volatile", true}, {"session_id", session_}, {"max_wire_line", maxWire}, {"max_decoded_bytes", maxDecoded},
                    {"heartbeat_seconds", 3}, {"client_loss_seconds", 15}, {"recovery_complete", recoveryDone_}});
            } else if (command == "ping") {
                keys(object, allowed); response(id, command, {{"time_ms", double(lastInput_)}, {"active", active_ || startPending_}, {"analysis_busy", task_.valid()},
                    {"job_id", activeJob_}, {"reliable_queue_bytes", queueBytes_}, {"records", records_.size()}});
            } else if (command == "start") {
                keys(object, allowed + QStringList{"plan"}); require(connected_, "UNAVAILABLE", "Capture service is not ready");
                require(recoveryDone_ && !task_.valid() && !active_ && !startPending_, "BUSY", "Capture, record recovery or complete analysis is active");
                require(records_.size() < maxRecords, "STORAGE_LIMIT", "Managed record registry is full; no records were deleted");
                require(object.value("plan").isObject(), "INVALID_PLAN", "plan must be an object"); auto plan = object.value("plan").toObject();
                keys(plan, {"sample_rate_hz", "duration_seconds", "pulses_per_rev"});
                require(integer(plan.value("sample_rate_hz"), 5000, 50000) && std::set<int>{5000, 10000, 25000, 50000}.count(plan.value("sample_rate_hz").toInt()) &&
                    integer(plan.value("duration_seconds"), 1, 40) && integer(plan.value("pulses_per_rev"), 1, 4096), "INVALID_PLAN", "Invalid sample rate, integer duration1..40 or pulses per revolution");
                const qint64 expected = 4096 + qint64(plan.value("sample_rate_hz").toInt()) * plan.value("duration_seconds").toInt() * 32;
                qint64 held = 0; for (const auto& record : records_) held += record->bin.bytes() + record->meta.bytes();
                struct statvfs space{}; require(expected <= maxFile && held + expected <= maxRootBytes && ::statvfs(QFile::encodeName(recordRoot).constData(), &space) == 0 &&
                    quint64(space.f_bavail) * space.f_frsize > quint64(expected + 8 * 1024 * 1024), "STORAGE_LIMIT", "Capture would exceed RAM storage bound or available free space");
                directorySafe(recordRoot); directorySafe(sessionDir_); plan["output_directory"] = sessionDir_;
                plan_ = plan; model_.reset(); generation_.clear(); preview_.reset(); downloadId_.clear(); ++epoch_;
                previewRejectCount_ = 0; lastPreviewRejectTime_ = 0; lastStatusState_.clear();
                activeJob_.clear(); cancelAfterStart_ = false; startPending_ = true;
                pending_[command] = {id, command}; client_.request("start", {{"plan", plan}, {"preview", true}});
            } else if (command == "cancel") {
                keys(object, allowed + QStringList{"job_id"});
                require(!object.contains("job_id") || (object.value("job_id").isString() && object.value("job_id").toString() == activeJob_), "UNKNOWN_JOB", "job_id does not identify the current job");
                require(!pending_.contains("cancel"), "BUSY", "Cancellation acknowledgement is already pending");
                if (startPending_) { pending_[command] = {id, command}; cancelAfterStart_ = true; preview_.reset(); return; }
                if (!active_) { response(id, command, {{"job_id", activeJob_}, {"accepted", false}}); return; }
                preview_.reset(); pending_[command] = {id, command}; client_.request("cancel", {{"job_id", activeJob_}});
            } else if (command == "list_records") {
                keys(object, allowed); require(recoveryDone_, "BUSY", "Managed record recovery is still active"); QJsonArray list;
                auto ids = records_.keys(); std::sort(ids.begin(), ids.end()); for (const auto& key : ids) if (records_[key]->bin.unchanged() && records_[key]->meta.unchanged()) list.append(records_[key]->json());
                response(id, command, {{"records", list}, {"volatile", true}, {"rejected_records", rejectedRecovery_}, {"limit", maxRecords}});
            } else if (command == "analyze_record") {
                keys(object, allowed + QStringList{"record_id"}); const auto record = findRecord(object);
                require(!active_ && !startPending_ && !task_.valid(), "BUSY", "Complete analysis is only available while capture and record I/O are idle");
                downloadId_.clear(); const auto token = ++epoch_; const auto path = record->bin.path, job = record->job;
                task_ = std::async(std::launch::async, [path, job, id, token] { return analyzeTask(path, job, id, token); });
            } else if (command == "download_begin") {
                keys(object, allowed + QStringList{"record_id"}); idleDownload(); const auto record = findRecord(object);
                require(record->bin.unchanged() && record->meta.unchanged(), "RECORD_CHANGED", "Capture pair identity changed"); downloadId_ = record->id;
                response(id, command, {{"record_id", record->id}, {"files", QJsonArray{record->bin.json(), record->meta.json()}}, {"max_chunk", 3072}, {"volatile", true}});
            } else if (command == "download_chunk") {
                keys(object, allowed + QStringList{"record_id", "kind", "offset", "length"}); idleDownload(); const auto record = findRecord(object);
                require(downloadId_ == record->id, "DOWNLOAD_NOT_OPEN", "Issue download_begin for this record first");
                const auto kind = object.value("kind").toString(); require(kind == "bin" || kind == "meta", "INVALID_REQUEST", "kind must be bin or meta");
                require(record->bin.unchanged() && record->meta.unchanged(), "RECORD_CHANGED", "Capture pair identity changed during download");
                const auto& file = kind == "bin" ? record->bin : record->meta;
                require(integer(object.value("offset"), 0, double(file.bytes())) && integer(object.value("length"), 1, 3072), "INVALID_RANGE", "Invalid chunk offset or length");
                const auto offset = qint64(object.value("offset").toDouble()); const int length = object.value("length").toInt();
                require(offset + length <= file.bytes(), "INVALID_RANGE", "Chunk extends beyond file"); const auto data = readAt(file, offset, length);
                response(id, command, {{"record_id", record->id}, {"kind", kind}, {"offset", double(offset)}, {"length", length}, {"total", double(file.bytes())},
                    {"data", QString::fromLatin1(data.toBase64())}, {"crc32", double(vibration::crc32(reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size())))}});
            } else if (command == "shutdown") {
                keys(object, allowed); response(id, command, {{"accepted", true}}); close("Client requested shutdown", 0, true);
            } else fail("UNKNOWN_COMMAND", "Unsupported command");
        } catch (const Failure& e) { error(id, command, e.code, QString::fromUtf8(e.what())); }
          catch (const std::exception& e) { error(id, command, "INTERNAL_ERROR", QString::fromUtf8(e.what())); }
    }
    std::shared_ptr<Record> findRecord(const QJsonObject& request) {
        require(request.value("record_id").isString(), "UNKNOWN_RECORD", "record_id must be a listed opaque ID");
        const auto id = request.value("record_id").toString(); require(records_.contains(id), "UNKNOWN_RECORD", "Unknown complete record ID"); return records_.value(id);
    }
    void idleDownload() { require(recoveryDone_ && !active_ && !startPending_ && !task_.valid(), "BUSY", "Download is only available while capture and complete analysis are idle"); }
    void rejectPreview(const QString& reason, bool quarantined) {
        preview_.reset(); if (quarantined) model_.end(reason); ++previewRejectCount_;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (quarantined || !lastPreviewRejectTime_ || now - lastPreviewRejectTime_ >= 1000) {
            lastPreviewRejectTime_ = now;
            event("preview_rejected", {{"job_id", activeJob_}, {"reason", reason}, {"quarantined", quarantined}, {"rejected_frames", double(previewRejectCount_)}});
        }
    }
    void serviceResponse(const QString& command, const QJsonObject& data) {
        if (command == "capabilities") { caps_ = data; connected_ = true; event("service_ready", {{"servicecaps", caps_}}); return; }
        if (command == "start") { startPending_ = false; active_ = true; activeJob_ = data.value("job_id").toString(); }
        if (pending_.contains(command)) { const auto request = pending_.take(command); response(request.first, request.second, data); }
        if (command == "start" && cancelAfterStart_) { cancelAfterStart_ = false; client_.request("cancel", {{"job_id", activeJob_}}); }
    }
    void jobStatus(QJsonObject status) {
        if (status.value("job_id") != activeJob_) return;
        const auto state = status.value("state").toString();
        if (terminal(state)) {
            active_ = false; preview_.reset(); model_.end(state); status["generation"] = generation_;
            event("job_status", status);
            if (state == "completed" && !closing_) {
                const auto path = status.value("output_path").toString(), job = activeJob_; const auto token = epoch_;
                if (!task_.valid()) task_ = std::async(std::launch::async, [path, job, token] { return analyzeTask(path, job, {}, token); });
                else event("analysis_failed", {{"job_id", job}, {"code", "BUSY"}, {"message", "Complete record analysis task unavailable"}});
            }
        } else {
            // Progress counters are observations, not a reliable state
            // transition stream. Limit repeated same-state progress to1Hz;
            // every state change and every terminal event remains reliable.
            const auto now = QDateTime::currentMSecsSinceEpoch();
            if (state != lastStatusState_ || now - lastStatusTime_ >= 1000) {
                lastStatusState_ = state; lastStatusTime_ = now; event("job_status", status);
            }
        }
    }
    void publishPreview() {
        if (!active_ || closing_ || !model_.isLive() || model_.info().quarantined) return;
        model_.pollSpectrum(); const auto info = model_.info(); const auto view = model_.view(.25); const auto spectrum = model_.spectrum();
        if (info.jobId != activeJob_ || info.generation != generation_) return;
        const auto amplitudeStats = vibration::serial::rawWindowStats(view.voltage, view.startTime, view.endTime, info.revision);
        for (const int limit : {156, 96, 60, 36, 18}) {
            QJsonArray voltage; for (const auto& series : view.voltage) voltage.append(reducePoints(series.points, limit));
            QJsonArray channels;
            if (spectrum.available) for (const auto& channel : spectrum.channels) {
                QJsonArray frequencies, amplitudes; for (const auto value : channel.frequency_hz) frequencies.append(value); for (const auto value : channel.peak_amplitude) amplitudes.append(value);
                channels.append(decimateSpectrum({{"frequency_hz", frequencies}, {"peak_amplitude", amplitudes}, {"fft_size", double(channel.fft_size)},
                    {"bin_width_hz", channel.bin_width_hz}, {"segment_count", double(channel.segment_count)}}, std::min(128, limit)));
            }
            const QJsonObject data{{"job_id", info.jobId}, {"generation", info.generation}, {"revision", double(info.revision)}, {"start_s", view.startTime}, {"end_s", view.endTime},
                {"sample_rate_hz", double(info.sampleRate)}, {"source", info.source}, {"synthetic", info.synthetic}, {"hardware_confirmed", info.hardwareConfirmed}, {"ppr_confirmed", info.pprConfirmed},
                {"preview_dropped_records", double(info.previewDroppedRecords)}, {"voltage", voltage}, {"speed", reducePoints(view.speed.points, limit)},
                {"amplitude_stats", amplitudeStats},
                {"mean_rpm", view.meanSpeed ? numeric(*view.meanSpeed) : QJsonValue(QJsonValue::Null)}, {"valid_speed_fraction", view.validSpeedFraction},
                {"spectrum", QJsonObject{{"available", spectrum.available}, {"start_s", spectrum.startTime}, {"end_s", spectrum.endTime}, {"channels", channels}, {"input_revision", double(spectrum.inputRevision)}}},
                {"diagnosis", QJsonObject{{"status", "preview_only"}, {"classification", "not_classified"}, {"limitations", QJsonArray{
                    "Lossy display summary of a bounded validated raw window; complete-record analysis follows saved-file integrity verification.",
                    "ADC voltage does not establish sensor calibration, physical vibration identity or a fault classification."}}}}};
            const QJsonObject payload{{"event", "preview"}, {"data", data}};
            const auto encoded = encode(payload);
            if (encoded.base64.size() * 3 / 4 <= 6144 || limit == 18) { preview_ = encoded; output_.setEnabled(true); break; }
        }
    }
    void pollTask() {
        if (!task_.valid() || task_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto result = task_.get();
        if (result.kind == "recovery") {
            for (const auto& record : result.records) records_[record->id] = record;
            rejectedRecovery_ = result.rejected;
            recoveryDone_ = result.error.isEmpty(); event("records_ready", {{"count", records_.size()}, {"rejected_records", rejectedRecovery_}, {"ok", recoveryDone_}, {"code", result.code}, {"message", result.error}}); return;
        }
        if (result.record && result.record->bin.unchanged() && result.record->meta.unchanged()) records_[result.record->id] = result.record;
        if (result.epoch != epoch_ || closing_) return;
        if (!result.error.isEmpty()) {
            if (result.id.isEmpty()) event("analysis_failed", {{"job_id", result.job}, {"code", result.code}, {"message", result.error}});
            else error(result.id, result.command, result.code, result.error);
            return;
        }
        QJsonObject data{{"job_id", result.job}, {"record_id", result.record->id}, {"result", result.result}, {"scope", "complete_record"},
            {"source_metadata", result.record->metadata}, {"files", QJsonArray{result.record->bin.json(), result.record->meta.json()}}};
        if (result.id.isEmpty()) event("analysis", data); else response(result.id, result.command, data);
    }
    void tick() {
        pollTask();
        if (closing_) {
            if ((!active_ && !startPending_ && reliable_.empty() && writing_.isEmpty()) || QDateTime::currentMSecsSinceEpoch() >= closingAt_) {
                client_.stop(); if (!exitReason_.isEmpty()) std::fprintf(stderr, "serial_bridge: %s\n", exitReason_.toUtf8().constData()); QCoreApplication::exit(exitCode_);
            }
            return;
        }
        if (QDateTime::currentMSecsSinceEpoch() - lastInput_ > 15000) close("Client heartbeat expired; owned acquisition cancelled", 2, true);
    }
    void close(const QString& reason, int code, bool drain) {
        if (closing_) return;
        closing_ = true; exitReason_ = reason; exitCode_ = code; input_.setEnabled(false); preview_.reset(); model_.end(reason);
        if (startPending_) cancelAfterStart_ = true;
        if (active_ && !client_.hasPending("cancel")) client_.request("cancel", {{"job_id", activeJob_}});
        if (!drain) { ignoreOutgoing_ = true; reliable_.clear(); queueBytes_ = 0; output_.setEnabled(false); writing_.clear(); }
        closingAt_ = QDateTime::currentMSecsSinceEpoch() + (drain ? 2000 : 500);
    }
};
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QString service = QCoreApplication::applicationDirPath() + "/vibration_capture_service"; QStringList serviceArguments;
    bool fixture = false, selfTest = false; int baud = 115200; QSet<QString> seen;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (seen.contains(argument)) { std::fprintf(stderr, "Duplicate option\n"); return 2; } seen.insert(argument);
        if (argument == "--service" && i + 1 < argc) service = QString::fromLocal8Bit(argv[++i]);
        else if (argument == "--baud" && i + 1 < argc) {
            const QString value = QString::fromLocal8Bit(argv[++i]);
            if (!QStringList{"115200", "460800", "921600"}.contains(value)) { std::fprintf(stderr, "--baud must be115200,460800 or921600\n"); return 2; }
            baud = value.toInt();
        }
        else if (argument == "--test-fixture") { fixture = true; serviceArguments << argument; }
        else if (argument == "--hardware-self-test") { selfTest = true; serviceArguments << argument; }
        else if (argument.startsWith("--fixture-fault=")) serviceArguments << argument;
        else if (argument == "--help") { std::fprintf(stderr, "Usage: vibration_serial_bridge [--service ABSOLUTE_EXECUTABLE] [--baud 115200|460800|921600] [--test-fixture | --hardware-self-test]\nConsole JSONL only; production defaults to unchanged capture service. Baud defaults to115200; caller configures the physical port. Records are volatile under /tmp.\n"); return 0; }
        else { std::fprintf(stderr, "Unknown or incomplete option\n"); return 2; }
    }
    if ((fixture && selfTest) || (!fixture && serviceArguments.join(' ').contains("--fixture-fault="))) { std::fprintf(stderr, "Conflicting or non-explicit fixture mode\n"); return 2; }
    if (!QDir::isAbsolutePath(service) || !QFileInfo(service).isExecutable() || !QFileInfo(service).isFile()) { std::fprintf(stderr, "--service must be an absolute executable path\n"); return 2; }
    // The worker mailbox keeps raw transport decoding off the console event loop.
    qputenv("AX7_CAPTURE_WORKER", "1"); qputenv("AX7_LIVE_DEMAND_ANALYSIS", "1");
    try { Bridge bridge(service, serviceArguments, baud); return app.exec(); }
    catch (const std::exception& e) { std::fprintf(stderr, "serial_bridge: %s\n", e.what()); return 2; }
}
