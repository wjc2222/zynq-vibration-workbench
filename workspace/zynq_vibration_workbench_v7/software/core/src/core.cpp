#include "vibration/core.hpp"
#include "vibration/sha256.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace vibration {
namespace {
constexpr std::uint64_t kFirmwareMaxPayload = 64ULL * 1024 * 1024 - 4096;
constexpr double kPi = 3.14159265358979323846;

std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}
std::uint32_t u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t u64(const std::uint8_t* p) {
    return u32(p) | (std::uint64_t(u32(p + 4)) << 32);
}
std::int16_t i16(const std::uint8_t* p) {
    const auto x = u16(p);
    return static_cast<std::int16_t>(x < 0x8000 ? int(x) : int(x) - 65536);
}
std::int32_t i32(const std::uint8_t* p) {
    const auto x = u32(p);
    return static_cast<std::int32_t>(x <= 0x7fffffff ? std::int64_t(x) : std::int64_t(x) - 4294967296LL);
}
const std::array<std::array<std::uint32_t, 256>, 8>& crc_table() {
    static const auto table = [] {
        std::array<std::array<std::uint32_t, 256>, 8> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto c = i;
            for (int b = 0; b < 8; ++b) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0u);
            t[0][i] = c;
        }
        for (std::size_t slice = 1; slice < t.size(); ++slice)
            for (std::size_t i = 0; i < t[slice].size(); ++i)
                t[slice][i] = (t[slice - 1][i] >> 8) ^ t[0][t[slice - 1][i] & 255u];
        return t;
    }();
    return table;
}
std::uint32_t crc_update(std::uint32_t c, const std::uint8_t* data, std::size_t n) {
    const auto& t = crc_table();
    // Byte assembly is safe for unaligned input and independent of host endian.
    while (n >= 8) {
        c ^= u32(data);
        c = t[7][c & 255u] ^ t[6][(c >> 8) & 255u] ^ t[5][(c >> 16) & 255u] ^ t[4][c >> 24] ^
            t[3][data[4]] ^ t[2][data[5]] ^ t[1][data[6]] ^ t[0][data[7]];
        data += 8;
        n -= 8;
    }
    for (std::size_t i = 0; i < n; ++i) c = t[0][(c ^ data[i]) & 255u] ^ (c >> 8);
    return c;
}
void require(bool ok, const std::string& message) { if (!ok) throw AnalysisError(message); }

// Parse the complete, bounded sidecar rather than matching text: nested labels,
// quoted examples, duplicate provenance keys and malformed JSON cannot promote
// a file to a physical measurement or hide a known synthetic declaration.
class MetadataReader {
public:
    explicit MetadataReader(const std::string& input) : input_(input) {}
    SourceMetadata read() {
        SourceMetadata result;
        bool seen_synthetic = false, seen_kind = false;
        expect('{');
        if (!take('}')) {
            do {
                const auto key = string();
                expect(':');
                if (key == "synthetic") {
                    require(!seen_synthetic, "Duplicate source metadata key");
                    seen_synthetic = true;
                    if (literal("true")) result.synthetic = true;
                    else require(literal("false"), "Invalid synthetic declaration");
                } else if (key == "source_kind") {
                    require(!seen_kind, "Duplicate source metadata key");
                    seen_kind = true;
                    result.synthetic = (string() == "synthetic") || result.synthetic;
                } else value(1);
            } while (take(','));
            expect('}');
        }
        whitespace();
        require(position_ == input_.size(), "Trailing source metadata content");
        result.status = result.synthetic ? "synthetic_sidecar" : "unconfirmed_sidecar";
        return result;
    }
private:
    const std::string& input_;
    std::size_t position_ = 0;
    void whitespace() {
        while (position_ < input_.size() && (input_[position_] == ' ' || input_[position_] == '\t' ||
               input_[position_] == '\r' || input_[position_] == '\n')) ++position_;
    }
    bool take(char ch) {
        whitespace();
        if (position_ < input_.size() && input_[position_] == ch) { ++position_; return true; }
        return false;
    }
    void expect(char ch) { require(take(ch), "Invalid source metadata JSON"); }
    bool literal(const char* word) {
        whitespace();
        const std::string token(word);
        if (input_.compare(position_, token.size(), token) != 0) return false;
        position_ += token.size();
        return true;
    }
    std::uint32_t hex4() {
        require(input_.size() - position_ >= 4, "Truncated Unicode escape");
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const auto ch = input_[position_++];
            const int digit = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                              ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            require(digit >= 0, "Invalid Unicode escape");
            code = (code << 4) | std::uint32_t(digit);
        }
        return code;
    }
    std::string string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const auto ch = static_cast<unsigned char>(input_[position_++]);
            if (ch == '"') return result;
            require(ch >= 32, "Control character in metadata string");
            if (ch != '\\') { result += char(ch); continue; }
            require(position_ < input_.size(), "Truncated metadata escape");
            switch (input_[position_++]) {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u': {
                auto code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    require(position_ + 2 <= input_.size() && input_[position_] == '\\' && input_[position_ + 1] == 'u',
                            "Unpaired metadata surrogate");
                    position_ += 2;
                    const auto low = hex4();
                    require(low >= 0xdc00 && low <= 0xdfff, "Unpaired metadata surrogate");
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else require(code < 0xdc00 || code > 0xdfff, "Unpaired metadata surrogate");
                if (code < 0x80) result += char(code);
                else if (code < 0x800) { result += char(0xc0 | (code >> 6)); result += char(0x80 | (code & 63)); }
                else if (code < 0x10000) {
                    result += char(0xe0 | (code >> 12)); result += char(0x80 | ((code >> 6) & 63)); result += char(0x80 | (code & 63));
                } else {
                    result += char(0xf0 | (code >> 18)); result += char(0x80 | ((code >> 12) & 63));
                    result += char(0x80 | ((code >> 6) & 63)); result += char(0x80 | (code & 63));
                }
                break;
            }
            default: throw AnalysisError("Invalid metadata escape");
            }
        }
        throw AnalysisError("Unterminated metadata string");
    }
    void value(unsigned depth) {
        require(depth <= 32, "Metadata nesting limit exceeded");
        whitespace();
        require(position_ < input_.size(), "Missing metadata value");
        if (input_[position_] == '"') { string(); return; }
        if (take('{')) {
            if (!take('}')) {
                do { string(); expect(':'); value(depth + 1); } while (take(','));
                expect('}');
            }
            return;
        }
        if (take('[')) {
            if (!take(']')) { do { value(depth + 1); } while (take(',')); expect(']'); }
            return;
        }
        if (literal("true") || literal("false") || literal("null")) return;
        if (input_[position_] == '-') ++position_;
        auto digits = [this] {
            const auto begin = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') ++position_;
            require(position_ > begin, "Invalid metadata number");
        };
        if (position_ < input_.size() && input_[position_] == '0') ++position_;
        else digits();
        // Number punctuation cannot be separated by whitespace.
        if (position_ < input_.size() && input_[position_] == '.') { ++position_; digits(); }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            digits();
        }
    }
};

