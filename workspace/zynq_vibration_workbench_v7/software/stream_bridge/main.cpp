// An independent UART console facade for vibration_stream_service.
// The child alone owns raw acquisition and SD publication. This process owns
// bounded, replaceable UI work, complete-file analysis and selected downloads.
#include "records.hpp"
#include <ax7z020_capture_stream.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QSet>
#include <QSocketNotifier>
#include <QStorageInfo>
#include <QTimer>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstdarg>
#include <deque>
#include <future>
#include <limits>
#include <optional>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <termios.h>

namespace {
using namespace streambridge;
const QString schema="vibration.serial.v1";
constexpr int maxWire=131072,maxDecoded=2097152,maxRequest=8192,maxServiceLine=262144;
constexpr int maxReliable=64,maxQueueBytes=1048576;
volatile std::sig_atomic_t signalStop=0;
void onSignal(int){signalStop=1;}
// Board console stdin/stdout/stderr share one physical UART. Any unframed
// diagnostic on its stderr would corrupt the serial packet stream.
void diagnostic(const char* format,...) {
    va_list arguments;va_start(arguments,format);
    if(::isatty(STDERR_FILENO))::vsyslog(LOG_NOTICE,format,arguments);
    else std::vfprintf(stderr,format,arguments);
    va_end(arguments);
}
void qtDiagnostic(QtMsgType type,const QMessageLogContext&,const QString& message) {
    const auto bytes=message.toLocal8Bit();diagnostic("qt[%d]: %s\n",int(type),bytes.constData());
}
class ConsoleFlags {
    std::array<int,2> flags_{};
public:
    ConsoleFlags(){
        for(int fd=0;fd<2;++fd){flags_[std::size_t(fd)]=::fcntl(fd,F_GETFL,0);require(flags_[std::size_t(fd)]>=0,"IO_ERROR","Cannot inspect console flags");}
        for(int fd=0;fd<2;++fd)if(::fcntl(fd,F_SETFL,flags_[std::size_t(fd)]|O_NONBLOCK)) {
            for(int j=0;j<fd;++j)(void)::fcntl(j,F_SETFL,flags_[std::size_t(j)]);
            fail("IO_ERROR","Cannot configure nonblocking console");
        }
    }
    ~ConsoleFlags(){for(int fd=0;fd<2;++fd)(void)::fcntl(fd,F_SETFL,flags_[std::size_t(fd)]);}
};
QByteArray encode(const QJsonObject& object,quint64 seq) {
    auto raw=QJsonDocument(object).toJson(QJsonDocument::Compact);
    require(raw.size()<=maxDecoded,"PACKET_TOO_LARGE","Decoded packet limit exceeded");
    auto line=QJsonDocument(QJsonObject{{"schema",schema},{"type","packet"},{"seq",double(seq)},
        {"codec","qcompress-base64"},{"payload",QString::fromLatin1(qCompress(raw,6).toBase64())},
        {"sha256",QString::fromLatin1(QCryptographicHash::hash(raw,QCryptographicHash::Sha256).toHex())}}).toJson(QJsonDocument::Compact)+'\n';
    require(line.size()<=maxWire,"PACKET_TOO_LARGE","Encoded packet limit exceeded");return line;
}
struct Pending {QString id,command;};
struct Preview {
    QString job,session,source;bool synthetic=false,hardware=false;
    quint64 revision=0,dropped=0,first=0,lastTick=0;int rate=0,clock=0,ppr=0,range=0;
    std::vector<vibration::Record> records;
};
QJsonArray points(const std::vector<vibration::Record>& records,int axis,int range,int rate,int clock,int ppr,int limit) {
    QJsonArray out;if(records.empty())return out;
    const int size=int(records.size()),buckets=std::max(1,limit/4),stride=std::max(1,(size+buckets-1)/buckets);
    const auto value=[&](int i)->double {
        const auto& r=records[std::size_t(i)];
        if(axis<3)return double(r.raw[std::size_t(axis)])*double(range)/1e6/32768.;
        return (r.flags&2u)&&!(r.flags&4u)&&r.speed_period_ticks&&ppr?double(clock)*60/double(r.speed_period_ticks)/ppr:std::numeric_limits<double>::quiet_NaN();
    };
    for(int begin=0;begin<size;begin+=stride) {
        const int end=std::min(size,begin+stride);int lo=begin,hi=begin,firstGap=-1,lastGap=-1;
        for(int i=begin;i<end;++i) {
            const auto v=value(i);if(!std::isfinite(v)){if(firstGap<0)firstGap=i;lastGap=i;}
            else {if(!std::isfinite(value(lo))||v<value(lo))lo=i;if(!std::isfinite(value(hi))||v>value(hi))hi=i;}
        }
        std::set<int> keep{begin,end-1};
        if(firstGap<0){keep.insert(lo);keep.insert(hi);}else{keep.insert(firstGap);keep.insert(lastGap);if(firstGap>begin)keep.insert(firstGap-1);if(lastGap+1<end)keep.insert(lastGap+1);}
        for(int i:keep)out.append(QJsonArray{double(records[std::size_t(i)].index)/rate,numeric(value(i))});
    }
    return out;
}
QJsonObject summarize(const Preview& view,int limit) {
    const auto count=view.records.size();require(count>0,"NO_PREVIEW","No validated raw preview");
    const double start=double(view.records.front().index)/view.rate,end=double(view.records.back().index)/view.rate;
    QJsonArray stats;double rpmSum=0;quint64 speedCount=0;
    for(int ch=0;ch<3;++ch) {
        double mean=0,m2=0,square=0,minimum=std::numeric_limits<double>::infinity(),maximum=-minimum;quint64 n=0;
        for(const auto& r:view.records) {
            const double v=double(r.raw[std::size_t(ch)])*view.range/1e6/32768.;++n;
            const double delta=v-mean;mean+=delta/double(n);m2+=delta*(v-mean);square+=v*v;minimum=std::min(minimum,v);maximum=std::max(maximum,v);
        }
        stats.append(QJsonObject{{"channel",ch+1},{"sample_count",double(n)},{"has_gaps",false},{"mean",mean},{"minimum",minimum},{"maximum",maximum},
            {"peak_to_peak",maximum-minimum},{"rms",std::sqrt(square/n)},{"ac_rms",std::sqrt(std::max(0.,m2/n))}});
    }
    for(const auto& r:view.records)if((r.flags&2u)&&!(r.flags&4u)&&r.speed_period_ticks&&view.ppr){rpmSum+=double(view.clock)*60/r.speed_period_ticks/view.ppr;++speedCount;}
    QJsonObject spectrum{{"available",false},{"start_s",start},{"end_s",end},{"max_hz",2000.},{"unit","V"},{"input_revision",double(view.revision)},{"channels",QJsonArray{}}};
    if(count>=128) {
        auto spectra=vibration::analyze_voltage_spectra(view.records,view.rate,view.clock,view.range,8192,2000);
        QJsonArray channels;
        for(const auto& s:spectra) {
            channels.append(packedSpectrum(s));
        }
        spectrum["available"]=true;spectrum["channels"]=channels;
    }
    return {{"job_id",view.job},{"generation",view.session},{"revision",double(view.revision)},{"global_first_index",number(view.records.front().index)},
        {"start_s",start},{"end_s",end},{"sample_rate_hz",view.rate},{"source",view.source},{"synthetic",view.synthetic},
        {"hardware_confirmed",view.hardware},{"ppr_confirmed",true},{"preview_dropped_records",double(view.dropped)},
        {"voltage",QJsonArray{}},{"voltage_representation","use_waveform_detail"},
        {"waveform_detail",waveformDetail(view.records,view.rate,view.range,0)},
        {"waveform_envelope",waveformEnvelope(view.records,view.rate,view.range,0,limit<=32?32:64)},
        {"speed",points(view.records,3,view.range,view.rate,view.clock,view.ppr,limit)},
        {"amplitude_stats",QJsonObject{{"scope","raw_window"},{"unit","V"},{"start_s",start},{"end_s",end},{"input_revision",double(view.revision)},
            {"derived_before_display_decimation",true},{"channels",stats}}},{"mean_rpm",speedCount?numeric(rpmSum/speedCount):QJsonValue(QJsonValue::Null)},
        {"valid_speed_fraction",double(speedCount)/double(count)},{"spectrum",spectrum},
        {"diagnosis",QJsonObject{{"status","preview_only"},{"classification","not_classified"},{"limitations",QJsonArray{
            "Lossy UI preview; raw SD capture is independent. Statistics use every original sample in this contiguous bounded window.",
            "Sensor identity, calibration and physical faults are unconfirmed; segment analysis does not certify the session."}}}}};
}

class Bridge final:public QObject {
    ConsoleFlags flags_;QProcess service_;QString root_;int baud_;
    QSocketNotifier input_{0,QSocketNotifier::Read,this},output_{1,QSocketNotifier::Write,this};QTimer timer_{this},previewTimer_{this};
    QByteArray ingress_,serviceIngress_,writing_;std::deque<QByteArray> reliable_;std::optional<QJsonObject> previewPacket_,analysisPacket_;
    QHash<QString,Pending> pending_;QSet<QString> requestIds_;std::deque<QString> requestOrder_;
    QJsonObject caps_,status_,requestedPlan_,acceptedPlan_;QString job_,latestSegment_,shutdownId_,fixtureDeviceLock_;bool fixture_=false,deleting_=false,downloadActive_=false,active_=false,startPending_=false,connected_=false,closing_=false,serviceExited_=false,shutdownConfirmed_=false,sessionStarted_=false,sessionDoneSeen_=false,discard_=false,ignoreOutput_=false;
    quint64 expectedTotal_=0,received_=0,durable_=0,segmentCount_=0;
    int queueBytes_=0,writeOffset_=0,exitCode_=0;quint64 seq_=0,serviceSeq_=0,previewDroppedRecords_=0,previewDroppedFrames_=0,previewRejects_=0,analysisSkipped_=0,revision_=0;
    qint64 lastInput_=QDateTime::currentMSecsSinceEpoch(),lastStatus_=0;QString statusState_;std::future<Task> task_;
    std::future<QJsonObject> previewTask_;Preview view_;quint64 summarizedRevision_=0;std::shared_ptr<Record> download_;std::optional<QJsonObject> queuedFileRequest_;
    std::array<qint64,2> downloadProgress_{{0,0}};
    QElapsedTimer pacing_;qint64 lastPacing_=0;double tokens_=0;bool sourceAnchored_=false;quint64 expectedPreview_=0,anchorTick_=0;
public:
    Bridge(const QString& executable,const QStringList& arguments,const QString& root,int baud,const QString& fixtureDeviceLock):root_(root),baud_(baud),fixtureDeviceLock_(fixtureDeviceLock) {
        fixture_=arguments.contains("--test-fixture");
        std::signal(SIGPIPE,SIG_IGN);std::signal(SIGINT,onSignal);std::signal(SIGTERM,onSignal);
        output_.setEnabled(false);pacing_.start();tokens_=std::max(256,baud_/10/10);
        connect(&input_,&QSocketNotifier::activated,this,[this](int){readInput();});connect(&output_,&QSocketNotifier::activated,this,[this](int){writeOutput();});
        connect(&service_,&QProcess::started,this,[this]{
            try{sendService(closing_?"shutdown":"capabilities",{},{});}
            catch(const std::exception& e){service_.closeWriteChannel();beginClose(QString::fromUtf8(e.what()),2,false);}
        });
        connect(&service_,&QProcess::readyReadStandardOutput,this,[this]{readService();});
        connect(&service_,&QProcess::readyReadStandardError,this,[this]{const auto data=service_.readAllStandardError();if(!data.isEmpty())diagnostic("%.*s",data.size(),data.constData());});
        connect(&service_,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this](int code,QProcess::ExitStatus state){
            readService();serviceExited_=true;connected_=false;
            if(code||state!=QProcess::NormalExit||!shutdownConfirmed_||(sessionStarted_&&!sessionDoneSeen_)){exitCode_=2;event("connection_lost",{{"reason","Stream service exited without a verified graceful shutdown and terminal session event"},{"raw_session_status",status_}});}
            if(!closing_)beginClose("Stream service exited",exitCode_,true);
            if(!shutdownId_.isEmpty()) {
                if(!exitCode_)response(shutdownId_,"shutdown",{{"accepted",true},{"durability_confirmed",!sessionStarted_||status_.value("session_complete").toBool()},{"session_done_seen",sessionDoneSeen_},{"job",status_}});
                else error(shutdownId_,"shutdown","SERVICE_LOST","Service ended without a verified graceful shutdown");
                shutdownId_.clear();
            }
            for(auto it=pending_.cbegin();it!=pending_.cend();++it)if(!it->id.isEmpty())error(it->id,it->command,"SERVICE_LOST","Stream service exited");
            pending_.clear();
        });
        connect(&service_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError errorCode){
            if(errorCode==QProcess::FailedToStart){serviceExited_=true;exitCode_=2;event("connection_lost",{{"reason",service_.errorString()}});beginClose("Unable to start stream service",2,true);}
        });
        timer_.setInterval(20);connect(&timer_,&QTimer::timeout,this,[this]{tick();});timer_.start();
        previewTimer_.setInterval(baud_==115200?2000:250);connect(&previewTimer_,&QTimer::timeout,this,[this]{launchPreview();});previewTimer_.start();
        service_.setProcessChannelMode(QProcess::SeparateChannels);service_.start(executable,arguments);
    }
    ~Bridge()override {
        // Normal exit is gated on child completion. Never turn tail fsync into
        // an arbitrary two-second kill. Exceptions before event-loop ownership
        // simply close stdin and wait for the same graceful service EOF path.
        if(service_.state()!=QProcess::NotRunning){service_.closeWriteChannel();service_.waitForFinished(-1);}
    }
