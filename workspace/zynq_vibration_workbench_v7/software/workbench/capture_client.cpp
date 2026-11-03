#include "capture_client.hpp"
#include "capture_transport.hpp"
#include "stream_preview_contract.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>

namespace vibration::workbench {
namespace {
enum class Kind {Response, Rejected, Status, Preview, PreviewError, Failed};
struct Delivery {
    Kind kind; QJsonObject object; QString a,b,c; bool flag=false;
    quint64 records=0,bytes=0,session=0,enqueuedNs=0;
};
quint64 nowNs(){return quint64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
}
struct CaptureMailbox {
    QMutex mutex;
    std::deque<Delivery> queue;
    QJsonObject process,profile;
    quint64 session=1,records=0,bytes=0,controls=0,recordLimit=100000;
    quint64 highRecords=0,highBytes=0,highFrames=0,highControls=0;
    quint64 previews=0,wakes=0,drains=0,delivered=0,dropped=0,stopDiscarded=0,controlDiscarded=0;
    quint64 latestReplacements=0;
    quint64 drainNs=0,maxDrainNs=0,maxFramesPerDrain=0,stale=0,overflows=0;
    quint64 maxDeliveryAgeNs=0,reportedDropped=0;
    quint64 previewAgeCount=0,previewAgeTotalNs=0,previewAgeMaxNs=0;
    std::array<quint64,256> previewAges{};
    QJsonArray losses;
    bool lossDetailsTruncated=false;
    bool accepting=false,wakePending=false,profileDirty=false,fatal=false,continuousStream=false;