SourceMetadata source_metadata(const std::filesystem::path& capture_path) {
    auto path = capture_path;
    path.replace_extension(".meta.json");
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (!exists && !error) return {};
    SourceMetadata invalid; invalid.status = "invalid_or_unreadable_sidecar";
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 128 * 1024) return invalid;
    std::ifstream in(path, std::ios::binary);
    std::string input(static_cast<std::size_t>(size), '\0');
    if (!in.read(input.data(), static_cast<std::streamsize>(input.size())) || in.peek() != std::char_traits<char>::eof()) return invalid;
    try { return MetadataReader(input).read(); }
    catch (const AnalysisError&) { return invalid; }
}

bool valid_speed(const Record& r) {
    return (r.flags & 2u) && !(r.flags & 4u) && r.speed_period_ticks != 0;
}
bool power_of_two(std::size_t n) { return n && !(n & (n - 1)); }
std::size_t floor_power_two(std::size_t n) {
    std::size_t p = 1;
    while (p <= n / 2) p *= 2;
    return p;
}
struct SpectrumPlan {
    std::size_t size;
    std::vector<double> window;
    double sum_w = 0, sum_w2 = 0;
    std::vector<std::pair<std::size_t, std::size_t>> swaps;
    std::vector<std::vector<std::complex<double>>> twiddles;

    explicit SpectrumPlan(std::size_t n) : size(n), window(n) {
        for (std::size_t j = 0; j < n; ++j) {
            window[j] = 0.5 - 0.5 * std::cos(2.0 * kPi * double(j) / double(n));
            sum_w += window[j]; sum_w2 += window[j] * window[j];
        }
        for (std::size_t i = 1, j = 0; i < n; ++i) {
            auto bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) swaps.emplace_back(i, j);
        }
        for (std::size_t len = 2; len <= n; len <<= 1) {
            const std::complex<double> step = std::polar(1.0, -2.0 * kPi / double(len));
            std::vector<std::complex<double>> row(len / 2);
            std::complex<double> w(1.0, 0.0);
            // Preserve the original recurrence, including its rounding order.
            for (std::size_t j = 0; j < len / 2; ++j) { row[j] = w; w *= step; }
            twiddles.push_back(std::move(row));
        }
    }
};

void fft(std::vector<std::complex<double>>& a, const SpectrumPlan& plan) {
    const auto n = a.size();
    for (const auto& swap : plan.swaps) std::swap(a[swap.first], a[swap.second]);
    std::size_t row = 0;
    for (std::size_t len = 2; len <= n; len <<= 1, ++row) {
        const auto& twiddles = plan.twiddles[row];
        for (std::size_t start = 0; start < n; start += len) {
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto u = a[start + j];
                const auto v = a[start + j + len / 2] * twiddles[j];
                a[start + j] = u + v;
                a[start + j + len / 2] = u - v;
            }
        }
    }
}
double value(const Capture& c, std::size_t i, std::size_t axis, double scale) {
    return double(c.records[i].raw[axis]) * scale;
}

void prepare_segment(const Capture& capture, std::size_t start, std::size_t axis, double scale,
                     const SpectrumPlan& plan, std::vector<std::complex<double>>& work) {
    double segment_mean = 0;
    for (std::size_t j = 0; j < plan.size; ++j) segment_mean += value(capture, start + j, axis, scale);
    segment_mean /= double(plan.size);
    for (std::size_t j = 0; j < plan.size; ++j)
        work[j] = (value(capture, start + j, axis, scale) - segment_mean) * plan.window[j];
    fft(work, plan);
}

void accumulate_segments(const Capture& capture, std::size_t axis, double scale, const SpectrumPlan& plan,
                         unsigned workers, std::vector<double>& energy, std::size_t& segment_count) {
    const auto n = plan.size;
    segment_count = 1 + (capture.records.size() - n) / (n / 2);
    const auto accumulate = [&](const std::vector<std::complex<double>>& work) {
        for (std::size_t k = 0; k <= n / 2; ++k) energy[k] += std::norm(work[k]);
    };
    // Thread handoff costs dominate tiny transforms. A one-window record cannot
    // overlap segment computation; both cases retain the exact serial path.
    if (workers == 1 || segment_count < 2 || n < 8192) {
        std::vector<std::complex<double>> work(n);
        for (std::size_t segment = 0; segment < segment_count; ++segment) {
            prepare_segment(capture, segment * (n / 2), axis, scale, plan, work);
            accumulate(work);
        }
        return;
    }
    struct Slot {
        std::vector<std::complex<double>> work;
        bool ready = false;
    };
    std::array<Slot, 2> slots;
    for (auto& slot : slots) slot.work.resize(n);
    std::mutex mutex;
    std::condition_variable changed;
    bool stop = false;
    std::exception_ptr failure;
    const auto produce = [&](std::size_t id) {
        try {
            auto& slot = slots[id];
            for (std::size_t segment = id; segment < segment_count; segment += 2) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock, [&] { return stop || !slot.ready; });
                    if (stop) return;
                }
                prepare_segment(capture, segment * (n / 2), axis, scale, plan, slot.work);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    slot.ready = true;
                }
                changed.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!failure) failure = std::current_exception();
                stop = true;
            }
            changed.notify_all();
        }
    };
    std::array<std::thread, 2> threads;
    const auto join = [&] {
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    };
    try {
        for (std::size_t id = 0; id < threads.size(); ++id) threads[id] = std::thread(produce, id);
        for (std::size_t segment = 0; segment < segment_count; ++segment) {
            auto& slot = slots[segment % 2];
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return stop || slot.ready; });
                if (stop) break;
            }
            // A producer cannot reuse its slot until this ordered accumulation
            // is complete. Memory remains bounded to two FFT work buffers.
            accumulate(slot.work);
            {
                std::lock_guard<std::mutex> lock(mutex);
                slot.ready = false;
            }
            changed.notify_all();
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        changed.notify_all();
        join();
        throw;
    }
    join();
    if (failure) std::rethrow_exception(failure);
}
std::string quote(const std::string& s) {
    std::ostringstream o;
    o << '"';
    for (unsigned char ch : s) {
        switch (ch) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if (ch < 32) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(ch) << std::dec;
            else o << char(ch);
        }
    }
    return o.str() + '"';
}
void number(std::ostream& o, double x) { if (std::isfinite(x)) o << x; else o << "null"; }
void optional(std::ostream& o, const std::optional<double>& x) { if (x) number(o, *x); else o << "null"; }
void doubles(std::ostream& o, const std::vector<double>& values) {
    o << '[';
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) o << ','; number(o, values[i]); }
    o << ']';
}
void strings(std::ostream& o, const std::vector<std::string>& values) {
    o << '[';
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) o << ','; o << quote(values[i]); }
    o << ']';
}
const char* signal_name(SignalKind kind) {
    switch (kind) {
    case SignalKind::VibrationConfirmed: return "vibration_confirmed";
    case SignalKind::TestSignal: return "test_signal";
    default: return "unknown";
    }
}
} // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    return crc_update(0xffffffffu, data, size) ^ 0xffffffffu;
}

