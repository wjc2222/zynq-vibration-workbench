#include "stream_output.hpp"
#include <QJsonDocument>
#include <algorithm>
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
struct StreamOutput::Impl {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<QJsonObject> controls;
    std::optional<QJsonObject> preview;
    bool stopping=false, broken=false;
    QString failure;
    quint64 undeliverableControls=0,highControls=0;
    std::chrono::steady_clock::time_point deadline;
    std::thread writer;
#ifdef __linux__
    int originalFlags=-1;
#endif
    Impl() {
#ifdef __linux__
        std::signal(SIGPIPE,SIG_IGN);
        originalFlags=::fcntl(STDOUT_FILENO,F_GETFL);
        if(originalFlags>=0)(void)::fcntl(STDOUT_FILENO,F_SETFL,originalFlags|O_NONBLOCK);
#endif
        writer=std::thread([this]{run();});
    }
    void close() {
        if(!writer.joinable())return;
        {std::lock_guard<std::mutex> guard(mutex);stopping=true;preview.reset();deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);}
        changed.notify_all();writer.join();
    }
    ~Impl() {
        close();
#ifdef __linux__
        if(originalFlags>=0)(void)::fcntl(STDOUT_FILENO,F_SETFL,originalFlags);
#endif
    }
    bool write(const QByteArray& line) {
#ifdef __linux__
        qint64 offset=0;
        while(offset<line.size()) {
            {std::lock_guard<std::mutex> guard(mutex);if(stopping&&std::chrono::steady_clock::now()>=deadline)return false;}
            auto n=::write(STDOUT_FILENO,line.constData()+offset,std::size_t(line.size()-offset));
            if(n>0){offset+=n;continue;}
            if(n<0&&errno==EINTR)continue;
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){pollfd p{STDOUT_FILENO,POLLOUT,0};(void)::poll(&p,1,20);continue;}
            return false;
        }
        return true;
#else
        std::cout.write(line.constData(),line.size());std::cout.flush();return bool(std::cout);
#endif
    }
    void run() {
        for(;;) {
            QJsonObject item;
            {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return stopping||!controls.empty()||preview;});
                if(stopping&&controls.empty())return;
                if(!controls.empty()){item=std::move(controls.front());controls.pop_front();}
                else {item=std::move(*preview);preview.reset();}
                changed.notify_all();
            }
            if(!write(QJsonDocument(item).toJson(QJsonDocument::Compact)+'\n')) {
                std::lock_guard<std::mutex> guard(mutex);broken=true;
                if(failure.isEmpty())failure=stopping?"TRANSPORT_DRAIN_TIMEOUT":"TRANSPORT_CLOSED";
                undeliverableControls+=controls.size()+(item.value("event")=="preview"?0:1);changed.notify_all();return;
            }
        }
    }
};
StreamOutput::StreamOutput():impl_(std::make_unique<Impl>()){}
StreamOutput::~StreamOutput()=default;
bool StreamOutput::send(const QJsonObject& item) {
    const bool live=item.value("event")=="preview";
    const auto bytes=QJsonDocument(item).toJson(QJsonDocument::Compact).size();
    if(bytes>(live?98304:16384)){
        if(!live){std::lock_guard<std::mutex> guard(impl_->mutex);impl_->broken=true;impl_->failure="OUTPUT_PROTOCOL_ERROR";++impl_->undeliverableControls;impl_->changed.notify_all();}
        return false;
    }
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if(live){if(impl_->stopping||impl_->broken)return false;impl_->preview=item;}
    else {
        const bool ready=impl_->changed.wait_for(lock,std::chrono::milliseconds(100),[&]{return impl_->stopping||impl_->broken||impl_->controls.size()<256;});
        if(!ready){impl_->broken=true;impl_->failure="TRANSPORT_BACKPRESSURE";impl_->preview.reset();}
        if(impl_->stopping||impl_->broken){++impl_->undeliverableControls;impl_->changed.notify_all();return false;}
        impl_->controls.push_back(item);
        impl_->highControls=std::max(impl_->highControls,quint64(impl_->controls.size()));
    }
    impl_->changed.notify_one();return true;
}
void StreamOutput::clearPreview(){std::lock_guard<std::mutex> guard(impl_->mutex);impl_->preview.reset();}
bool StreamOutput::failed()const{std::lock_guard<std::mutex> guard(impl_->mutex);return impl_->broken;}
QString StreamOutput::failureCode()const{std::lock_guard<std::mutex> guard(impl_->mutex);return impl_->failure;}
QJsonObject StreamOutput::diagnostics()const{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return {{"failed",impl_->broken},{"error_code",impl_->failure},{"undeliverable_controls",QString::number(impl_->undeliverableControls)},
        {"queued_controls",int(impl_->controls.size())},{"high_controls",int(impl_->highControls)},{"control_limit",256},{"enqueue_wait_ms",100},{"shutdown_drain_ms",2000}};
}
void StreamOutput::shutdown(){impl_->close();}
