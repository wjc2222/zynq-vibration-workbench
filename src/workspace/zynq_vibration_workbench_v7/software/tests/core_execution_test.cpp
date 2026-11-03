#include "vibration/core.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::uint32_t reference_crc(const std::uint8_t* data, std::size_t size) {
    auto crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}
vibration::Capture capture(std::size_t count) {
    vibration::Capture result;
    result.integrity_verified = true; // Test-only in-memory fixture, not source provenance.
    result.source_path = "core execution test fixture";
    result.header.sample_rate_hz = 50000;
    result.header.clock_hz = 50000000;
    result.header.adc_range_uv = 5000000;
    result.header.pulses_per_rev = 1;
    result.header.completed_records = static_cast<std::uint32_t>(count);
    result.records.resize(count);
    std::uint32_t random = 0x13579bdfu;
    for (std::size_t i = 0; i < count; ++i) {
        auto& row = result.records[i];
        random = random * 1664525u + 1013904223u;
        row.index = i; row.tick = i * 1000;
        row.raw = {static_cast<std::int16_t>(30000 + int(i % 3) - 1),
                   static_cast<std::int16_t>(int(random >> 16) - 32768),
                   static_cast<std::int16_t>(i % 1009 == 0 ? -32768 : i % 997 == 0 ? 32767 : 0)};
        row.flags = 3; row.speed_period_ticks = 1500000; row.speed_edge_count = static_cast<std::uint32_t>(i / 1500);
    }
    return result;
}
void compare(const vibration::Capture& input, std::size_t fft_size) {
    vibration::AnalysisConfig config;
    config.fft_size = fft_size;
    config.execution_threads = 1;
    const auto serial = vibration::to_json(vibration::analyze(input, config));
    config.execution_threads = 2;
    for (int repetition = 0; repetition < 3; ++repetition)
        check(vibration::to_json(vibration::analyze(input, config)) == serial,
              "Serial/parallel complete JSON differs");
}
}
int main() {
    try {
        const std::array<std::uint8_t, 9> known{'1','2','3','4','5','6','7','8','9'};
        check(vibration::crc32(known.data(), known.size()) == 0xcbf43926u, "IEEE CRC32 known vector failed");
        check(vibration::crc32(nullptr, 0) == 0, "Empty CRC32 failed");
        std::vector<std::uint8_t> bytes(65544);
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>((i * 127) ^ (i >> 3));
        for (std::size_t offset = 0; offset < 8; ++offset) {
            for (std::size_t length = 0; length <= 512; ++length)
                check(vibration::crc32(bytes.data() + offset, length) == reference_crc(bytes.data() + offset, length),
                      "CRC32 alignment/tail mismatch");
            for (auto length : {4095u, 4096u, 4097u, 65535u, 65536u, 65537u})
                check(vibration::crc32(bytes.data() + offset, length) == reference_crc(bytes.data() + offset, length),
                      "CRC32 boundary mismatch");
        }
        for (auto count : {1u, 7u, 16u, 8191u, 8192u, 8193u, 20000u}) compare(capture(count), 65536);
        auto full = capture(500000);
        for (auto fft_size : {16u, 1024u, 8192u, 16384u, 32768u, 65536u, 131072u, 262144u}) compare(full, fft_size);
        for (std::size_t i = 100; i < full.records.size(); i += 997) {
            full.records[i].flags = 5; full.records[i].speed_period_ticks = 0;
        }
        compare(full, 65536);
        for (unsigned invalid : {0u, 3u}) {
            vibration::AnalysisConfig config; config.execution_threads = invalid;
            bool rejected = false;
            try { vibration::analyze(full, config); } catch (const vibration::AnalysisError&) { rejected = true; }
            check(rejected, "Invalid execution worker count accepted");
        }
        std::cout << "PASS: CRC32 vectors/alignment/tails, bounded execution policy, exact serial/parallel JSON across FFT sizes/counts/gaps\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