Capture load_capture(const std::string& path, const LoadOptions& options) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    require(bool(in), "Cannot open capture file: " + path);
    const auto end = in.tellg();
    require(end >= 0, "Cannot determine file length");
    const auto bytes = static_cast<std::uint64_t>(end);
    require(bytes >= 4096, "Truncated 4096-byte header");
    require(bytes <= options.max_file_bytes && bytes <= kFirmwareMaxPayload + 4096,
            "Capture exceeds configured or protocol memory limit");
    in.seekg(0);
    std::array<std::uint8_t, 4096> b{};
    in.read(reinterpret_cast<char*>(b.data()), b.size());
    require(bool(in), "Cannot read file header");
    Sha256 capture_sha;capture_sha.update(b.data(),b.size());
    const std::array<std::uint8_t, 8> magic{'C', 'A', 'P', 'D', 'D', 'R', '2', 0};
    const std::array<std::uint8_t, 8> stream_magic{'C', 'A', 'P', 'S', 'T', 'R', 'M', '1'};
    const bool stream = std::equal(stream_magic.begin(), stream_magic.end(), b.begin());
    require(stream || std::equal(magic.begin(), magic.end(), b.begin()), "Wrong CAPDDR2/CAPSTRM1 magic");
    Capture capture;
    capture.source_path = path;
    auto& h = capture.header;
    h.version = u32(&b[8]); h.header_bytes = u32(&b[12]); h.record_bytes = u32(&b[16]);
    h.state = u32(&b[20]); h.sample_rate_hz = u32(&b[24]); h.clock_hz = u32(&b[28]);
    h.requested_records = u32(&b[32]); h.completed_records = u32(&b[36]);
    h.payload_bytes = u32(&b[40]); h.error_code = u32(&b[44]);
    h.hw_status = u32(&b[48]); h.dma_status = u32(&b[52]);
    h.dropped_records = u32(&b[56]); h.pulses_per_rev = u32(&b[60]);
    h.first_tick = u64(&b[64]); h.last_tick = u64(&b[72]);
    h.channel_map = u32(&b[80]); h.adc_range_uv = i32(&b[84]);
    h.speed_filter_cycles = u32(&b[88]); h.speed_timeout_cycles = u32(&b[92]);
    h.payload_crc32 = u32(&b[96]); h.header_crc32 = u32(&b[100]);
    h.stream_segment = stream;
    if (stream) {
        h.global_first_index = u64(&b[104]);
        std::copy(b.begin() + 112, b.begin() + 128, h.session_id.begin());
        h.segment_index = u64(&b[128]); h.session_total_records = u64(&b[136]);
        h.segment_target_records = u32(&b[144]); h.stream_flags = u32(&b[148]);
        require(u32(&b[152]) == 4096 && !(h.stream_flags & ~1u), "Unsupported stream header extension");
        require(std::any_of(h.session_id.begin(), h.session_id.end(), [](std::uint8_t x) { return x != 0; }), "Missing stream session identity");
        require(h.segment_target_records > 0 && h.segment_target_records <= std::uint64_t(h.sample_rate_hz) * 40 &&
                h.completed_records <= h.segment_target_records, "Invalid stream segment target");
        require(h.global_first_index <= std::numeric_limits<std::uint64_t>::max() - h.completed_records,
                "Stream index overflow");
        require(!h.session_total_records || h.global_first_index + h.completed_records <= h.session_total_records,
                "Stream segment exceeds session total");
        for (std::size_t j = 156; j < b.size(); ++j) require(b[j] == 0, "Nonzero reserved stream header field");
    }
    std::fill(b.begin() + 100, b.begin() + 104, 0);
    require(crc32(b.data(), stream ? b.size() : 128) == h.header_crc32, "Header CRC mismatch");
    require(h.version == (stream ? 1u : 2u) && h.header_bytes == 4096 && h.record_bytes == 32, "Unsupported protocol layout");
    require(h.state == 2 && h.error_code == 0 && h.dropped_records == 0, "Capture is not READY/successful");
    require(h.completed_records > 0 && h.completed_records == h.requested_records, "Incomplete record count");
    require(std::uint64_t(h.completed_records) * 32 == h.payload_bytes &&
            h.payload_bytes <= kFirmwareMaxPayload && bytes == 4096ULL + h.payload_bytes, "File length mismatch");
    require(h.clock_hz == 50000000 && h.sample_rate_hz > 0 &&
            h.sample_rate_hz <= 100000 && h.clock_hz % h.sample_rate_hz == 0,
            "Unsupported protocol clock/sample rate");
    require(h.channel_map == 0x00050301, "Unsupported channel mapping; expected V1/V3/V5");
    require(h.adc_range_uv > 0 && h.adc_range_uv <= 10000000, "Invalid ADC input range");
    if (!stream) for (std::size_t j = 104; j < 128; ++j) require(b[j] == 0, "Nonzero reserved header field");
    const auto step = std::uint64_t(h.clock_hz / h.sample_rate_hz);
    require(std::uint64_t(h.completed_records - 1) <=
            (std::numeric_limits<std::uint64_t>::max() - h.first_tick) / step, "Timestamp overflow");
    require(h.last_tick == h.first_tick + std::uint64_t(h.completed_records - 1) * step, "Final timestamp mismatch");
    capture.records.reserve(h.completed_records);
    std::array<std::uint8_t, 32> r{};
    auto crc = 0xffffffffu;
    for (std::uint32_t i = 0; i < h.completed_records; ++i) {
        in.read(reinterpret_cast<char*>(r.data()), r.size());
        if (!in) throw AnalysisError("Truncated record at " + std::to_string(i));
        capture_sha.update(r.data(),r.size());
        crc = crc_update(crc, r.data(), r.size());
        Record record;
        record.index = u64(&r[0]); record.tick = u64(&r[8]);
        record.raw = {i16(&r[16]), i16(&r[18]), i16(&r[20])};
        record.flags = u16(&r[22]); record.speed_period_ticks = u32(&r[24]);
        record.speed_edge_count = u32(&r[28]);
        if (record.index != h.global_first_index + i) throw AnalysisError("Record index discontinuity at " + std::to_string(i));
        if (record.tick != h.first_tick + std::uint64_t(i) * step)
            throw AnalysisError("Sample timestamp discontinuity at " + std::to_string(i));
        if (!(record.flags & 1u) || (record.flags & ~31u)) throw AnalysisError("Invalid ADC/flags at " + std::to_string(i));
        if ((record.flags & 2u) && !(record.speed_period_ticks > 0 && !(record.flags & 4u)))
            throw AnalysisError("Inconsistent SPEED status at " + std::to_string(i));
        capture.records.push_back(record);
    }
    require((crc ^ 0xffffffffu) == h.payload_crc32, "Payload CRC mismatch");
    capture.capture_sha256=capture_sha.hex_digest();
    capture.source_metadata = source_metadata(std::filesystem::u8path(path));
    // The combined capture IP records its actual TEST source in STATUS bit 6.
    // Legacy vendor STATUS reserves this bit as zero. Keep sidecar status
    // compatible, but never let a missing/false sidecar erase a known TEST bit
    // from the CRC-verified file header.
    if ((!h.stream_segment && (h.hw_status & (1u << 6))) || (h.stream_flags & 1u)) capture.source_metadata.synthetic = true;
    capture.integrity_verified = true;
    return capture;
}