private:
    std::shared_ptr<Fd> deletionLease()const {
        if(fixture_) {
            // Explicit host fixtures use a shared advisory lock only. The real
            // production path below never falls back to this simulated device.
            const auto path=fixtureDeviceLock_.isEmpty()?root_+"/.vibration-fixture-device.lock":fixtureDeviceLock_;
            auto lease=std::make_shared<Fd>(::open(QFile::encodeName(path).constData(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600));struct stat s{};
            require(lease->value>=0&&!::fstat(lease->value,&s)&&S_ISREG(s.st_mode)&&s.st_nlink==1&&s.st_uid==::geteuid(),"UNSAFE_STORAGE","Explicit fixture device lock must be an owned ordinary file");
            require(!::flock(lease->value,LOCK_EX|LOCK_NB),"BUSY","Another explicit fixture capture/delete holds the simulated capture device");return lease;
        }
        auto lease=std::make_shared<Fd>(::open("/dev/ax7z020-capture",O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW|O_EXCL));
        require(lease->value>=0,errno==EBUSY?"BUSY":"UNAVAILABLE","Cannot exclusively hold the capture device; acquisition or another owner may be active");
        struct stat s{};ax7z020_stream_caps caps{};
        require(!::fstat(lease->value,&s)&&S_ISCHR(s.st_mode)&&!::ioctl(lease->value,AX7Z020_STREAM_GET_CAPS,&caps)&&
            caps.abi_version==AX7Z020_STREAM_ABI&&caps.clock_hz==50000000&&caps.record_bytes==32&&(caps.features&7u)==7u,
            "UNAVAILABLE","Exclusive capture-device lease is not the verified stream ABI");return lease;
    }
    void validatePlan(const QJsonObject& plan)const {
        for(const auto& key:QStringList{"mode","sample_rate_hz","pulses_per_rev","output_directory","segment_seconds"})
            require(plan.value(key)==acceptedPlan_.value(key),"SERVICE_PROTOCOL","Service plan differs from accepted start plan: "+key);
        require(acceptedPlan_.value("mode")=="timed"?plan.value("duration_seconds")==acceptedPlan_.value("duration_seconds"):plan.value("duration_seconds").isNull(),"SERVICE_PROTOCOL","Service duration differs from accepted start plan");
        require(decimal(plan.value("total_records"))==expectedTotal_,"SERVICE_PROTOCOL","Service plan total differs from accepted start plan");
    }
    void validateStatus(const QJsonObject& o,bool final=false,bool ordered=true) {
        require(!job_.isEmpty()&&o.value("job_id")==job_&&o.value("session_id")==job_,"SERVICE_PROTOCOL","Status is not for the accepted session");
        validatePlan(o.value("plan").toObject());require(o.value("directory")==root_,"SERVICE_PROTOCOL","Status storage directory changed");
        const auto state=o.value("state").toString();require(QStringList{"preparing","running","stopping","completed","stopped","failed"}.contains(state),"SERVICE_PROTOCOL","Unknown capture state");
        require(o.value("source")==caps_.value("source")&&o.value("synthetic")==caps_.value("synthetic")&&o.value("hardware_confirmed").isBool()&&
            (fixture_?!o.value("hardware_confirmed").toBool():((state=="preparing"||state=="failed")||o.value("hardware_confirmed")==caps_.value("hardware_confirmed"))),"SERVICE_PROTOCOL","Status source differs from accepted backend");
        const auto received=decimal(o.value("records_received")),durable=decimal(o.value("durable_records")),segments=decimal(o.value("segment_count"));
        require(decimal(o.value("total_records"))==expectedTotal_&&decimal(o.value("records_captured"))==received&&decimal(o.value("records_persisted"))==durable&&
            durable<=received&&(!expectedTotal_||received<=expectedTotal_)&&(!ordered||(received>=received_&&durable>=durable_&&segments>=segmentCount_))&&segments<=durable,
            "SERVICE_PROTOCOL","Status totals/durable prefix are inconsistent or decreased");
        require(state!="completed"||(expectedTotal_&&received==expectedTotal_&&durable==received),"SERVICE_PROTOCOL","Completed status did not reach durable timed target");
        if(final) {
            require((state=="completed"||state=="stopped"||state=="failed")&&o.value("session_complete").isBool()&&o.value("target_complete").isBool(),"SERVICE_PROTOCOL","Invalid terminal session declaration");
            const bool complete=o.value("session_complete").toBool(),target=o.value("target_complete").toBool();
            require(complete==(state!="failed"&&received==durable)&&target==(state=="completed")&&
                (state=="failed"?!o.value("error_code").toString().isEmpty():o.value("error_code").toString().isEmpty()),"SERVICE_PROTOCOL","Terminal completion/error declarations conflict with raw counts");
        }
        received_=std::max(received_,received);durable_=std::max(durable_,durable);segmentCount_=std::max(segmentCount_,segments);
    }
    QJsonObject storage()const {
        bool available=false;QString reason,device,filesystem,mount;
        try {
            safeDirectory(root_);
            if(fixture_){available=true;reason="Explicit synthetic host storage fixture";}
            else {
                const QStorageInfo info(root_);device=QString::fromLocal8Bit(info.device());filesystem=QString::fromLocal8Bit(info.fileSystemType());mount=info.rootPath();
                available=info.isValid()&&info.isReady()&&!info.isReadOnly()&&mount!="/"&&
                    QRegularExpression("^/dev/mmcblk[0-9]+p[0-9]+$").match(device).hasMatch();
                reason=available?"Writable persistent SD partition mounted":"Writable mounted SD partition unavailable; no RAM fallback";
            }
        }catch(const std::exception& e){reason=QString::fromUtf8(e.what());}
        return {{"storage","sd"},{"persistent",!fixture_},{"storage_available",available},{"storage_reason",reason},
            {"storage_device",device},{"storage_filesystem",filesystem},{"storage_mount",mount},{"output_directory",root_},{"volatile",false},{"storage_fixture",fixture_}};
    }
    void enqueue(const QJsonObject& object,bool lossy=false,bool analysis=false) {
        if(ignoreOutput_)return;
        if(lossy){auto& slot=analysis?analysisPacket_:previewPacket_;if(slot){if(analysis)++analysisSkipped_;else ++previewDroppedFrames_;}slot=object;return;}
        try {
            auto bytes=encode(object,++seq_);
            if(reliable_.size()>=maxReliable||queueBytes_+bytes.size()>maxQueueBytes){beginClose("Reliable UART output queue exhausted; raw service is stopping gracefully",3,false);return;}
            queueBytes_+=bytes.size();reliable_.push_back(bytes);output_.setEnabled(true);
        }catch(const std::exception& e){beginClose(QString::fromUtf8(e.what()),3,false);}
    }
    void response(const QString& id,const QString& command,const QJsonObject& data){enqueue({{"id",id},{"command",command},{"ok",true},{"data",data}});}
    void error(const QString& id,const QString& command,const QString& code,const QString& message){enqueue({{"id",id},{"command",command},{"ok",false},{"error",QJsonObject{{"code",code},{"message",message}}}});}
    void event(const QString& name,const QJsonObject& data){enqueue({{"event",name},{"data",data}});}
    void launchFileRequest(const QJsonObject& o) {
        const auto id=o.value("id").toString(),command=o.value("command").toString(),root=root_;
        if(command=="list_records") {
            const auto cursor=o.value("cursor").toString();const int limit=o.value("limit").toInt(100);
            task_=std::async(std::launch::async,[root,cursor,limit,id]{Task t;t.kind="list";t.id=id;t.command="list_records";try{auto p=listPage(root,cursor,limit);t.data={{"records",p.records},{"cursor",cursor},{"next_cursor",p.next},{"has_more",!p.next.isEmpty()},{"limit",limit},{"rejected_records",p.rejected},{"volatile",false},{"storage","sd"},{"integrity","header_and_metadata_only"}};}catch(const Failure& e){t.code=e.code;t.error=QString::fromUtf8(e.what());}catch(const std::exception& e){t.code="FILE_ERROR";t.error=QString::fromUtf8(e.what());}return t;});
        }else {
            const auto recordId=o.value("record_id").toString();
            if(command=="analyze_record")task_=std::async(std::launch::async,[root,recordId,id]{return analyzeTask(root,recordId,id);});
            else task_=std::async(std::launch::async,[root,recordId,id]{Task t;t.kind="download";t.id=id;t.command="download_begin";try{t.record=inspectRecord(root,recordId,true);t.data={{"record_id",recordId},{"files",QJsonArray{t.record->bin.json(),t.record->meta.json()}},{"max_chunk",3072},{"volatile",false},{"storage","sd"}};}catch(const Failure& e){t.code=e.code;t.error=QString::fromUtf8(e.what());}catch(const std::exception& e){t.code="FILE_ERROR";t.error=QString::fromUtf8(e.what());}return t;});
        }
    }
    void scheduleFileRequest(const QJsonObject& o) {
        if(task_.valid()) {
            require(!queuedFileRequest_,"BUSY","The bounded user file-request queue already contains one request");
            queuedFileRequest_=o; // Preserve original request ID; no early success.
        }else launchFileRequest(o);
    }
    void sendService(const QString& command,const QJsonObject& fields,const Pending& request) {
        const QString id="bridge_"+number(++serviceSeq_);auto value=fields;value["schema"]="vibration.stream.v1";value["request_id"]=id;value["command"]=command;
        require(service_.state()==QProcess::Running && service_.bytesToWrite()<65536,"SERVICE_LOST","Stream service input unavailable");
        auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact)+'\n';require(service_.write(bytes)==bytes.size(),"SERVICE_LOST","Cannot queue stream service command");pending_[id]={request.id,request.command.isEmpty()?command:request.command};
    }
    void writeOutput() {
        const auto now=pacing_.elapsed();tokens_=std::min(double(baud_/10)/5,tokens_+double(now-lastPacing_)*baud_*.08/1000.);lastPacing_=now;
        int budget=int(tokens_);
        while(budget>0) {
            if(writing_.isEmpty()) {
                if(!reliable_.empty()){writing_=std::move(reliable_.front());queueBytes_-=writing_.size();reliable_.pop_front();}
                else if(previewPacket_){
                    writing_=encode(*previewPacket_,++seq_);previewPacket_.reset();
                    // Bound one nonpreemptible JSONL display packet. Control
                    // already queued always wins at the next packet boundary.
                    if(baud_==115200&&writing_.size()>18432){writing_.clear();++previewDroppedFrames_;continue;}
                }
                else if(analysisPacket_){writing_=encode(*analysisPacket_,++seq_);analysisPacket_.reset();}
                else {output_.setEnabled(false);return;}
                writeOffset_=0;
            }
            auto n=::write(1,writing_.constData()+writeOffset_,std::size_t(std::min(budget,writing_.size()-writeOffset_)));
            if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return;beginClose("UART output disconnected",2,false);return;}
            if(!n)return;
            writeOffset_+=int(n);budget-=int(n);tokens_-=double(n);
            if(writeOffset_==writing_.size()){writing_.clear();writeOffset_=0;}
        }
        // Reenable only on the timer to avoid a writable-fd busy loop.
        output_.setEnabled(false);
    }
    void readInput() {
        std::array<char,4096> chunk{};int budget=32768;
        while(budget>0&&!closing_) {
            auto n=::read(0,chunk.data(),std::size_t(std::min(budget,int(chunk.size()))));
            if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return;beginClose("UART input error",2,false);return;}
            if(!n){beginClose("UART EOF",0,false);return;}budget-=int(n);
            for(int i=0;i<n;++i) {
                if(chunk[std::size_t(i)]=='\n') {
                    if(discard_){discard_=false;error({}, {},"LINE_TOO_LONG","Request exceeds 8192 bytes");}
                    else if(!ingress_.trimmed().isEmpty())request(ingress_);
                    ingress_.clear();
                }else if(!discard_){if(ingress_.size()>=maxRequest){ingress_.clear();discard_=true;}else ingress_.append(chunk[std::size_t(i)]);}
                if(closing_)return;
            }
        }
    }
    void request(const QByteArray& raw) {
        QString id,command;
        try {
            const auto o=parse(raw);id=o.value("id").toString();command=o.value("command").toString();
            require(o.value("schema")==schema && QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(id).hasMatch() && o.value("command").isString(),"INVALID_REQUEST","Invalid serial schema/id/command");
            require(!requestIds_.contains(id),"DUPLICATE_REQUEST","Request ID was recently used");requestIds_.insert(id);requestOrder_.push_back(id);
            if(requestOrder_.size()>4096){requestIds_.remove(requestOrder_.front());requestOrder_.pop_front();}
            lastInput_=QDateTime::currentMSecsSinceEpoch();const QStringList base{"schema","id","command"};
            if(command=="hello") {
                keys(o,base);response(id,command,{{"version","2.0"},{"stream_protocol","vibration.stream.v1"},{"baud",baud_},{"preview_interval_ms",previewTimer_.interval()},
                    {"servicecaps",caps_},{"ready",connected_},{"recordroot",root_},{"volatile",false},{"storage","sd"},
                    {"max_wire_line",maxWire},{"max_decoded_bytes",maxDecoded},{"heartbeat_seconds",3},{"client_loss_seconds",15},{"recovery_complete",true}});
            }else if(command=="ping") {
                keys(o,base);response(id,command,{{"time_ms",double(lastInput_)},{"active",active_||startPending_},{"job_id",job_},{"analysis_busy",task_.valid()},
                    {"reliable_queue_bytes",queueBytes_},{"deleting",deleting_},{"download_active",downloadActive_},{"file_request_queued",bool(queuedFileRequest_)},{"preview_dropped_frames",number(previewDroppedFrames_)},{"preview_dropped_records",number(previewDroppedRecords_)},{"preview_rejected_frames",number(previewRejects_)},{"analysis_skipped_segments",number(analysisSkipped_)}});
            }else if(command=="start") {
                keys(o,base+QStringList{"plan"});require(connected_&&caps_.value("available").toBool(),"UNAVAILABLE","Stream service or SD storage unavailable");require(!active_&&!startPending_&&!deleting_,"BUSY","A raw capture or selected deletion is active");
                require(o.value("plan").isObject(),"INVALID_PLAN","Expected capture plan");auto plan=o.value("plan").toObject();
                keys(plan,{"mode","sample_rate_hz","duration_seconds","pulses_per_rev","output_directory","segment_seconds"});
                const auto mode=plan.value("mode").toString();require(mode=="timed"||mode=="continuous","INVALID_PLAN","mode must be timed or continuous");
                require(integer(plan.value("sample_rate_hz"),5000,50000)&&std::set<int>{5000,10000,25000,50000}.count(plan.value("sample_rate_hz").toInt())&&integer(plan.value("pulses_per_rev"),1,4096),"INVALID_PLAN","Unsupported sample rate or PPR");
                require(mode=="timed"?integer(plan.value("duration_seconds"),1,86400):!plan.contains("duration_seconds"),"INVALID_PLAN","Timed duration is 1..86400 seconds; continuous duration must be omitted");
                require(!plan.contains("output_directory")||plan.value("output_directory")==root_,"UNSAFE_STORAGE","Capture output is restricted to configured SD root");
                require(!plan.contains("segment_seconds")||integer(plan.value("segment_seconds"),1,40),"INVALID_PLAN","Segment duration must be 1..40 seconds");
                const auto disk=storage();require(disk.value("storage_available").toBool(),"UNSAFE_STORAGE",disk.value("storage_reason").toString());
                plan["output_directory"]=root_;if(!plan.contains("segment_seconds"))plan["segment_seconds"]=10;
                requestedPlan_=plan;view_=Preview{};sourceAnchored_=false;expectedPreview_=0;revision_=0;summarizedRevision_=0;previewDroppedRecords_=previewDroppedFrames_=previewRejects_=analysisSkipped_=0;latestSegment_.clear();download_.reset();downloadActive_=false;previewPacket_.reset();analysisPacket_.reset();
                sendService("start",{{"plan",plan}},{id,command});startPending_=true;
            }else if(command=="stop") {
                keys(o,base+QStringList{"job_id"});require(!o.contains("job_id")||o.value("job_id")==job_,"UNKNOWN_JOB","Unknown active job");
                if(!active_&&!startPending_)response(id,command,{{"accepted",false},{"job_id",job_},{"job",status_}});
                else sendService("stop",{}, {id,command});
            }else if(command=="list_records") {
                keys(o,base+QStringList{"cursor","limit"});
                require(!deleting_,"BUSY","A selected deletion is active");
                require(!o.contains("cursor")||o.value("cursor").isString(),"INVALID_CURSOR","cursor must be a string");require(!o.contains("limit")||integer(o.value("limit"),1,100),"INVALID_LIMIT","Page limit must be 1..100");
                require(!o.contains("cursor")||o.value("cursor").toString().isEmpty()||recordName(o.value("cursor").toString()),"INVALID_CURSOR","Invalid record cursor");scheduleFileRequest(o);
            }else if(command=="analyze_record"||command=="download_begin") {
                keys(o,base+QStringList{"record_id"});require(!deleting_,"BUSY","A selected deletion is active");const auto recordId=o.value("record_id").toString();require(recordName(recordId),"UNKNOWN_RECORD","Invalid opaque segment ID");scheduleFileRequest(o);
            }else if(command=="download_chunk") {
                keys(o,base+QStringList{"record_id","kind","offset","length"});require(!deleting_,"BUSY","A selected deletion is active");require(download_&&o.value("record_id")==download_->id,"DOWNLOAD_NOT_OPEN","Issue download_begin for the selected segment first");
                const auto kind=o.value("kind").toString();require(kind=="bin"||kind=="meta","INVALID_REQUEST","kind must be bin or meta");const auto& f=kind=="bin"?download_->bin:download_->meta;
                require(download_->bin.unchanged()&&download_->meta.unchanged(),"RECORD_CHANGED","Selected capture pair changed");
                require(integer(o.value("offset"),0,double(f.bytes()))&&integer(o.value("length"),1,3072),"INVALID_RANGE","Invalid download range");const auto offset=qint64(o.value("offset").toDouble());const int length=o.value("length").toInt();auto bytes=readAt(f,offset,length);
                response(id,command,{{"record_id",download_->id},{"kind",kind},{"offset",double(offset)},{"length",length},{"total",double(f.bytes())},{"data",QString::fromLatin1(bytes.toBase64())},{"crc32",double(vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),std::size_t(bytes.size())))}});
                auto& progress=downloadProgress_[kind=="bin"?0:1];if(offset==progress)progress+=length;
                if(downloadProgress_[0]==download_->bin.bytes()&&downloadProgress_[1]==download_->meta.bytes())downloadActive_=false;
            }else if(command=="download_end") {
                keys(o,base+QStringList{"record_id"});require(!o.contains("record_id")||(download_&&o.value("record_id")==download_->id),"UNKNOWN_RECORD","download_end record_id does not match the current download");
                const auto recordId=download_?download_->id:QString();const bool wasActive=downloadActive_;download_.reset();downloadActive_=false;downloadProgress_={{0,0}};response(id,command,{{"accepted",wasActive},{"record_id",recordId}});
            }else if(command=="delete_records") {
                keys(o,base+QStringList{"record_ids"});require(o.value("record_ids").isArray(),"INVALID_REQUEST","record_ids must be an explicit array");const auto array=o.value("record_ids").toArray();
                require(array.size()>=1&&array.size()<=100,"INVALID_REQUEST","Select 1..100 explicit segment IDs; whole-card deletion is unsupported");QStringList ids;QSet<QString> unique;
                for(const auto& value:array){require(value.isString()&&!value.toString().isEmpty()&&value.toString().size()<=180&&!unique.contains(value.toString()),"INVALID_REQUEST","Every selected record_id must be a bounded unique string");unique.insert(value.toString());ids.append(value.toString());}
                require(connected_&&!active_&&!startPending_&&(!sessionStarted_||sessionDoneSeen_)&&!deleting_&&!task_.valid()&&!queuedFileRequest_&&latestSegment_.isEmpty()&&!downloadActive_,"BUSY","Deletion requires a terminal sealed session and no analysis, queued file task, active download or deletion");
                const auto disk=storage();require(disk.value("storage_available").toBool(),"UNSAFE_STORAGE",disk.value("storage_reason").toString());auto lease=deletionLease();const auto root=root_;
                download_.reset();downloadProgress_={{0,0}};deleting_=true;
                try{task_=std::async(std::launch::async,[root,ids,id,lease]()mutable{Task t;t.kind="delete";t.id=id;t.command="delete_records";try{t.data=deleteRecords(root,ids);}catch(const Failure& e){t.code=e.code;t.error=QString::fromUtf8(e.what());}catch(const std::exception& e){t.code="DELETE_FAILED";t.error=QString::fromUtf8(e.what());}lease.reset();return t;});}
                catch(...){deleting_=false;throw;}
            }else if(command=="shutdown") {
                keys(o,base);shutdownId_=id;beginClose("Client requested shutdown",0,true);
            }else fail("UNKNOWN_COMMAND","Unsupported stream bridge command");
        }catch(const Failure& e){error(id,command,e.code,QString::fromUtf8(e.what()));}
        catch(const std::exception& e){error(id,command,"INTERNAL_ERROR",QString::fromUtf8(e.what()));}
    }
    void readService() {
        serviceIngress_+=service_.readAllStandardOutput();
        while(serviceIngress_.contains('\n')) {
            const int end=serviceIngress_.indexOf('\n');auto line=serviceIngress_.left(end);serviceIngress_.remove(0,end+1);
            try{require(line.size()<=maxServiceLine,"SERVICE_PROTOCOL","Service line exceeds bound");serviceMessage(parse(line));}
            catch(const std::exception& e){event("service_error",{{"message",QString::fromUtf8(e.what())}});beginClose("Invalid stream service message",2,true);}
        }
        if(serviceIngress_.size()>maxServiceLine){serviceIngress_.clear();beginClose("Unterminated service message exceeds bound",2,true);}
    }
    void serviceMessage(const QJsonObject& o) {
        require(o.value("schema")=="vibration.stream.v1" && o.value("emitter")=="stream_service","SERVICE_PROTOCOL","Unexpected service schema/emitter");
        if(o.value("type")=="response") {
            const auto id=o.value("request_id").toString();require(pending_.contains(id),"SERVICE_PROTOCOL","Unmatched service response");const auto p=pending_.take(id);
            if(o.value("ok")!=true) {
                if(p.command=="start")startPending_=false;
                if(!p.id.isEmpty())error(p.id,p.command,o.value("error_code").toString(),o.value("error_message").toString());else event("service_error",o);return;
            }
            if(p.command=="capabilities") {
                caps_=o.value("capabilities").toObject();const auto disk=storage();
                const QString expectedSource=fixture_?"test_fixture":service_.arguments().contains("--hardware-test")?"hardware_test":"adc_unconfirmed";
                require(caps_.value("available").isBool()&&caps_.value("source")==expectedSource&&caps_.value("synthetic")== (expectedSource!="adc_unconfirmed")&&
                    caps_.value("hardware_confirmed").isBool(),"SERVICE_PROTOCOL","Capabilities backend/source is inconsistent with explicit launch mode");
                for(auto it=disk.begin();it!=disk.end();++it)caps_[it.key()]=it.value();
                if(!disk.value("storage_available").toBool()){caps_["available"]=false;caps_["reason"]=disk.value("storage_reason");}
                connected_=true;
                event("service_ready",{{"servicecaps",caps_}});return;
            }
            if(p.command=="shutdown"){shutdownConfirmed_=true;return;}
            auto job=o.value("job").toObject();
            if(p.command=="start") {
                require(QRegularExpression("^[0-9a-f]{32}$").match(job.value("job_id").toString()).hasMatch(),"SERVICE_PROTOCOL","Start did not return a valid session UUID");
                acceptedPlan_=requestedPlan_;expectedTotal_=acceptedPlan_.value("mode")=="timed"?quint64(acceptedPlan_.value("sample_rate_hz").toInt())*quint64(acceptedPlan_.value("duration_seconds").toInt()):0;
                received_=durable_=segmentCount_=0;sessionStarted_=true;sessionDoneSeen_=false;job_=job.value("job_id").toString();validateStatus(job);
                startPending_=false;active_=true;status_=job;statusState_.clear();
            }else if(!job.isEmpty())validateStatus(job,false,false);
            QJsonObject data=job;data["job"]=job;data["job_id"]=job.value("job_id");if(p.command=="stop")data["accepted"]=true;
            if(!p.id.isEmpty())response(p.id,p.command,data);
            return;
        }
        require(o.value("type")=="event","SERVICE_PROTOCOL","Unexpected service message type");const auto eventName=o.value("event").toString();
        if(eventName=="preview") {if(!closing_)acceptPreview(o);return;}
        if(eventName=="segment_ready") {
            const auto segment=o.value("segment_id").toString();require(recordName(segment)&&o.value("path")==root_+'/'+segment+"/data.BIN"&&o.value("meta_path")==root_+'/'+segment+"/data.meta.json","SERVICE_PROTOCOL","Segment path outside configured SD root");
            require(o.value("job_id")==job_&&o.value("session_id")==job_&&segment.startsWith(job_+'-')&&o.value("source")==caps_.value("source")&&o.value("synthetic")==caps_.value("synthetic")&&validSource(o),"SERVICE_PROTOCOL","Segment source/session differs from accepted capture");
            if(!latestSegment_.isEmpty())++analysisSkipped_;
            latestSegment_=segment;auto data=o;data["record_id"]=segment;data["scope"]="completed_segment";event("segment_ready",data);return;
        }
        if(eventName=="job_status"||eventName=="session_done") {
            validateStatus(o,eventName=="session_done");if(eventName=="session_done")sessionDoneSeen_=true;
            const auto state=o.value("state").toString();status_=o;const bool terminal=state=="completed"||state=="stopped"||state=="failed";
            if(terminal){active_=false;previewPacket_.reset();view_.records.clear();}
            const auto now=QDateTime::currentMSecsSinceEpoch();
            if(eventName=="session_done"||terminal||state!=statusState_||now-lastStatus_>=1000){statusState_=state;lastStatus_=now;event(eventName,o);}
            return;
        }
        event(eventName,o);
    }
    void acceptPreview(const QJsonObject& frame) {
        try {
            if(!active_||frame.value("job_id")!=job_)return;
            const auto source=frame.value("source").toString();const bool synthetic=frame.value("synthetic").toBool();
            require(validSource(frame)&&frame.value("session_id")==job_&&frame.value("source")==caps_.value("source")&&frame.value("synthetic")==caps_.value("synthetic")&&frame.value("hardware_confirmed")==caps_.value("hardware_confirmed"),"INVALID_SOURCE","Preview source/session differs from explicit accepted backend");
            const int n=frame.value("record_count").toInt(),rate=frame.value("sample_rate_hz").toInt(),clock=frame.value("clock_hz").toInt(),ppr=frame.value("ppr").toInt(),range=frame.value("adc_range_uv").toInt();
            require(integer(frame.value("record_count"),1,2000)&&frame.value("sample_rate_hz")==acceptedPlan_.value("sample_rate_hz")&&frame.value("ppr")==acceptedPlan_.value("pulses_per_rev")&&
                frame.value("clock_hz")==caps_.value("clock_hz")&&frame.value("adc_range_uv")==caps_.value("adc_range_uv")&&frame.value("channel_map")==caps_.value("channel_map")&&
                n>0&&n<=2000&&std::set<int>{5000,10000,25000,50000}.count(rate)&&clock==50000000&&ppr>=1&&ppr<=4096&&range>0&&range<=10000000&&frame.value("record_bytes")==32&&frame.value("channel_map")==0x50301,"INVALID_PREVIEW","Preview parameters differ from accepted acquisition plan");
            const auto encoded=frame.value("raw_base64").toString().toLatin1();auto bytes=QByteArray::fromBase64(encoded);require(bytes.toBase64()==encoded&&bytes.size()==n*32,"INVALID_PREVIEW","Raw base64 length/canonical form mismatch");
            require(frame.value("crc32").toDouble(-1)==vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),std::size_t(bytes.size())),"INVALID_PREVIEW","Raw CRC mismatch");
            const auto first=decimal(frame.value("global_first_index")),firstTick=decimal(frame.value("first_tick")),lastTick=decimal(frame.value("last_tick"));
            require(first<=std::numeric_limits<quint64>::max()-quint64(n)&&first<=(std::numeric_limits<quint64>::max()/quint64(clock/rate))&&
                firstTick>=first*quint64(clock/rate)&&quint64(n-1)<=(std::numeric_limits<quint64>::max()-firstTick)/quint64(clock/rate),"INVALID_PREVIEW","Raw global index/tick overflow");
            const auto anchor=firstTick-first*quint64(clock/rate);
            require(!sourceAnchored_||(anchor==anchorTick_&&first>=expectedPreview_&&view_.rate==rate&&view_.clock==clock&&view_.ppr==ppr&&view_.range==range&&view_.source==source&&view_.session==frame.value("session_id").toString()&&view_.synthetic==synthetic&&view_.hardware==frame.value("hardware_confirmed").toBool()),"INVALID_PREVIEW","Raw preview identity/continuity mismatch");
            if(sourceAnchored_&&first>expectedPreview_){previewDroppedRecords_+=first-expectedPreview_;view_.records.clear();}
            else if(!sourceAnchored_)previewDroppedRecords_+=first;
            std::vector<vibration::Record> records;records.reserve(std::size_t(n));const auto* b=reinterpret_cast<const unsigned char*>(bytes.constData());
            for(int i=0;i<n;++i) {
                const auto* p=b+i*32;vibration::Record r;r.index=u64(p);r.tick=u64(p+8);for(int ch=0;ch<3;++ch)r.raw[std::size_t(ch)]=std::int16_t(u16(p+16+ch*2));r.flags=u16(p+22);r.speed_period_ticks=u32(p+24);r.speed_edge_count=u32(p+28);
                require(r.index==first+quint64(i)&&r.tick==firstTick+quint64(i)*quint64(clock/rate)&&(r.flags&1u)&&!(r.flags&~31u)&&(!(r.flags&2u)||(r.speed_period_ticks&&!(r.flags&4u))),"INVALID_PREVIEW","Raw index/tick/flags invalid");records.push_back(r);
            }
            require(records.back().tick==lastTick,"INVALID_PREVIEW","Raw last tick mismatch");
            sourceAnchored_=true;anchorTick_=anchor;expectedPreview_=first+quint64(n);view_.job=job_;view_.session=frame.value("session_id").toString();view_.source=source;view_.synthetic=synthetic;view_.hardware=frame.value("hardware_confirmed").toBool();
            view_.rate=rate;view_.clock=clock;view_.ppr=ppr;view_.range=range;view_.revision=++revision_;view_.dropped=previewDroppedRecords_;
            view_.records.insert(view_.records.end(),records.begin(),records.end());const auto keep=std::size_t(std::min(32768,rate/4));if(view_.records.size()>keep)view_.records.erase(view_.records.begin(),view_.records.end()-std::ptrdiff_t(keep));
        }catch(const std::exception& e){++previewRejects_;view_.records.clear();previewPacket_.reset();if(previewRejects_==1||previewRejects_%25==0)event("preview_rejected",{{"job_id",job_},{"reason",QString::fromUtf8(e.what())},{"quarantined",false},{"rejected_frames",number(previewRejects_)}});}
    }
    void launchPreview() {
        if(closing_||!active_||view_.records.size()<128||previewTask_.valid()||view_.revision==summarizedRevision_)return;
        auto view=view_;summarizedRevision_=view.revision;const int limit=baud_==115200?32:96;
        previewTask_=std::async(std::launch::async,[view,limit]{return summarize(view,limit);});
    }
    void pollTasks() {
        if(previewTask_.valid()&&previewTask_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            try{auto p=previewTask_.get();if(!closing_&&active_&&p.value("job_id")==job_) {
                p["preview_dropped_frames"]=number(previewDroppedFrames_);
                if(baud_==115200&&encode({{"event","preview"},{"data",p}},std::numeric_limits<quint64>::max()).size()>18432) {
                    p.remove("waveform_envelope");p["overview_omitted_due_to_budget"]=true;
                }
                if(baud_==115200&&encode({{"event","preview"},{"data",p}},std::numeric_limits<quint64>::max()).size()>18432) {
                    ++previewRejects_;++previewDroppedFrames_;
                    if(previewRejects_==1||previewRejects_%25==0)event("preview_rejected",{{"reason","Complete detail and FFT exceed the explicit UART packet budget"},{"code","PREVIEW_BUDGET"},{"rejected_frames",number(previewRejects_)}});
                }else enqueue({{"event","preview"},{"data",p}},true);
            }}
            catch(const std::exception& e){++previewRejects_;event("preview_rejected",{{"reason",QString::fromUtf8(e.what())},{"rejected_frames",number(previewRejects_)}});}
        }
        if(task_.valid()&&task_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            auto t=task_.get();if(t.kind=="delete")deleting_=false;if(!closing_) {
                if(!t.error.isEmpty()){if(t.id.isEmpty())event("analysis_failed",{{"scope","completed_segment"},{"code",t.code},{"message",t.error}});else error(t.id,t.command,t.code,t.error);}
                else if(t.kind=="analysis"&&t.id.isEmpty())enqueue({{"event","analysis"},{"data",t.data}},true,true);
                else{if(t.kind=="download"){download_=t.record;downloadProgress_={{0,0}};downloadActive_=true;}response(t.id,t.command,t.data);}
            }
        }
        if(!closing_&&!task_.valid()&&queuedFileRequest_) {
            auto request=*queuedFileRequest_;queuedFileRequest_.reset();
            try{launchFileRequest(request);}catch(const std::exception& e){error(request.value("id").toString(),request.value("command").toString(),"FILE_ERROR",QString::fromUtf8(e.what()));}
        }
        if(!closing_&&!task_.valid()&&!latestSegment_.isEmpty()) {
            auto id=latestSegment_;latestSegment_.clear();const auto root=root_;
            task_=std::async(std::launch::async,[root,id]{return analyzeTask(root,id,{});});
        }
    }
    void tick() {
        pollTasks();if(signalStop&&!closing_)beginClose("Console signal",0,true);
        if(!closing_&&QDateTime::currentMSecsSinceEpoch()-lastInput_>15000)beginClose("Client heartbeat expired",2,false);
        if(!ignoreOutput_&&(!reliable_.empty()||!writing_.isEmpty()||previewPacket_||analysisPacket_))output_.setEnabled(true);
        if(closing_&&serviceExited_&&(ignoreOutput_||(reliable_.empty()&&writing_.isEmpty()))) {
            // write(2) completion only reaches the tty driver queue. Keep the
            // bridge's current baud until the final complete packet leaves it.
            if(!ignoreOutput_&&::isatty(STDOUT_FILENO)) {
                int result;do{result=::tcdrain(STDOUT_FILENO);}while(result<0&&errno==EINTR);
                if(result<0){diagnostic("UART final tcdrain failed: %s\n",std::strerror(errno));exitCode_=2;}
            }
            QCoreApplication::exit(exitCode_);
        }
    }
    void beginClose(const QString& reason,int code,bool drain) {
        if(closing_) {
            if(!drain){ignoreOutput_=true;reliable_.clear();queueBytes_=0;writing_.clear();previewPacket_.reset();analysisPacket_.reset();output_.setEnabled(false);if(code)exitCode_=code;}
            return;
        }
        closing_=true;exitCode_=code;input_.setEnabled(false);previewTimer_.stop();previewPacket_.reset();analysisPacket_.reset();latestSegment_.clear();
        if(queuedFileRequest_){error(queuedFileRequest_->value("id").toString(),queuedFileRequest_->value("command").toString(),"SHUTTING_DOWN","Queued file request cancelled by graceful shutdown");queuedFileRequest_.reset();}
        diagnostic("stream_bridge: %s; waiting for service tail publication\n",reason.toUtf8().constData());
        if(!drain){ignoreOutput_=true;reliable_.clear();queueBytes_=0;writing_.clear();output_.setEnabled(false);}
        if(service_.state()==QProcess::Running) {
            try{sendService("shutdown",{},{});}catch(...){service_.closeWriteChannel();}
        }else if(service_.state()==QProcess::NotRunning)serviceExited_=true;
        // No kill timer. shutdown response is issued only after child finish().
    }
};
}
int main(int argc,char** argv) {
    ::openlog("vibration_stream_bridge",LOG_PID,LOG_USER);qInstallMessageHandler(qtDiagnostic);
    QCoreApplication app(argc,argv);QString service=QCoreApplication::applicationDirPath()+"/vibration_stream_service",root="/mnt/capture-data/vibration",fixtureDeviceLock;QStringList arguments;int baud=115200;bool fixture=false,hardwareTest=false;QSet<QString> seen;
    for(int i=1;i<argc;++i) {
        const auto option=QString::fromLocal8Bit(argv[i]);const auto key=option.section('=',0,0);
        if(seen.contains(key)){diagnostic("Duplicate option\n");return 2;}seen.insert(key);
        if(option=="--service"&&i+1<argc)service=QString::fromLocal8Bit(argv[++i]);
        else if(option=="--output-root"&&i+1<argc)root=QString::fromLocal8Bit(argv[++i]);
        else if(option=="--fixture-device-lock"&&i+1<argc)fixtureDeviceLock=QString::fromLocal8Bit(argv[++i]);
        else if(option=="--baud"&&i+1<argc){auto value=QString::fromLocal8Bit(argv[++i]);if(!QStringList{"115200","460800","921600"}.contains(value))return 2;baud=value.toInt();}
        else if(option=="--test-fixture"){fixture=true;arguments<<option;}
        else if(option=="--hardware-test"){hardwareTest=true;arguments<<option;}
        else if(option.startsWith("--fixture-chunk-ms=")||option.startsWith("--fixture-fault=")||option.startsWith("--reserve-bytes="))arguments<<option;
        else if(option=="--help"){diagnostic("vibration_stream_bridge [--service ABS_EXECUTABLE] [--output-root /mnt/capture-data/vibration] [--baud 115200|460800|921600] [--test-fixture | --hardware-test] [--fixture-chunk-ms=N] [--fixture-fault=NAME] [--reserve-bytes=N]\nJSONL UART control; raw stays on SD. Root must already exist. No implicit fixture/RAM fallback. EOF/shutdown stops and drains without a kill timeout.\n");return 0;}
        else{diagnostic("Unknown or incomplete option\n");return 2;}
    }
    if((fixture&&hardwareTest)||(!fixture&&(!fixtureDeviceLock.isEmpty()||arguments.join(' ').contains("--fixture-chunk-ms=")||arguments.join(' ').contains("--fixture-fault=")))){diagnostic("Conflicting modes or fixture options without --test-fixture\n");return 2;}
    if(!fixtureDeviceLock.isEmpty()&&(!QDir::isAbsolutePath(fixtureDeviceLock)||QDir::cleanPath(fixtureDeviceLock)!=fixtureDeviceLock)){diagnostic("Fixture lock path must be canonical absolute\n");return 2;}
    if(!QDir::isAbsolutePath(service)||!QFileInfo(service).isFile()||!QFileInfo(service).isExecutable()){diagnostic("--service must be an absolute executable\n");return 2;}
    if(!QDir::isAbsolutePath(root)||QDir::cleanPath(root)!=root){diagnostic("--output-root must be canonical absolute path\n");return 2;}
    try{Bridge bridge(service,arguments,root,baud,fixtureDeviceLock);return app.exec();}catch(const std::exception& e){diagnostic("stream_bridge: %s\n",e.what());return 2;}
}

