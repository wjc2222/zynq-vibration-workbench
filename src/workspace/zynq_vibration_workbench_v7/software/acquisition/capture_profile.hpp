#pragma once
#include <QJsonObject>
#include <QJsonDocument>
#include <QtGlobal>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vibration::capture_diag {
// Fixed-size, opt-in wall-time counters. Nested spans are inclusive and must
// not be added together. No timers, atomics or per-frame JSON when disabled.
enum Metric { Receive, JsonParse, UniqueKeys, PreviewVerify, Base64Materialize,
    Base64Decode, Base64Canonical, FrameCrc, Records, SignalDelivery,
    PreviewPoll, InfoIoctl, CopyIoctl, SliceCrc, PreviewRecords, BuildEvent,
    Base64Encode, PreviewEnqueue, QueueEnqueue, OutputSerialize, OutputWrite,
    OutputWait, Acquire, FinalHeader, FinalSave, StatusIoctl, FinalRead, MetricCount };
enum Count { InputBytes, InputMessages, AcceptedFrames, RejectedFrames,
    EnqueuedPreviews, ReplacedPreviews, WrittenPreviews, OutputBytes,
    CopiedRecords, DiscardedPreviews, WriteSyscalls, ShortWrites, WouldBlockWrites,
    MaxLineBytes, MaxInputChunkBytes, InputChunksLe64k, InputChunksOver64k, CountCount };
class Profile {
    struct Counter {std::atomic<quint64> calls{0},total{0},maximum{0};};
    std::array<Counter,MetricCount> metrics_;
    std::array<std::atomic<quint64>,CountCount> counts_{};
    bool enabled_=qgetenv("AX7_CAPTURE_PROFILE")=="1";
public:
    Profile(){for(auto& value:counts_)value.store(0,std::memory_order_relaxed);}
    bool enabled()const{return enabled_;}
    void count(Count metric,quint64 value=1){if(enabled_)counts_[metric].fetch_add(value,std::memory_order_relaxed);}
    void maximum(Count metric,quint64 value){if(enabled_){auto prior=counts_[metric].load(std::memory_order_relaxed);while(prior<value&&!counts_[metric].compare_exchange_weak(prior,value,std::memory_order_relaxed)){}}}
    void record(Metric metric,quint64 ns){
        auto& counter=metrics_[metric];counter.calls.fetch_add(1,std::memory_order_relaxed);counter.total.fetch_add(ns,std::memory_order_relaxed);
        auto prior=counter.maximum.load(std::memory_order_relaxed);while(prior<ns&&!counter.maximum.compare_exchange_weak(prior,ns,std::memory_order_relaxed)){}
    }
    QJsonObject snapshot()const{
        static constexpr const char* names[]{"receive_total","json_parse","unique_keys","preview_verify","base64_materialize","base64_decode","base64_canonical","frame_crc","record_validation","signal_delivery","preview_poll","preview_info_ioctl","preview_copy_ioctl","preview_slice_crc","preview_record_validation","preview_build_event","base64_encode","preview_enqueue","queue_enqueue","output_serialize","output_write","output_poll_wait","acquire_total","final_header_validation","final_save","capture_status_ioctl","final_payload_read"};
        static constexpr const char* countNames[]{"input_bytes","input_messages","accepted_frames","rejected_frames","enqueued_previews","replaced_previews","written_previews","output_bytes","copied_records","discarded_previews","write_syscalls","short_writes","would_block_writes","max_line_bytes","max_input_chunk_bytes","input_chunks_le64k","input_chunks_over64k"};
        QJsonObject metrics,counts;
        if(enabled_){
            for(int i=0;i<MetricCount;++i){const auto calls=metrics_[i].calls.load(std::memory_order_relaxed);if(calls)metrics[QLatin1String(names[i])]=QJsonObject{{"calls",double(calls)},{"total_ns",double(metrics_[i].total.load(std::memory_order_relaxed))},{"max_ns",double(metrics_[i].maximum.load(std::memory_order_relaxed))}};}
            for(int i=0;i<CountCount;++i){const auto n=counts_[i].load(std::memory_order_relaxed);if(n)counts[QLatin1String(countNames[i])]=double(n);}
        }
        return {{"enabled",enabled_},{"clock","steady wall time; nested spans inclusive"},{"metrics",metrics},{"counters",counts}};
    }
};
class Span {
    Profile* profile_;Metric metric_;std::chrono::steady_clock::time_point start_;
public:
    Span(Profile& profile,Metric metric):profile_(profile.enabled()?&profile:nullptr),metric_(metric){if(profile_){const auto error=errno;start_=std::chrono::steady_clock::now();errno=error;}}
    ~Span(){finish();}
    void finish(){if(profile_){const auto error=errno;profile_->record(metric_,quint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start_).count()));profile_=nullptr;errno=error;}}
    Span(const Span&)=delete;Span& operator=(const Span&)=delete;
};
inline void emitDiagnostic(QJsonObject value){
#ifdef __linux__
    auto bytes=QByteArray("AX7_CAPTURE_PROFILE ")+QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n';
    // One <=PIPE_BUF write is all-or-nothing on a nonblocking pipe. Diagnostic
    // consumers can disappear without ever blocking acquisition or teardown.
    if(bytes.size()>4096)return;
    const auto flags=::fcntl(STDERR_FILENO,F_GETFL);if(flags<0)return;
    if(::fcntl(STDERR_FILENO,F_SETFL,flags|O_NONBLOCK)<0)return;
    const auto written=::write(STDERR_FILENO,bytes.constData(),std::size_t(bytes.size()));(void)written;
    (void)::fcntl(STDERR_FILENO,F_SETFL,flags);
#else
    (void)value;
#endif
}
}