std::array<Spectrum, 3> analyze_voltage_spectra(const std::vector<Record>& records,
        std::uint32_t fs, std::uint32_t clock, std::int32_t range_uv, std::size_t requested_fft, double max_hz) {
    require(clock == 50000000 && fs > 0 && fs <= 100000 && clock % fs == 0, "Invalid live spectrum timebase");
    require(range_uv > 0 && range_uv <= 10000000, "Invalid live spectrum ADC range");
    require(records.size() >= 16 && records.size() <= 32768 && records.size() <= std::uint64_t(fs) * 2,
            "Live spectrum snapshot must contain 16..32768 records and no more than 2 seconds");
    require(power_of_two(requested_fft) && requested_fft >= 16 && requested_fft <= 16384,
            "Live spectrum FFT must be a power of two in [16,16384]");
    require(std::isfinite(max_hz) && max_hz > 0, "Invalid live spectrum frequency limit");
    const auto step = std::uint64_t(clock / fs);
    require(records.size() - 1 <= (std::numeric_limits<std::uint64_t>::max() - records.front().tick) / step,
            "Live spectrum timestamp overflow");
    require(records.size() - 1 <= std::numeric_limits<std::uint64_t>::max() - records.front().index,
            "Live spectrum index overflow");
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        require(r.index == records.front().index + i && r.tick == records.front().tick + i * step,
                "Live spectrum requires a contiguous raw snapshot");
        require((r.flags & 1u) && !(r.flags & ~31u) &&
                (!(r.flags & 2u) || (r.speed_period_ticks > 0 && !(r.flags & 4u))), "Invalid live spectrum record flags");
    }
    // Reuse the numerical helpers without asserting Capture::integrity_verified
    // or invoking the file-analysis/diagnosis path.
    Capture input; input.records = records;
    const auto n = std::min(requested_fft, floor_power_two(records.size()));
    const SpectrumPlan plan(n); const double scale = double(range_uv) / 1e6 / 32768.0;
    std::array<Spectrum, 3> output;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        auto& sp = output[channel]; sp.fft_size = n; sp.bin_width_hz = double(fs) / n;
        sp.window_duration_s = double(n) / fs; sp.enbw_hz = fs * plan.sum_w2 / (plan.sum_w * plan.sum_w);
        std::vector<double> energy(n / 2 + 1, 0.0);
        accumulate_segments(input, channel, scale, plan, 1, energy, sp.segment_count);
        const auto final = std::min(n / 2, std::size_t(std::floor(std::min(max_hz, fs / 2.0) / sp.bin_width_hz)));
        for (std::size_t k = 0; k <= final; ++k) {
            const double side = k == 0 || k == n / 2 ? 1 : 2;
            const double averaged = energy[k] / double(sp.segment_count);
            sp.frequency_hz.push_back(k * sp.bin_width_hz);
            sp.peak_amplitude.push_back(side * std::sqrt(averaged) / plan.sum_w);
            sp.psd.push_back(side * averaged / (fs * plan.sum_w2));
        }
    }
    return output;
}

ChannelCalibration effective_channel_calibration(const AnalysisConfig& cfg, std::size_t channel) {
    require(channel < 3, "Calibration channel index must be in [0,2]");
    if (cfg.channel_calibration) return (*cfg.channel_calibration)[channel];
    return {1000.0 * cfg.sensor_sensitivity_v_per_g / standard_gravity_m_s2, cfg.analog_gain, 0.0};
}

void validate_calibration_config(const AnalysisConfig& cfg) {
    require(cfg.amplitude_unit == AmplitudeUnit::Voltage || cfg.amplitude_unit == AmplitudeUnit::AccelerationSI ||
            cfg.amplitude_unit == AmplitudeUnit::Gravity, "Invalid amplitude unit");
    if (!cfg.channel_calibration) {
        require(std::isfinite(cfg.sensor_sensitivity_v_per_g) && cfg.sensor_sensitivity_v_per_g > 0,
                "Sensor sensitivity must be finite and positive");
        require(std::isfinite(cfg.analog_gain) && cfg.analog_gain != 0.0, "Analog gain must be finite and nonzero");
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto c = effective_channel_calibration(cfg, channel);
        require(std::isfinite(c.sensitivity_mv_per_m_s2) && c.sensitivity_mv_per_m_s2 > 0,
                "Channel sensitivity must be finite and positive");
        require(std::isfinite(c.analog_gain) && c.analog_gain != 0.0, "Channel analog gain must be finite and nonzero");
        require(std::isfinite(c.voltage_bias_v), "Channel voltage bias must be finite");
        const auto slope = 1000.0 / c.analog_gain / c.sensitivity_mv_per_m_s2;
        require(std::isfinite(slope) && slope != 0.0 && std::isfinite(c.voltage_bias_v * slope),
                "Configured conversion is outside supported numerical range");
    }
}