    void loss(const Delivery& x,const char* reason){
        const bool stream=x.object.value("schema")=="vibration.stream.v1";
        const auto first=stream?stream_contract::decimal(x.object.value("global_first_index")):quint64(x.object.value("first_record").toDouble());
        QJsonObject range{{"job_id",x.object.value("job_id")},{"session",QString::number(x.session)},{"first_record",stream?QJsonValue(QString::number(first)):QJsonValue(double(first))},{"record_count",double(x.records)},{"reason",QString::fromLatin1(reason)}};
        if(!losses.isEmpty()){
            auto last=losses.last().toObject();
            const auto previous=last.value("first_record").isString()?stream_contract::decimal(last.value("first_record")):quint64(last.value("first_record").toDouble());
            if(last.value("job_id")==range.value("job_id")&&last.value("reason")==range.value("reason")&&last.value("session")==range.value("session")&&previous+quint64(last.value("record_count").toDouble())==first){last["record_count"]=last.value("record_count").toDouble()+double(x.records);losses[losses.size()-1]=last;return;}
        }
        if(losses.size()>=64){losses.removeAt(0);lossDetailsTruncated=true;}
        losses.append(range);
    }
    void discard(bool stop){
        for(const auto& x:queue){if(x.kind==Kind::Preview){loss(x,stop?"stop_or_reconnect":"control_overflow");if(stop)stopDiscarded+=x.records;else dropped+=x.records;}else ++controlDiscarded;}
        queue.clear();records=bytes=controls=previews=0;
    }
    // Caller holds mutex. Only one no-payload Qt wake can be outstanding.
    bool wake(){if(wakePending)return false;wakePending=true;++wakes;return true;}
    bool append(Delivery item,bool& notify){
        QMutexLocker lock(&mutex);
        if(item.session!=session||!accepting||fatal){++stale;return true;}
        item.enqueuedNs=nowNs();
        if(item.kind==Kind::Response&&item.a=="start"){
            const auto rate=item.object.value("plan").toObject().value("sample_rate_hz").toInt();
            continuousStream=item.object.value("schema")=="vibration.stream.v1";
            const auto boundedRate=quint64(std::max(5000,std::min(50000,rate)));
            recordLimit=boundedRate*2;
        }
        if(item.kind==Kind::Preview){
            // Keep a bounded continuous raw preview window, even when a GUI
            // paint takes several producer periods. Rendering consumes the
            // final adopted window once, not one historical UI per chunk.
            while(previews>=64||records+item.records>recordLimit||bytes+item.bytes>16777216){
                const auto it=std::find_if(queue.begin(),queue.end(),[](const Delivery& d){return d.kind==Kind::Preview;});
                if(it==queue.end())break;
                loss(*it,"preview_overflow");records-=it->records;bytes-=it->bytes;dropped+=it->records;--previews;++overflows;queue.erase(it);
            }
            records+=item.records;bytes+=item.bytes;++previews;
        }else{
            if(controls>=128){
                discard(false);fatal=true;accepting=false;
                queue.push_back({Kind::Failed,{},QStringLiteral("Capture reliable-control mailbox limit exceeded")});controls=1;
                notify=wake();return false;
            }
            ++controls;
        }
        queue.push_back(std::move(item));highRecords=std::max(highRecords,records);highBytes=std::max(highBytes,bytes);
        highFrames=std::max(highFrames,previews);highControls=std::max(highControls,controls);notify=wake();return true;
    }
};

CaptureClient::CaptureClient(QObject* parent):QObject(parent),mailbox_(std::make_shared<CaptureMailbox>()){
    workerEnabled_=qgetenv("AX7_CAPTURE_WORKER")=="1";
    profileEnabled_=qgetenv("AX7_CAPTURE_PROFILE")=="1";
    program_=QCoreApplication::applicationDirPath()+"/vibration_capture_service";
    transport_=new CaptureTransport(workerEnabled_?nullptr:this);
    if(!workerEnabled_){
        transport_->initialize();
        connect(transport_,&CaptureTransport::responseReceived,this,&CaptureClient::responseReceived);
        connect(transport_,&CaptureTransport::requestRejected,this,&CaptureClient::requestRejected);
        connect(transport_,&CaptureTransport::jobUpdated,this,&CaptureClient::jobUpdated);
        connect(transport_,&CaptureTransport::liveFrame,this,&CaptureClient::liveFrame);
        connect(transport_,&CaptureTransport::previewRejected,this,&CaptureClient::previewRejected);
        connect(transport_,&CaptureTransport::connectionFailed,this,&CaptureClient::connectionFailed);
        connect(transport_,&CaptureTransport::transportProfileUpdated,this,[this](const QJsonObject&){emit transportProfileUpdated(transportProfile());});
        return;
    }
    const auto box=mailbox_;const auto engine=transport_;
    // These direct callbacks execute only on the engine's thread and touch
    // shared mailbox values, never the facade or widgets.
    const auto enqueue=[box,engine](Delivery item){
        item.session=engine->sessionToken();bool wake=false;
        const bool ok=box->append(std::move(item),wake);
        if(wake)emit engine->mailboxReady(engine->sessionToken());
        if(!ok)engine->abortTransport("Capture reliable-control mailbox limit exceeded");
    };
    connect(engine,&CaptureTransport::responseReceived,engine,[enqueue](const QString& c,const QJsonObject& d){enqueue({Kind::Response,d,c});});
    connect(engine,&CaptureTransport::requestRejected,engine,[enqueue](const QString& c,const QString& e,const QString& m){enqueue({Kind::Rejected,{},c,e,m});});
    connect(engine,&CaptureTransport::jobUpdated,engine,[enqueue](const QJsonObject& d){enqueue({Kind::Status,d});});
    connect(engine,&CaptureTransport::liveFrame,engine,[enqueue](const QJsonObject& d){
        Delivery item{Kind::Preview,d};item.records=quint64(d.value("record_count").toInt());
        // Conservative payload accounting: UTF16 base64 plus8KiB envelope/Qt
        // entry allowance. This is a payload bound, not a total-RSS promise.
        item.bytes=8192+2*4*((item.records*32+2)/3);enqueue(std::move(item));
    });
    connect(engine,&CaptureTransport::previewRejected,engine,[enqueue](const QString& r,bool q){Delivery d{Kind::PreviewError,{},r};d.flag=q;enqueue(std::move(d));});
    connect(engine,&CaptureTransport::connectionFailed,engine,[enqueue](const QString& d){enqueue({Kind::Failed,{},d});});
    connect(engine,&CaptureTransport::stateSnapshotChanged,engine,[box,engine](const QJsonObject& state){QMutexLocker lock(&box->mutex);if(engine->sessionToken()==box->session)box->process=state;});
    connect(engine,&CaptureTransport::transportProfileUpdated,engine,[box,engine](const QJsonObject& profile){
        bool wake=false;{QMutexLocker lock(&box->mutex);if(engine->sessionToken()!=box->session)return;box->profile=profile;box->profileDirty=true;if(box->accepting)wake=box->wake();}
        if(wake)emit engine->mailboxReady(engine->sessionToken());
    });
    connect(engine,&CaptureTransport::mailboxReady,this,&CaptureClient::drain,Qt::QueuedConnection);
    engine->setCooperative(true);
    worker_=new QThread;worker_->setObjectName("vibration-capture-transport");engine->moveToThread(worker_);
    connect(worker_,&QThread::started,engine,&CaptureTransport::initialize);
    connect(worker_,&QThread::finished,engine,&QObject::deleteLater);
    // Facade destruction always joins before deleting the QThread QObject.
    worker_->start();
}
CaptureClient::~CaptureClient(){
    if(!workerEnabled_){stop();return;}
    stop();const auto engine=transport_;auto thread=worker_;thread->requestInterruption();
    QMetaObject::invokeMethod(engine,[engine,thread]{engine->stop();thread->quit();},Qt::QueuedConnection);
    // No GUI callback is needed by actor shutdown. Graceful child shutdown has
    // bounded waits; absolute join is preferred to leaving an acquisition actor
    // behind. Extreme OS stalls can extend destruction beyond this warning.
    if(!thread->wait(3000)){qWarning("Capture transport join exceeded graceful deadline; waiting for owned actor exit");thread->wait();}
    delete thread;
    transport_=nullptr;worker_=nullptr;
}
void CaptureClient::setCommand(const QString& program,const QStringList& arguments){
    if(!workerEnabled_){transport_->setCommand(program,arguments);return;}
    program_=program;arguments_=arguments;
    if(!stopWorker()){emit connectionFailed("Capture transport is still stopping; command change has not been applied to a child");return;}
    if(profileEnabled_)emit transportProfileUpdated(transportProfile());
}
void CaptureClient::setStreaming(bool value){
    if(streaming_==value)return;
    if(!workerEnabled_){transport_->setStreaming(value);streaming_=value;return;}
    if(!stopWorker()){emit connectionFailed("Previous capture backend is still stopping");return;}
    const auto previous=QCoreApplication::applicationDirPath()+(streaming_?"/vibration_stream_service":"/vibration_capture_service");
    if(program_==previous)program_=QCoreApplication::applicationDirPath()+(value?"/vibration_stream_service":"/vibration_capture_service");
    streaming_=value;const auto engine=transport_;
    QMetaObject::invokeMethod(engine,[engine,value]{engine->setStreaming(value);},Qt::QueuedConnection);
}
bool CaptureClient::stopWorker(){
    {QMutexLocker lock(&mailbox_->mutex);mailbox_->accepting=false;mailbox_->discard(true);mailbox_->wakePending=false;}
    submitted_.clear();
    const auto done=std::make_shared<QSemaphore>();const auto engine=transport_;
    QMetaObject::invokeMethod(engine,[engine,done]{engine->stop();done->release();},Qt::QueuedConnection);
    if(streaming_){done->acquire();return true;}
    return done->tryAcquire(1,3000);
}
void CaptureClient::stop(){
    if(!workerEnabled_){transport_->stop();return;}
    const bool stopped=stopWorker();
    if(profileEnabled_)emit transportProfileUpdated(transportProfile());
    if(!stopped)qWarning("Capture transport stop deadline exceeded; child stop remains unconfirmed");
}
void CaptureClient::connectService(){
    if(!workerEnabled_){transport_->connectService();return;}
    if(!stopWorker()){emit connectionFailed("Capture transport is still stopping; driver stop is unconfirmed");return;}
    ++session_;lastDeliveredEnd_=0;
    {QMutexLocker lock(&mailbox_->mutex);mailbox_->session=session_;mailbox_->accepting=true;mailbox_->fatal=false;mailbox_->profileDirty=false;mailbox_->profile={};mailbox_->process={};}
    const auto engine=transport_;const auto program=program_;const auto arguments=arguments_;const auto token=session_;
    QMetaObject::invokeMethod(engine,[engine,program,arguments,token]{engine->setSessionToken(token);engine->setCommand(program,arguments);engine->connectService();},Qt::QueuedConnection);
}
bool CaptureClient::isRunning()const{if(!workerEnabled_)return transport_->isRunning();QMutexLocker lock(&mailbox_->mutex);return mailbox_->process.value("running").toBool()&&mailbox_->accepting;}
bool CaptureClient::hasPending(const QString& command)const{
    if(!workerEnabled_)return transport_->hasPending(command);
    if(submitted_.value(command)>0)return true;
    QMutexLocker lock(&mailbox_->mutex);for(const auto& x:mailbox_->process.value("pending").toArray())if(x==command)return true;return false;
}
void CaptureClient::request(const QString& command,QJsonObject fields){
    if(!workerEnabled_){transport_->request(command,std::move(fields));return;}
    if(!isRunning()){emit connectionFailed("Capture service is not running");return;}
    int count=0;for(const auto n:submitted_)count+=n;
    if(count>=128||QJsonDocument(fields).toJson(QJsonDocument::Compact).size()>8192){QPointer<CaptureClient> guard(this);stop();if(guard)emit connectionFailed("Capture command queue/line limit exceeded");return;}
    ++submitted_[command];const auto engine=transport_;const auto token=session_;
    // Separate Qt command event: never queued behind the GUI raw mailbox.
    QMetaObject::invokeMethod(engine,[engine,token,command,fields]{if(engine->sessionToken()==token)engine->request(command,fields);},Qt::QueuedConnection);
}
QJsonObject CaptureClient::serviceProcessInfo()const{
    QJsonObject info;
    if(!workerEnabled_)info=transport_->processInfo();else{QMutexLocker lock(&mailbox_->mutex);info=mailbox_->process;}
    info["worker_enabled"]=workerEnabled_;info["session"]=QString::number(session_);return info;
}
QJsonObject CaptureClient::transportProfile()const{
    if(!workerEnabled_){auto value=transport_->transportProfile();value["worker"]=QJsonObject{{"enabled",false}};return value;}
    QMutexLocker lock(&mailbox_->mutex);auto value=mailbox_->profile;
    const auto& b=*mailbox_;
    QJsonObject worker{{"enabled",true},{"profile_enabled",profileEnabled_},{"session",QString::number(session_)},
        {"queued_records",double(b.records)},{"queued_frames",double(b.previews)},{"accounted_payload_bytes",double(b.bytes)},
        {"high_records",double(b.highRecords)},{"high_frames",double(b.highFrames)},{"high_accounted_payload_bytes",double(b.highBytes)},
        {"high_controls",double(b.highControls)},{"record_limit",double(b.recordLimit)},{"frame_limit",64},{"payload_byte_limit",16777216},
        {"preview_policy",b.continuousStream?"bounded_contiguous_fifo":"bounded_fifo"},{"latest_frame_replacements",double(b.latestReplacements)},
        {"control_limit",128},{"mailbox_dropped_records",double(b.dropped)},{"stop_discarded_records",double(b.stopDiscarded)},
        {"discarded_controls",double(b.controlDiscarded)},{"overflow_entries",double(b.overflows)},{"stale_deliveries",double(b.stale)},
        {"wake_count",double(b.wakes)},{"wake_pending",b.wakePending},{"drain_count",double(b.drains)},{"delivered_frames",double(b.delivered)},
        {"main_delivery_total_ns",double(b.drainNs)},{"main_delivery_max_ns",double(b.maxDrainNs)},{"max_frames_per_drain",double(b.maxFramesPerDrain)},
        {"batch_frame_limit",b.continuousStream?64:2},{"batch_cooperative_budget_ns",b.continuousStream?0:6000000},{"budget_is_preemptive",false},
        {"stream_batch_scope","fixed delivery count observed at drain entry; new arrivals wait for next batch"}};
    worker["max_delivery_age_ns"]=double(b.maxDeliveryAgeNs);worker["loss_ranges"]=b.losses;worker["loss_range_limit"]=64;worker["loss_details_truncated"]=b.lossDetailsTruncated;
    const auto ageSamples=std::size_t(std::min<quint64>(b.previewAgeCount,256));
    auto sorted=b.previewAges;std::sort(sorted.begin(),sorted.begin()+ageSamples);
    worker["preview_delivery_age"]=QJsonObject{{"clock","steady monotonic; validated enqueue to GUI emit, excludes raw hardware startup"},{"count",double(b.previewAgeCount)},{"mean_ns",b.previewAgeCount?double(b.previewAgeTotalNs)/b.previewAgeCount:0},{"max_ns",double(b.previewAgeMaxNs)},{"recent_samples",int(ageSamples)},{"recent_p50_ns",ageSamples?double(sorted[(ageSamples-1)/2]):0},{"recent_p95_ns",ageSamples?double(sorted[std::size_t((ageSamples-1)*.95)]):0}};
    quint64 oldest=0;for(const auto& item:b.queue)if(item.kind==Kind::Preview){oldest=nowNs()-item.enqueuedNs;break;}worker["oldest_queued_preview_age_ns"]=double(oldest);
    value["worker"]=worker;value["scope"]="Worker engine cumulative wall timings; signal_delivery measures mailbox enqueue. main_delivery includes synchronous Model; do not add nested metrics.";return value;
}
void CaptureClient::drain(quint64 token){
    if(token!=session_)return;
    QPointer<CaptureClient> guard(this);
    QElapsedTimer timer;timer.start();int frames=0;bool profileDirty=false,batchStarted=false;quint64 lost=0;
    int remaining=0;bool continuous=false;
    {QMutexLocker lock(&mailbox_->mutex);if(!mailbox_->accepting&&!mailbox_->fatal){mailbox_->wakePending=false;return;}profileDirty=mailbox_->profileDirty;mailbox_->profileDirty=false;remaining=int(mailbox_->queue.size());continuous=mailbox_->continuousStream;}
    while(true){
        if(continuous&&remaining--<=0)break;
        Delivery item;
        {QMutexLocker lock(&mailbox_->mutex);
            if(token!=mailbox_->session||mailbox_->queue.empty())break;
            item=std::move(mailbox_->queue.front());mailbox_->queue.pop_front();
            if(item.enqueuedNs)mailbox_->maxDeliveryAgeNs=std::max(mailbox_->maxDeliveryAgeNs,nowNs()-item.enqueuedNs);
            if(item.kind==Kind::Preview){const auto age=nowNs()-item.enqueuedNs;mailbox_->previewAges[mailbox_->previewAgeCount%256]=age;++mailbox_->previewAgeCount;mailbox_->previewAgeTotalNs+=age;mailbox_->previewAgeMaxNs=std::max(mailbox_->previewAgeMaxNs,age);mailbox_->records-=item.records;mailbox_->bytes-=item.bytes;--mailbox_->previews;lost=mailbox_->dropped;}
            else --mailbox_->controls;
        }
        switch(item.kind){
        case Kind::Response:
            if(submitted_.value(item.a)>0)--submitted_[item.a];
            if(item.a=="start")lastDeliveredEnd_=0;
            emit responseReceived(item.a,item.object);break;
        case Kind::Rejected:if(submitted_.value(item.a)>0)--submitted_[item.a];emit requestRejected(item.a,item.b,item.c);break;
        case Kind::Status:emit jobUpdated(item.object);break;
        case Kind::Preview:{
            if(continuous&&!batchStarted){batchStarted=true;emit previewBatchStarted();if(!guard)return;if(token!=session_)break;}
            const bool stream=item.object.value("schema")=="vibration.stream.v1";
            const auto first=stream?stream_contract::decimal(item.object.value("global_first_index")):quint64(item.object.value("first_record").toDouble());
            const auto gap=first>=lastDeliveredEnd_?first-lastDeliveredEnd_:0;
            item.object["observed_gap_records"]=stream?QJsonValue(QString::number(gap)):QJsonValue(double(gap));
            if(lost)item.object["mailbox_dropped_records"]=stream?QJsonValue(QString::number(lost)):QJsonValue(double(lost));
            bool notifyGap=false;{QMutexLocker lock(&mailbox_->mutex);if(lost>mailbox_->reportedDropped){mailbox_->reportedDropped=lost;notifyGap=true;}}
            if(notifyGap&&!stream){emit previewRejected(QStringLiteral("预览处理队列已满；缺失记录以空隙显示，完整文件不受影响"),false);if(!guard)return;if(token!=session_)return;QMutexLocker lock(&mailbox_->mutex);if(!mailbox_->accepting)return;}
            lastDeliveredEnd_=first+item.records;emit liveFrame(item.object);if(!guard)return;++frames;
            {QMutexLocker lock(&mailbox_->mutex);++mailbox_->delivered;}break;}
        case Kind::PreviewError:emit previewRejected(item.a,item.flag);break;
        case Kind::Failed:submitted_.clear();emit connectionFailed(item.a);break;
        }
        if(!guard)return;
        // A synchronous public consumer may stop/reconnect this facade.
        if(token!=session_)break;
        {QMutexLocker lock(&mailbox_->mutex);if(!mailbox_->accepting&&!mailbox_->fatal)break;}
        if(!continuous&&(frames>=2||timer.nsecsElapsed()>=6000000))break;
    }
    if(batchStarted){emit previewBatchFinished();if(!guard)return;}
    bool more=false;
    {QMutexLocker lock(&mailbox_->mutex);if(token!=mailbox_->session)return;
        ++mailbox_->drains;mailbox_->drainNs+=quint64(timer.nsecsElapsed());mailbox_->maxDrainNs=std::max(mailbox_->maxDrainNs,quint64(timer.nsecsElapsed()));mailbox_->maxFramesPerDrain=std::max(mailbox_->maxFramesPerDrain,quint64(frames));
        more=!mailbox_->queue.empty()||mailbox_->profileDirty;
        if(!more)mailbox_->wakePending=false;
    }
    if(profileDirty&&profileEnabled_)emit transportProfileUpdated(transportProfile());
    if(!guard)return;
    if(more)QTimer::singleShot(1,this,[this,token]{drain(token);});
}
}
