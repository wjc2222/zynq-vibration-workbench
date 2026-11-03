#include "capture_output.hpp"
#include "capture_profile.hpp"
#include <QJsonDocument>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#ifdef __linux__
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
namespace diag=vibration::capture_diag;

struct CaptureOutput::Impl {
    diag::Profile profile;
    const bool widePipe=qgetenv("AX7_CAPTURE_WIDE_PIPE")=="1";
    int pipeBefore=-1,pipeAfter=-1,pipeError=0;
    bool pipeQueried=false,pipeResizeAttempted=false;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<QJsonObject> critical;
    std::optional<QJsonObject> status,preview;
    bool stopping=false;
    std::chrono::steady_clock::time_point deadline;
    std::thread writer;
    QString previousKey;
    quint32 previousEnd=0;
#ifdef __linux__
    int originalFlags=-1;
#endif
    QJsonObject snapshot()const{
        auto value=profile.snapshot();
        value["stdout_pipe"]=QJsonObject{{"wide_opt_in",widePipe},{"requested_bytes",widePipe?262144:0},{"queried",pipeQueried},{"is_pipe",pipeBefore>=0},{"before_bytes",pipeBefore},{"after_bytes",pipeAfter},{"resize_attempted",pipeResizeAttempted},{"errno",pipeError}};
        return value;
    }
    void configurePipe(){
#if defined(__linux__) && defined(F_GETPIPE_SZ) && defined(F_SETPIPE_SZ)
        if(!widePipe&&!profile.enabled())return;
        const auto savedError=errno;pipeQueried=true;
        pipeBefore=::fcntl(STDOUT_FILENO,F_GETPIPE_SZ);pipeAfter=pipeBefore;
        if(pipeBefore<0)pipeError=errno;
        else if(widePipe){
            pipeResizeAttempted=true;
            if(::fcntl(STDOUT_FILENO,F_SETPIPE_SZ,262144)<0)pipeError=errno;
            const auto actual=::fcntl(STDOUT_FILENO,F_GETPIPE_SZ);
            if(actual>=0)pipeAfter=actual;else if(!pipeError)pipeError=errno;
        }
        errno=savedError;
#else
        if(widePipe)pipeError=ENOTSUP;
#endif
    }
    Impl(){
#ifdef __linux__
        std::signal(SIGPIPE,SIG_IGN);
        originalFlags=::fcntl(STDOUT_FILENO,F_GETFL);
        if(originalFlags>=0)(void)::fcntl(STDOUT_FILENO,F_SETFL,originalFlags|O_NONBLOCK);
#endif
        configurePipe();
        if(widePipe&&!profile.enabled())diag::emitDiagnostic({{"component","output"},{"phase","pipe_setup"},{"profile",snapshot()}});
        writer=std::thread([this]{run();});
    }
    ~Impl(){
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(750);}
        changed.notify_all();writer.join();
        if(profile.enabled())diag::emitDiagnostic({{"component","output"},{"phase","writer_shutdown"},{"profile",snapshot()}});
#ifdef __linux__
        if(originalFlags>=0)(void)::fcntl(STDOUT_FILENO,F_SETFL,originalFlags);
#endif
    }
    bool expired(){std::lock_guard<std::mutex> lock(mutex);return stopping&&std::chrono::steady_clock::now()>=deadline;}
    bool writeLine(const QByteArray& bytes){
        diag::Span writeTime(profile,diag::OutputWrite);
#ifdef __linux__
        qsizetype offset=0;
        while(offset<bytes.size()){
            if(expired())return false;
            profile.count(diag::WriteSyscalls);
            const auto remaining=std::size_t(bytes.size()-offset);
            const auto n=::write(STDOUT_FILENO,bytes.constData()+offset,remaining);
            if(n>0){if(std::size_t(n)<remaining)profile.count(diag::ShortWrites);offset+=qsizetype(n);profile.count(diag::OutputBytes,quint64(n));continue;}
            if(n<0&&errno==EINTR)continue;
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){profile.count(diag::WouldBlockWrites);diag::Span waitTime(profile,diag::OutputWait);pollfd p{STDOUT_FILENO,POLLOUT,0};(void)::poll(&p,1,20);continue;}
            return false;
        }
        return true;