double voltage_to_amplitude(double voltage, std::size_t channel, const AnalysisConfig& cfg) {
    validate_calibration_config(cfg);
    require(channel < 3 && std::isfinite(voltage), "Invalid voltage or channel for conversion");
    if (cfg.amplitude_unit == AmplitudeUnit::Voltage) return voltage;
    // Retain the legacy arithmetic when no per-channel parameters are supplied.
    if (!cfg.channel_calibration) {
        const auto g = voltage / (cfg.analog_gain * cfg.sensor_sensitivity_v_per_g);
        const auto result = cfg.amplitude_unit == AmplitudeUnit::Gravity ? g : g * standard_gravity_m_s2;
        require(std::isfinite(result), "Converted amplitude is outside supported numerical range");
        return result;
    }
    const auto c = effective_channel_calibration(cfg, channel);
    const auto acceleration = 1000.0 * (voltage - c.voltage_bias_v) / c.analog_gain / c.sensitivity_mv_per_m_s2;
    const auto result = cfg.amplitude_unit == AmplitudeUnit::Gravity ? acceleration / standard_gravity_m_s2 : acceleration;
    require(std::isfinite(result), "Converted amplitude is outside supported numerical range");
    return result;
}

double adc_to_amplitude(std::int16_t raw, std::int32_t adc_range_uv, std::size_t channel, const AnalysisConfig& cfg) {
    require(adc_range_uv > 0, "ADC range must be positive");
    return voltage_to_amplitude(double(raw) * (double(adc_range_uv) / 1e6 / 32768.0), channel, cfg);
}

std::string amplitude_unit_symbol(const AnalysisConfig& cfg) {
    switch (cfg.amplitude_unit) {
    case AmplitudeUnit::Voltage: return "V";
    case AmplitudeUnit::AccelerationSI: return "m/s^2";
    case AmplitudeUnit::Gravity: return "g";
    }
    throw AnalysisError("Invalid amplitude unit");
}

bool calibration_config_equal(const AnalysisConfig& a, const AnalysisConfig& b) {
    validate_calibration_config(a); validate_calibration_config(b);
    if (a.amplitude_unit != b.amplitude_unit || bool(a.channel_calibration) != bool(b.channel_calibration)) return false;
    if (!a.channel_calibration)
        return a.sensor_sensitivity_v_per_g == b.sensor_sensitivity_v_per_g && a.analog_gain == b.analog_gain;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto x = effective_channel_calibration(a, channel), y = effective_channel_calibration(b, channel);
        if (x.sensitivity_mv_per_m_s2 != y.sensitivity_mv_per_m_s2 || x.analog_gain != y.analog_gain ||
            x.voltage_bias_v != y.voltage_bias_v) return false;
    }
    return true;
}

AnalysisResult analyze(const Capture& c, const AnalysisConfig& config) {
    auto cfg = config;
    const bool hardware_test_source = !c.header.stream_segment && (c.header.hw_status & (1u << 6)) != 0;
    if (c.source_metadata.synthetic || hardware_test_source) cfg.signal_kind = SignalKind::TestSignal;
    else if (c.source_metadata.status == "invalid_or_unreadable_sidecar") cfg.signal_kind = SignalKind::Unknown;
    require(c.integrity_verified, "Analyze only a capture returned by load_capture after validation");
    require(!c.records.empty() && c.records.size() == c.header.completed_records, "Capture record count inconsistent");
    require(c.header.sample_rate_hz > 0 && c.header.clock_hz > 0, "Invalid analysis timebase");
    validate_calibration_config(cfg);
    require(power_of_two(cfg.fft_size) && cfg.fft_size >= 16 && cfg.fft_size <= 262144,
            "FFT size must be a power of two in [16,262144]");
    require(std::isfinite(cfg.spectrum_max_hz) && cfg.spectrum_max_hz > 0, "Invalid spectrum display limit");
    require(cfg.waveform_points >= 12 && cfg.waveform_points <= 10000, "Waveform point budget must be in [12,10000]");
    require(cfg.execution_threads >= 1 && cfg.execution_threads <= 2, "Execution thread count must be 1 or 2");
    if (cfg.ppr_override) require(std::isfinite(*cfg.ppr_override) && *cfg.ppr_override >= 1e-6 && *cfg.ppr_override <= 1e9,
                                  "PPR override must be finite and in [0.000001,1000000000]");
    const auto count = c.records.size();
    const auto fs = double(c.header.sample_rate_hz);
    const auto adc_volts = double(c.header.adc_range_uv) / 1e6 / 32768.0;
    std::array<double, 3> scales{}, offsets{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        offsets[channel] = voltage_to_amplitude(0.0, channel, cfg);
        scales[channel] = voltage_to_amplitude(adc_volts, channel, cfg) - offsets[channel];
        // Avoid cancellation at a user-supplied large bias, and preserve exact
        // legacy arithmetic for pre-existing analysis and threshold fixtures.
        if (cfg.amplitude_unit == AmplitudeUnit::Voltage) scales[channel] = adc_volts;
        else if (!cfg.channel_calibration) {
            scales[channel] = adc_volts / (cfg.analog_gain * cfg.sensor_sensitivity_v_per_g);
            if (cfg.amplitude_unit == AmplitudeUnit::AccelerationSI) scales[channel] *= standard_gravity_m_s2;
        } else {
            const auto p = effective_channel_calibration(cfg, channel);
            scales[channel] = adc_volts * 1000.0 / p.analog_gain / p.sensitivity_mv_per_m_s2;
            if (cfg.amplitude_unit == AmplitudeUnit::Gravity) scales[channel] /= standard_gravity_m_s2;
        }
        require(std::isfinite(scales[channel]) && scales[channel] != 0.0 && std::abs(scales[channel]) <= 1e6 &&
                std::abs(offsets[channel]) <= 1e100, "Configured conversion scale is outside supported numerical range");
    }
    AnalysisResult out;
    out.source_path = c.source_path; out.header = c.header; out.config = cfg;
    out.source_metadata = c.source_metadata;
    out.source_metadata.synthetic = out.source_metadata.synthetic || hardware_test_source;
    if (out.source_metadata.synthetic) out.warnings.push_back("Synthetic source declaration or hardware TEST header: development signal only; this record is not a physical machine measurement.");
    if (out.source_metadata.status == "invalid_or_unreadable_sidecar")
        out.warnings.push_back("Source sidecar is invalid or unreadable; source confirmation is disabled until it is resolved.");
    out.duration_s = double(count) / fs;
    out.amplitude_unit = amplitude_unit_symbol(cfg);
    if (!cfg.calibration_confirmed && cfg.amplitude_unit != AmplitudeUnit::Voltage) {
        out.amplitude_unit += "_nominal_uncalibrated";
        out.warnings.push_back("Acceleration uses configured nominal sensitivity/gain; calibration is not confirmed.");
    }
    if (cfg.channel_calibration)
        out.warnings.push_back("Per-channel sensitivity, signed gain and voltage bias are user-entered conversion parameters, not independent evidence of physical calibration.");
    out.warnings.push_back("V1/V3/V5 are storage channels; physical X/Y/Z direction requires a verified wiring record.");
    out.warnings.push_back("The file proves digital continuity, not sensor validity or analog anti-alias performance.");
    auto& speed = out.speed;
    if (c.header.pulses_per_rev) {
        if (cfg.ppr_override) require(std::abs(*cfg.ppr_override - double(c.header.pulses_per_rev)) < 1e-9,
                                     "Configured PPR conflicts with nonzero file PPR");
        speed.pulses_per_rev = double(c.header.pulses_per_rev); speed.ppr_source = "file_header";
    } else if (cfg.ppr_override) {
        speed.pulses_per_rev = *cfg.ppr_override; speed.ppr_source = "explicit_user_override";
    } else speed.ppr_source = "unknown";
    std::size_t valid = 0, timeout = 0, fresh = 0;
    double mean_hz = 0, m2_hz = 0, fresh_hz_sum = 0;
    double min_hz = std::numeric_limits<double>::infinity(), max_hz = 0;
    const auto trace_stride = std::max<std::size_t>(1, (count + cfg.waveform_points - 1) / cfg.waveform_points);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& r = c.records[i];
        if (r.flags & 4u) ++timeout;
        std::optional<double> hz;
        if (valid_speed(r)) {
            hz = double(c.header.clock_hz) / double(r.speed_period_ticks);
            ++valid;
            const auto delta = *hz - mean_hz;
            mean_hz += delta / double(valid); m2_hz += delta * (*hz - mean_hz);
            min_hz = std::min(min_hz, *hz); max_hz = std::max(max_hz, *hz);
            if (i && std::uint32_t(r.speed_edge_count - c.records[i - 1].speed_edge_count) == 1) {
                ++fresh; fresh_hz_sum += *hz;
            }
        }
        if (i && r.speed_edge_count != c.records[i - 1].speed_edge_count)
            speed.observable_new_edges += std::uint32_t(r.speed_edge_count - c.records[i - 1].speed_edge_count);
    }
    // A fixed-stride snapshot can erase a brief timeout and draw a continuous
    // speed line over it. Preserve each bucket's endpoints, its first/last
    // missing point and the valid neighbours of that span (six points maximum).
    // If gaps alternate faster than this display budget, hide valid fragments
    // enclosed by missing runs. Statistics above still use every original row.
    std::optional<std::size_t> last_trace_index;
    const auto append_trace = [&](std::size_t i) {
        if (last_trace_index && *last_trace_index == i) return;
        SpeedPoint point; point.time_s = double(i) / fs;
        if (valid_speed(c.records[i])) {
            point.pulse_frequency_hz = double(c.header.clock_hz) / double(c.records[i].speed_period_ticks);
            if (speed.pulses_per_rev) point.rpm = *point.pulse_frequency_hz * 60.0 / *speed.pulses_per_rev;
        }
        speed.trace.push_back(point);
        last_trace_index = i;
    };
    for (std::size_t begin = 0; begin < count; begin += trace_stride) {
        const auto end = std::min(count, begin + trace_stride);
        auto first_missing = end, last_missing = end;
        std::size_t missing_runs = 0;
        bool previous_valid = true;
        for (std::size_t i = begin; i < end; ++i) {
            const bool current_valid = valid_speed(c.records[i]);
            if (!current_valid) {
                if (first_missing == end) first_missing = i;
                last_missing = i;
                if (previous_valid) ++missing_runs;
            }
            previous_valid = current_valid;
        }
        append_trace(begin);
        if (first_missing != end) {
            if (first_missing > begin) append_trace(first_missing - 1);
            append_trace(first_missing);
            append_trace(last_missing);
            if (last_missing + 1 < end) append_trace(last_missing + 1);
            if (missing_runs > 1) speed.trace_gaps_coalesced = true;
        }
        append_trace(end - 1);
    }
    if (speed.trace_gaps_coalesced)
        out.warnings.push_back("Speed display merges multiple missing runs within a display bucket; intervening valid fragments are hidden. Full-record statistics are unchanged.");
    speed.valid_fraction = double(valid) / double(count); speed.timeout_fraction = double(timeout) / double(count);
    if (valid) {
        speed.held_mean_pulse_hz = mean_hz;
        if (speed.pulses_per_rev) {
            const auto factor = 60.0 / *speed.pulses_per_rev;
            speed.held_mean_rpm = mean_hz * factor;
            speed.held_std_rpm = std::sqrt(m2_hz / double(valid)) * factor;
            speed.min_rpm = min_hz * factor; speed.max_rpm = max_hz * factor;
            if (fresh) speed.fresh_period_mean_rpm = fresh_hz_sum / double(fresh) * factor;
        }
    }
    if (!speed.pulses_per_rev) out.warnings.push_back("PPR is unknown; only pulse Hz is available and RPM/order features are disabled.");
    if (speed.valid_fraction < 0.9) out.warnings.push_back("Less than 90 percent of SPEED snapshots are valid; order features are disabled.");
    std::optional<double> rotation_hz;
    if (speed.held_mean_rpm && speed.valid_fraction >= 0.9 && *speed.held_mean_rpm > 0 &&
        speed.held_std_rpm && *speed.held_std_rpm / *speed.held_mean_rpm <= 0.05)
        rotation_hz = *speed.held_mean_rpm / 60.0;
    else if (speed.held_mean_rpm && speed.valid_fraction >= 0.9)
        out.warnings.push_back("Speed variation exceeds 5 percent; fixed-frequency order bands are disabled. Order tracking is not implemented.");
    const auto n = std::min(cfg.fft_size, floor_power_two(count));
    // Construct once per analysis. All segment workers see an immutable plan;
    // there is no mutable global cache or cross-analysis lifetime dependency.
    const std::unique_ptr<const SpectrumPlan> plan = n >= 16 ? std::make_unique<SpectrumPlan>(n) : nullptr;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto scale = scales[axis], offset = offsets[axis];
        auto& stats = out.time_stats[axis];
        double mean = 0, sum_sq = 0;
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (std::size_t i = 0; i < count; ++i) {
            const auto x = value(c, i, axis, scale) + offset;
            mean += (x - mean) / double(i + 1); sum_sq += x * x;
            low = std::min(low, x); high = std::max(high, x);
            if (c.records[i].raw[axis] == -32768 || c.records[i].raw[axis] == 32767) ++stats.adc_endpoint_count;
        }
        double second = 0, fourth = 0, ac_peak = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto x = value(c, i, axis, scale) + offset - mean;
            second += x * x; fourth += x * x * x * x; ac_peak = std::max(ac_peak, std::abs(x));
        }
        stats.mean = mean; stats.rms = std::sqrt(sum_sq / double(count));
        stats.ac_rms = std::sqrt(second / double(count)); stats.abs_peak = std::max(std::abs(low), std::abs(high));
        stats.peak_to_peak = high - low;
        if (stats.ac_rms > std::abs(scale) * 0.5) {
            stats.crest_factor = ac_peak / stats.ac_rms;
            stats.pearson_kurtosis = (fourth / double(count)) / std::pow(second / double(count), 2);
        }
        auto& sp = out.spectra[axis];
        if (n >= 16) {
            sp.fft_size = n; sp.bin_width_hz = fs / double(n); sp.window_duration_s = double(n) / fs;
            std::vector<double> energy(n / 2 + 1, 0.0);
            const auto sum_w = plan->sum_w, sum_w2 = plan->sum_w2;
            sp.enbw_hz = fs * sum_w2 / (sum_w * sum_w);
            // FFT preprocessing explicitly removes each segment mean. The
            // fixed configured bias cancels here; no time waveform is demeaned.
            accumulate_segments(c, axis, scale, *plan, cfg.execution_threads, energy, sp.segment_count);
            const auto max_bin = std::min(n / 2, static_cast<std::size_t>(std::floor(std::min(cfg.spectrum_max_hz, fs / 2) / sp.bin_width_hz)));
            for (std::size_t k = 0; k <= max_bin; ++k) {
                const auto side = (k == 0 || k == n / 2) ? 1.0 : 2.0;
                const auto averaged = energy[k] / double(sp.segment_count);
                sp.frequency_hz.push_back(double(k) * sp.bin_width_hz);
                sp.peak_amplitude.push_back(side * std::sqrt(averaged) / sum_w);
                sp.psd.push_back(side * averaged / (fs * sum_w2));
            }
            for (int order = 1; order <= 3; ++order) {
                auto& band = out.order_bands[axis][order - 1]; band.order = order;
                if (!rotation_hz) { band.status = "speed_missing_or_unstable"; continue; }
                const auto center = *rotation_hz * double(order), half = 1.5 * sp.bin_width_hz;
                band.center_hz = center; band.half_width_hz = half;
                if (center - half <= 0 || center + half > fs / 2 || 2.0 * half >= *rotation_hz) {
                    band.status = "insufficient_frequency_resolution"; continue;
                }
                double integral = 0;
                for (std::size_t k = 1; k < energy.size(); ++k) {
                    if (std::abs(double(k) * sp.bin_width_hz - center) > half) continue;
                    const auto side = k == n / 2 ? 1.0 : 2.0;
                    integral += side * energy[k] / double(sp.segment_count) / (fs * sum_w2) * sp.bin_width_hz;
                }
                band.rms = std::sqrt(integral); band.status = "measured_band_rms";
            }
        } else {
            for (int order = 1; order <= 3; ++order) {
                out.order_bands[axis][order - 1].order = order;
                out.order_bands[axis][order - 1].status = "too_few_samples";
            }
        }
    }
    // Display-only min/max selection; all analysis above uses original samples.
    std::set<std::size_t> indices{0, count - 1};
    const auto buckets = std::max<std::size_t>(1, (cfg.waveform_points - 2) / 6);
    for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
        const auto begin = count * bucket / buckets, end = count * (bucket + 1) / buckets;
        if (begin == end) continue;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            auto lo = begin, hi = begin;
            for (std::size_t j = begin + 1; j < end; ++j) {
                if (c.records[j].raw[axis] < c.records[lo].raw[axis]) lo = j;
                if (c.records[j].raw[axis] > c.records[hi].raw[axis]) hi = j;
            }
            indices.insert(lo); indices.insert(hi);
        }
    }
    for (auto i : indices) out.waveform.push_back({double(i) / fs,
        {value(c, i, 0, scales[0]) + offsets[0], value(c, i, 1, scales[1]) + offsets[1], value(c, i, 2, scales[2]) + offsets[2]}});
    auto& d = out.diagnosis;
    d.classification = "not_classified";
    if (cfg.signal_kind != SignalKind::VibrationConfirmed) d.status = "not_a_confirmed_vibration_record";
    else if (!cfg.calibration_confirmed) d.status = "awaiting_calibration_and_baseline";
    else d.status = "awaiting_labeled_baseline_and_validation";
    d.evidence.push_back(std::string(c.header.stream_segment ? "CAPSTRM1 segment" : "CAPDDR2") + " integrity, index continuity and sample timestamps passed validation.");
    d.evidence.push_back("Time-domain features and window-averaged spectra are measured from this record.");
    if (rotation_hz) d.evidence.push_back("1X/2X/3X band energy is referenced to measured SPEED and the stated PPR.");
    if (std::any_of(out.time_stats.begin(), out.time_stats.end(), [](const TimeStats& s) { return s.adc_endpoint_count > 0; })) {
        d.status = "adc_saturation_requires_review";
        d.evidence.push_back("One or more channels contain ADC endpoint codes; inspect saturation before interpretation.");
    }
    d.limitations.push_back("No trained classifier or validated machine-specific thresholds are installed; no fault category or accuracy is claimed.");
    d.limitations.push_back("A 1X or 2X peak alone cannot uniquely identify imbalance, misalignment or looseness.");
    d.limitations.push_back("Envelope analysis and angle resampling are not implemented in this first analysis core.");
    return out;
}