#else
        std::cout.write(bytes.constData(),bytes.size());std::cout.flush();return bool(std::cout);
#endif
    }
    void run(){
        for(;;){
            QJsonObject object;
            {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[this]{return stopping||!critical.empty()||status||preview;});
                if(stopping&&(std::chrono::steady_clock::now()>=deadline||(critical.empty()&&!status&&!preview)))return;
                if(!critical.empty()){object=std::move(critical.front());critical.pop_front();}
                else if(status){object=std::move(*status);status.reset();}
                else if(preview){object=std::move(*preview);preview.reset();}
                else continue;
            }
            const bool live=object.value("event")=="preview";
            QString key;quint32 end=0;
            if(live){
                key=object.value("job_id").toString()+":"+object.value("generation").toString();
                const auto first=quint32(object.value("first_record").toDouble());
                const auto previous=key==previousKey?previousEnd:0;
                object["dropped_preview_records"]=double(first>=previous?first-previous:0);
                end=first+quint32(object.value("record_count").toInt());
            }
            QByteArray bytes;{diag::Span measured(profile,diag::OutputSerialize);bytes=QJsonDocument(object).toJson(QJsonDocument::Compact)+'\n';}
            profile.maximum(diag::MaxLineBytes,quint64(bytes.size()));
            if(bytes.size()-1>(live?196608:8192)){
                std::cerr<<"Capture output message exceeds protocol line limit; dropped\n";
                continue;
            }
            if(!writeLine(bytes))return;
            if(live){profile.count(diag::WrittenPreviews);previousKey=key;previousEnd=end;}
        }
    }
};
CaptureOutput::CaptureOutput():impl_(std::make_unique<Impl>()){}
CaptureOutput::~CaptureOutput()=default;
bool CaptureOutput::enqueue(const QJsonObject& object){
    diag::Span enqueueTime(impl_->profile,diag::QueueEnqueue);
    const bool preview=object.value("event")=="preview";
    // A preview is constructed internally from canonical base64. Bound its
    // large string and small metadata before storing, then serialize the full
    // frame only in the writer. The writer independently enforces the actual
    // UTF-8 line limit before any bytes are written. Queue memory stays bounded
    // even for an invalid internally supplied string; controls keep 8192B.
    if(preview){
        const auto encoded=object.value("raw_base64");
        if(!encoded.isString()||encoded.toString().size()>174764)return false;
        auto metadata=object;metadata.remove("raw_base64");
        if(QJsonDocument(metadata).toJson(QJsonDocument::Compact).size()>8192)return false;
    }else if(QJsonDocument(object).toJson(QJsonDocument::Compact).size()>8192)return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(preview){impl_->profile.count(diag::EnqueuedPreviews);if(impl_->preview)impl_->profile.count(diag::ReplacedPreviews);impl_->preview=object;}
    else if(object.value("event")=="job_status"){
        const auto state=object.value("state").toString();
        if(state=="completed"||state=="cancelled"||state=="failed"){
            if(impl_->preview)impl_->profile.count(diag::DiscardedPreviews);
            impl_->preview.reset();impl_->status.reset();
            if(impl_->critical.size()>=128)return false;
            impl_->critical.push_back(object);
        }else impl_->status=object;
    }else{
        if(impl_->critical.size()>=128)return false;
        impl_->critical.push_back(object);
    }
    impl_->changed.notify_one();return true;
}
void CaptureOutput::discardPreview(){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->preview.reset();}
QJsonObject CaptureOutput::transportProfile()const{return impl_->snapshot();}