AnalysisResult analyze_file(const std::string& path, const AnalysisConfig& config, const LoadOptions& options) {
    return analyze(load_capture(path, options), config);
}

std::string to_json(const AnalysisResult& r) {
    std::ostringstream o; o.imbue(std::locale::classic()); o << std::setprecision(12);
    o << "{\"schema\":\"zynq_vibration.analysis.v1\",\"source_path\":" << quote(r.source_path)
      << ",\"source_metadata\":{\"synthetic\":" << (r.source_metadata.synthetic ? "true" : "false")
      << ",\"status\":" << quote(r.source_metadata.status) << '}'
      << ",\"integrity\":\"passed\",\"channel_adc_inputs\":[1,3,5],\"header\":{"
      << "\"protocol_version\":" << r.header.version << ",\"sample_rate_hz\":" << r.header.sample_rate_hz
      << ",\"clock_hz\":" << r.header.clock_hz << ",\"records\":" << r.header.completed_records
      << ",\"payload_bytes\":" << r.header.payload_bytes << ",\"first_tick\":" << r.header.first_tick
      << ",\"last_tick\":" << r.header.last_tick << ",\"pulses_per_rev\":" << r.header.pulses_per_rev
      << ",\"adc_range_uv\":" << r.header.adc_range_uv
      << ",\"format\":" << quote(r.header.stream_segment ? "CAPSTRM1" : "CAPDDR2")
      << ",\"global_first_index\":" << r.header.global_first_index << ",\"segment_index\":" << r.header.segment_index
      << ",\"session_total_records\":" << r.header.session_total_records << "},\"duration_s\":" << r.duration_s
      << ",\"amplitude_unit\":" << quote(r.amplitude_unit) << ",\"configuration\":{\"signal_kind\":"
      << quote(signal_name(r.config.signal_kind)) << ",\"sensor_sensitivity_v_per_g\":" << r.config.sensor_sensitivity_v_per_g
      << ",\"analog_gain\":" << r.config.analog_gain << ",\"calibration_confirmed\":" << (r.config.calibration_confirmed ? "true" : "false")
      << ",\"amplitude_unit\":" << quote(amplitude_unit_symbol(r.config))
      << ",\"calibration_parameter_source\":\"user_configuration_not_measurement\""
      << ",\"channel_calibration\":";
    if (!r.config.channel_calibration) o << "null";
    else {
        o << '[';
        for (std::size_t channel = 0; channel < 3; ++channel) {
            if (channel) o << ',';
            const auto p = effective_channel_calibration(r.config, channel);
            o << "{\"channel\":" << channel + 1 << ",\"sensitivity_mv_per_m_s2\":" << p.sensitivity_mv_per_m_s2
              << ",\"analog_gain\":" << p.analog_gain << ",\"voltage_bias_v\":" << p.voltage_bias_v << '}';
        }
        o << ']';
    }
    o << ",\"spectrum_preprocessing\":\"per_segment_mean_removal\",\"time_domain_dc_removal\":false"
      << ",\"ppr_override\":"; optional(o, r.config.ppr_override);
    o << "},\"time_stats\":[";
    for (std::size_t a = 0; a < 3; ++a) {
        if (a) o << ',';
        const auto& s = r.time_stats[a];
        o << "{\"channel\":" << a + 1 << ",\"mean\":" << s.mean << ",\"rms\":" << s.rms
          << ",\"ac_rms\":" << s.ac_rms << ",\"abs_peak\":" << s.abs_peak << ",\"peak_to_peak\":" << s.peak_to_peak
          << ",\"crest_factor\":"; optional(o, s.crest_factor); o << ",\"pearson_kurtosis\":"; optional(o, s.pearson_kurtosis);
        o << ",\"adc_endpoint_count\":" << s.adc_endpoint_count << '}';
    }
    o << "],\"spectra\":[";
    for (std::size_t a = 0; a < 3; ++a) {
        if (a) o << ',';
        const auto& s = r.spectra[a];
        o << "{\"channel\":" << a + 1 << ",\"fft_size\":" << s.fft_size << ",\"segment_count\":" << s.segment_count
          << ",\"bin_width_hz\":" << s.bin_width_hz << ",\"window_duration_s\":" << s.window_duration_s
          << ",\"enbw_hz\":" << s.enbw_hz << ",\"window\":\"periodic_hann\",\"frequency_hz\":";
        doubles(o, s.frequency_hz); o << ",\"peak_amplitude\":"; doubles(o, s.peak_amplitude);
        o << ",\"psd\":"; doubles(o, s.psd); o << '}';
    }
    o << "],\"order_bands\":[";
    for (std::size_t a = 0; a < 3; ++a) {
        if (a) o << ',';
        o << '[';
        for (std::size_t j = 0; j < 3; ++j) {
            if (j) o << ',';
            const auto& b = r.order_bands[a][j];
            o << "{\"order\":" << b.order << ",\"center_hz\":"; optional(o, b.center_hz);
            o << ",\"half_width_hz\":"; optional(o, b.half_width_hz); o << ",\"rms\":"; optional(o, b.rms);
            o << ",\"status\":" << quote(b.status) << '}';
        }
        o << ']';
    }
    const auto& s = r.speed;
    o << "],\"speed\":{\"pulses_per_rev\":"; optional(o, s.pulses_per_rev);
    o << ",\"ppr_source\":" << quote(s.ppr_source) << ",\"valid_fraction\":" << s.valid_fraction
      << ",\"timeout_fraction\":" << s.timeout_fraction << ",\"observable_new_edges\":" << s.observable_new_edges
      << ",\"held_mean_pulse_hz\":"; optional(o, s.held_mean_pulse_hz);
    o << ",\"held_mean_rpm\":"; optional(o, s.held_mean_rpm);
    o << ",\"held_std_rpm\":"; optional(o, s.held_std_rpm);
    o << ",\"fresh_period_mean_rpm\":"; optional(o, s.fresh_period_mean_rpm);
    o << ",\"min_rpm\":"; optional(o, s.min_rpm); o << ",\"max_rpm\":"; optional(o, s.max_rpm);
    o << ",\"trace_gap_policy\":\"bucket_missing_span_preserved\",\"trace_gaps_coalesced\":"
      << (s.trace_gaps_coalesced ? "true" : "false") << ",\"trace\":[";
    for (std::size_t i = 0; i < s.trace.size(); ++i) {
        if (i) o << ',';
        const auto& p = s.trace[i];
        o << "{\"time_s\":" << p.time_s << ",\"pulse_frequency_hz\":"; optional(o, p.pulse_frequency_hz);
        o << ",\"rpm\":"; optional(o, p.rpm); o << '}';
    }
    o << "]},\"waveform\":[";
    for (std::size_t i = 0; i < r.waveform.size(); ++i) {
        if (i) o << ',';
        const auto& p = r.waveform[i];
        o << "{\"time_s\":" << p.time_s << ",\"values\":[" << p.values[0] << ',' << p.values[1] << ',' << p.values[2] << "]}";
    }
    o << "],\"diagnosis\":{\"status\":" << quote(r.diagnosis.status) << ",\"classification\":" << quote(r.diagnosis.classification)
      << ",\"evidence\":"; strings(o, r.diagnosis.evidence);
    o << ",\"limitations\":"; strings(o, r.diagnosis.limitations); o << "},\"warnings\":"; strings(o, r.warnings);
    o << '}'; return o.str();
}
} // namespace vibration
