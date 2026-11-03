#include "capture_transport.hpp"
#include "stream_preview_contract.hpp"
#include <QFileInfo>
#include <QThread>
#include "../acquisition/canonical_base64.hpp"
#include <vibration/core.hpp>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QPointer>
#include <QTimer>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace vibration::workbench {
namespace diag=vibration::capture_diag;
namespace {
const QString schema=QStringLiteral("vibration.capture.v2");
bool number(const QJsonValue& v,double lo,double hi){const auto d=v.toDouble(-1);return v.isDouble()&&std::isfinite(d)&&std::floor(d)==d&&d>=lo&&d<=hi;}
quint16 read16(const unsigned char* p){return quint16(p[0])|(quint16(p[1])<<8);}
quint32 read32(const unsigned char* p){return quint32(p[0])|(quint32(p[1])<<8)|(quint32(p[2])<<16)|(quint32(p[3])<<24);}
quint64 read64(const unsigned char* p){return read32(p)|(quint64(read32(p+4))<<32);}
quint32 crc32(const QByteArray& raw){
    return vibration::crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()),std::size_t(raw.size()));
}
// QJsonDocument alone accepts duplicate object keys. Check the already-parsed,
// bounded JSON text as well, so ambiguous frames cannot drive page state.
class UniqueKeys {
public:
    explicit UniqueKeys(const QByteArray& bytes):bytes_(bytes){}
    void check(){value();}
private:
    const QByteArray& bytes_;int position_=0;
    void whitespace(){while(position_<bytes_.size()&&QByteArray(" \t\r\n").contains(bytes_[position_]))++position_;}
    QString string(bool decode=true){
        const int start=position_++;
        while(position_<bytes_.size()){
            // QJsonDocument has already validated JSON syntax. Jump over long
            // base64 strings without a byte-at-a-time Qt indexing loop, while
            // preserving escaped quote parity for keys and arbitrary strings.
            const int end=bytes_.indexOf('"',position_);
            if(end<0){position_=bytes_.size();break;}
            int slashes=0;for(int i=end-1;i>=position_&&bytes_[i]=='\\';--i)++slashes;
            position_=end+1;if(!(slashes&1))break;
        }
        return decode?QJsonDocument::fromJson("["+bytes_.mid(start,position_-start)+"]").array().at(0).toString():QString();
    }
    void value(){
        whitespace();if(position_>=bytes_.size())return;const char c=bytes_[position_];
        if(c=='{'){
            ++position_;whitespace();std::set<QString> keys;
            while(position_<bytes_.size()&&bytes_[position_]!='}'){
                whitespace();if(!keys.insert(string()).second)throw std::runtime_error("Duplicate JSON key");
                whitespace();++position_;value();whitespace();
                if(position_<bytes_.size()&&bytes_[position_]==','){++position_;continue;}break;
            }
            ++position_;
        }else if(c=='['){
            ++position_;whitespace();
            while(position_<bytes_.size()&&bytes_[position_]!=']'){
                value();whitespace();if(position_<bytes_.size()&&bytes_[position_]==','){++position_;continue;}break;
            }
            ++position_;
        }else if(c=='"')string(false);
        else while(position_<bytes_.size()&&!QByteArray(",]} \r\n\t").contains(bytes_[position_]))++position_;
    }
};
}
CaptureTransport::CaptureTransport(QObject* parent):QObject(parent){}
void CaptureTransport::initialize(){
    if(process_)return;
    process_=new QProcess(this);deadlineTimer_=new QTimer(this);
    if(program_.isEmpty())program_=QCoreApplication::applicationDirPath()+"/vibration_capture_service";
    process_->setProcessChannelMode(QProcess::SeparateChannels);deadlineTimer_->setInterval(250);
    connect(process_,&QProcess::stateChanged,this,[this](QProcess::ProcessState){emit stateSnapshotChanged(processInfo());});
    connect(process_,&QProcess::started,this,[this]{startedEver_=true;deadlineTimer_->start();request("capabilities");emit stateSnapshotChanged(processInfo());});
    connect(process_,&QProcess::readyReadStandardOutput,this,&CaptureTransport::receive);
    connect(process_,&QProcess::readyReadStandardError,this,&CaptureTransport::receiveDiagnostics);
    connect(process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError){if(!stopping_)fail(process_->errorString());});
    connect(process_,QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),this,[this](int,QProcess::ExitStatus){exitSeen_=true;emit stateSnapshotChanged(processInfo());receiveDiagnostics();if(!stopping_)fail("Capture service exited");});
    connect(deadlineTimer_,&QTimer::timeout,this,[this]{
        if(profile_.enabled())emit transportProfileUpdated(transportProfile());
        const auto now=QDateTime::currentMSecsSinceEpoch();
        for(auto it=pending_.cbegin();it!=pending_.cend();++it)if(now>it->deadline){fail("Capture service response deadline exceeded");return;}
    });
}
CaptureTransport::~CaptureTransport(){stop();}
QJsonObject CaptureTransport::transportProfile()const{auto value=profile_.snapshot();value["scope"]="client lifetime cumulative";value["decoder_mode"]="single_pass_utf16";if(!serviceProfile_.isEmpty())value["service_profile"]=serviceProfile_;return value;}
void CaptureTransport::receiveDiagnostics(){
    if(!process_->isOpen())return;
    const auto bytes=process_->readAllStandardError();if(!profile_.enabled())return;
    for(char c:bytes){
        if(c=='\n'){
            if(!diagnosticDiscard_&&diagnosticBuffer_.startsWith("AX7_CAPTURE_PROFILE ")){
                const auto document=QJsonDocument::fromJson(diagnosticBuffer_.mid(20));
                if(document.isObject()){
                    const auto object=document.object();const auto component=object.value("component").toString();
                    if((component=="service"||component=="output")&&object.value("profile").isObject()){
                        serviceProfile_[component]=object;emit transportProfileUpdated(transportProfile());
                    }
                }
            }
            diagnosticBuffer_.clear();diagnosticDiscard_=false;
        }else if(!diagnosticDiscard_){
            if(diagnosticBuffer_.size()>=16384){diagnosticBuffer_.clear();diagnosticDiscard_=true;}
            else diagnosticBuffer_.append(c);
        }
    }
}
void CaptureTransport::setCommand(const QString& program,const QStringList& arguments){QPointer<CaptureTransport> guard(this);stop();if(guard){program_=program;arguments_=arguments;}}
void CaptureTransport::setStreaming(bool value){
    if(streaming_==value)return;
    stop();
    const auto oldDefault=QCoreApplication::applicationDirPath()+(streaming_?"/vibration_stream_service":"/vibration_capture_service");
    if(program_.isEmpty()||program_==oldDefault)program_=QCoreApplication::applicationDirPath()+(value?"/vibration_stream_service":"/vibration_capture_service");
    streaming_=value;
}
bool CaptureTransport::isRunning()const{return process_&&process_->state()==QProcess::Running;}
QJsonObject CaptureTransport::processInfo()const{
    QJsonArray pending;for(const auto& p:pending_)pending.append(p.command);
    return {{"running",isRunning()},{"pid",double(isRunning()?process_->processId():0)},{"program",program_},{"arguments",QJsonArray::fromStringList(arguments_)},{"canonical_program",QFileInfo(program_).canonicalFilePath()},{"pending",pending},{"exit_observed",exitSeen_},{"started_observed",startedEver_},{"process_not_running",process_&&process_->state()==QProcess::NotRunning},{"owner_thread_matches",QThread::currentThread()==thread()},{"streaming",streaming_},{"session_done_observed",streamSessionDone_}};
}
bool CaptureTransport::hasPending(const QString& command)const{for(auto it=pending_.cbegin();it!=pending_.cend();++it)if(it->command==command)return true;return false;}
void CaptureTransport::connectService(){initialize();QPointer<CaptureTransport> guard(this);stop();if(!guard)return;serviceProfile_={};diagnosticBuffer_.clear();diagnosticDiscard_=false;startedEver_=exitSeen_=false;process_->start(program_,arguments_);emit stateSnapshotChanged(processInfo());}
void CaptureTransport::stop(){
    if(!process_)return;
    if(streaming_){
        stopping_=true;deadlineTimer_->stop();receivePending_=false;
        if(process_->state()!=QProcess::NotRunning){
            // EOF asks the stream service to stop at the next chunk boundary,
            // drain the ring and fsync its tail. Never kill that child on a
            // fixed GUI deadline while it still owns an unpublished segment.
            process_->closeWriteChannel();
            while(process_->state()!=QProcess::NotRunning){(void)process_->waitForFinished(100);receive();receiveDiagnostics();}
        }
        receive();pending_.clear();buffer_.clear();previewTerminal_=true;liveJob_.clear();
        if(!streamSessionDone_)qWarning("Stream process exited without a verified session_done; storage completion remains unconfirmed");
        stopping_=false;emit stateSnapshotChanged(processInfo());return;
    }
    stopping_=true;deadlineTimer_->stop();pending_.clear();buffer_.clear();receivePending_=false;previewTerminal_=true;liveJob_.clear();
    if(process_->state()!=QProcess::NotRunning){
        // EOF invokes service cancellation and worker join. Escalation is only
        // for a child that cannot finish, never a shell or hardware reset.
        process_->closeWriteChannel();
        if(!process_->waitForFinished(1500)){
            process_->terminate();if(!process_->waitForFinished(500)){process_->kill();process_->waitForFinished(500);}
        }
    }
    receiveDiagnostics();if(profile_.enabled())emit transportProfileUpdated(transportProfile());
    stopping_=false;emit stateSnapshotChanged(processInfo());
}
void CaptureTransport::fail(const QString& detail){QPointer<CaptureTransport> guard(this);stop();if(guard)emit connectionFailed(detail.left(2048));}
void CaptureTransport::request(const QString& command,QJsonObject fields){
    if(!isRunning()){emit connectionFailed("Capture service is not running");return;}
    if(command=="start")requestedPlan_=fields.value("plan").toObject();
    const QString id=QString("workbench_%1").arg(++sequence_);
    fields["schema"]=streaming_?QStringLiteral("vibration.stream.v1"):schema;fields["request_id"]=id;fields["command"]=command;
    const auto bytes=QJsonDocument(fields).toJson(QJsonDocument::Compact);
    if(bytes.size()>8192){fail("Request exceeds protocol line limit");return;}
    if(pending_.size()>=128||process_->bytesToWrite()>1048576){fail("Capture command queue limit exceeded");return;}
    pending_.insert(id,{command,QDateTime::currentMSecsSinceEpoch()+8000});process_->write(bytes+'\n');emit stateSnapshotChanged(processInfo());
}
void CaptureTransport::receive(){
    if(!process_||(!process_->isOpen()&&buffer_.isEmpty()))return;
    if(stopping_&&!streaming_)return;diag::Span receiveTime(profile_,diag::Receive);const auto bytes=process_->readAllStandardOutput();profile_.count(diag::InputBytes,quint64(bytes.size()));profile_.maximum(diag::MaxInputChunkBytes,quint64(bytes.size()));profile_.count(bytes.size()>65536?diag::InputChunksOver64k:diag::InputChunksLe64k);buffer_+=bytes;
    if(cooperative_&&buffer_.size()>1048576){fail("Capture ingress staging limit exceeded");return;}
    QElapsedTimer batch;batch.start();int lines=0;
    while(true){
        if(cooperative_&&!(streaming_&&stopping_)&&QThread::currentThread()->isInterruptionRequested())return;
        if(cooperative_&&!(streaming_&&stopping_)&&(lines>=4||batch.nsecsElapsed()>=8000000)&&buffer_.contains('\n')){
            if(!receivePending_){receivePending_=true;const auto token=session_;QTimer::singleShot(1,this,[this,token]{if(token==session_){receivePending_=false;receive();}});}
            return;
        }
        const int end=buffer_.indexOf('\n');
        if(end<0){if(buffer_.size()>196608)fail("Service message exceeds preview line limit");return;}
        if(end>196608){fail("Service message exceeds preview line limit");return;}
        const auto line=buffer_.left(end);buffer_.remove(0,end+1);
        ++lines;
        profile_.count(diag::InputMessages);
        QJsonParseError error;QJsonDocument document;{diag::Span measured(profile_,diag::JsonParse);document=QJsonDocument::fromJson(line,&error);}
        if(error.error!=QJsonParseError::NoError||!document.isObject()){fail(line.size()>8192?"Control message exceeds original 8192-byte line limit":"Malformed service JSON");return;}
        if(line.size()>8192&&document.object().value("event")!="preview"){fail("Control message exceeds original 8192-byte line limit");return;}
        try{diag::Span measured(profile_,diag::UniqueKeys);UniqueKeys(line).check();}catch(const std::exception& e){
            const auto object=document.object();
            if(object.value("event")=="preview"){
                if(!previewTerminal_&&!previewQuarantined_&&object.value("job_id").toString()==liveJob_){profile_.count(diag::RejectedFrames);emit previewRejected(QString::fromUtf8(e.what()),false);}
                continue;
            }
            fail(QString::fromUtf8(e.what()));return;
        }
        frame(document.object());if(!isRunning()&&!(streaming_&&stopping_))return;
    }
}
void CaptureTransport::frame(const QJsonObject& object){
    if(streaming_){streamFrame(object);return;}
    if(object.value("event")=="preview"){previewFrame(object);return;}
    if(object.value("schema")!=schema){fail("Capture service version mismatch");return;}
    if(object.value("type")=="event"){
        if(object.value("event")!="job_status"||!object.value("job_id").isString()){fail("Invalid capture event");return;}
        const auto state=object.value("state").toString();if(object.value("job_id").toString()==liveJob_&&(state=="completed"||state=="failed"||state=="cancelled"))previewTerminal_=true;
        if(profile_.enabled())emit transportProfileUpdated(transportProfile());emit jobUpdated(object);return;
    }
    if(object.value("type")!="response"||!object.value("request_id").isString()||!object.value("ok").isBool()){
        fail("Invalid capture response envelope");return;
    }
    const auto id=object.value("request_id").toString();
    if(!pending_.contains(id)){fail("Unexpected capture response ID");return;}
    const auto command=pending_.take(id).command;emit stateSnapshotChanged(processInfo());
    if(object.value("command")!=command){fail("Capture response command mismatch");return;}
    if(!object.value("ok").toBool()){
        if(!object.value("error").isObject()){fail("Capture error response missing detail");return;}
        const auto error=object.value("error").toObject();
        if(!error.value("code").isString()||!error.value("message").isString()){fail("Invalid capture error detail");return;}
        emit requestRejected(command,error.value("code").toString(),error.value("message").toString());return;
    }
    if(!object.value("data").isObject()){fail("Capture response missing data");return;}
    const auto data=object.value("data").toObject();
    if(command=="status"&&data.value("job_id").toString()==liveJob_&&QStringList{"completed","failed","cancelled"}.contains(data.value("state").toString()))previewTerminal_=true;
    if(command=="capabilities")backendCapabilities_=data;
    if(command=="start"){
        const auto plan=data.value("plan").toObject();
        for(const auto& key:QStringList{"sample_rate_hz","duration_seconds","pulses_per_rev","output_directory"})if(plan.value(key)!=requestedPlan_.value(key)||!requestedPlan_.contains(key)){fail("Start response does not match requested plan");return;}
        if(!number(plan.value("requested_records"),1,2097024)||plan.value("requested_records").toInt()!=requestedPlan_.value("sample_rate_hz").toInt()*requestedPlan_.value("duration_seconds").toInt()){fail("Start response record count differs from plan");return;}
        livePlan_=plan;liveJob_=data.value("job_id").toString();liveGeneration_.clear();liveIdentity_.clear();lastFrameSequence_=0;lastRecordEnd_=0;haveTickAnchor_=false;previewQuarantined_=false;previewTerminal_=false;
    }
    emit responseReceived(command,object.value("data").toObject());
}
void CaptureTransport::streamFrame(const QJsonObject& object){
    if(object.value("event")=="preview"){streamPreview(object);return;}
    const auto reject=[this](const QString& detail){if(stopping_){qWarning("Invalid final stream message: %s",qPrintable(detail));return;}fail(detail);};
    if(object.value("schema")!="vibration.stream.v1"||object.value("emitter")!="stream_service"){reject("Stream service envelope mismatch");return;}
    if(object.value("type")=="event"){
        const auto event=object.value("event").toString();
        if(!QStringList{"job_status","session_done","segment_ready"}.contains(event)||object.value("job_id").toString()!=liveJob_||object.value("session_id").toString()!=liveJob_){reject("Unexpected stream event/session");return;}
        const bool fixture=backendCapabilities_.value("backend")=="test_fixture";
        if(!object.value("synthetic").isBool()||!object.value("hardware_confirmed").isBool()||object.value("synthetic")!=backendCapabilities_.value("synthetic")||object.value("source")!=backendCapabilities_.value("source")||(fixture&&object.value("hardware_confirmed").toBool())){reject("Stream event source mismatch");return;}
        const auto state=object.value("state").toString();
        if(event!="segment_ready"){
            try{
                const auto total=stream_contract::decimal(object.value("total_records")),received=stream_contract::decimal(object.value("records_received")),durable=stream_contract::decimal(object.value("durable_records"));
                if(!QStringList{"preparing","running","stopping","completed","stopped","failed"}.contains(state)||total!=stream_contract::decimal(livePlan_.value("total_records"))||durable>received||(total&&received>total)||
                   (state=="completed"&&(!total||received!=total||durable!=received)))throw std::runtime_error("Stream status plan/count/state mismatch");
                if(event=="session_done"&&((object.value("session_complete").toBool()&&(state=="failed"||durable!=received))||
                   (object.value("target_complete").toBool()&&(!total||received!=total||state!="completed"))))throw std::runtime_error("Inconsistent stream completion claim");
            }catch(const std::exception& e){reject(QString::fromUtf8(e.what()));return;}
        }
        if(event=="session_done"){
            if(!QStringList{"completed","stopped","failed"}.contains(state)||!object.value("session_complete").isBool()||!object.value("target_complete").isBool()){reject("Invalid final stream status");return;}
            streamSessionDone_=true;previewTerminal_=true;
        }else if(event=="job_status"&&QStringList{"completed","stopped","failed"}.contains(state))previewTerminal_=true;
        emit jobUpdated(object);return;
    }
    if(object.value("type")!="response"||!object.value("request_id").isString()||!object.value("ok").isBool()){reject("Invalid stream response");return;}
    const auto id=object.value("request_id").toString();if(!pending_.contains(id)){reject("Unexpected stream response ID");return;}
    const auto command=pending_.take(id).command;emit stateSnapshotChanged(processInfo());
    if(!object.value("ok").toBool()){
        if(!object.value("error_code").isString()||!object.value("error_message").isString()){reject("Invalid stream error detail");return;}
        emit requestRejected(command,object.value("error_code").toString(),object.value("error_message").toString());return;
    }
    QJsonObject data;
    if(command=="capabilities"){
        if(!object.value("capabilities").isObject()){reject("Stream capabilities missing");return;}
        data=object.value("capabilities").toObject();backendCapabilities_=data;
    }else if(command=="start"||command=="status"||command=="stop"){
        if(!object.value("job").isObject()&&!object.value("job").isNull()){reject("Stream job response missing");return;}
        data=object.value("job").toObject();
        if(command=="start"){
            const auto plan=data.value("plan").toObject();
            for(const auto& key:QStringList{"mode","sample_rate_hz","pulses_per_rev","output_directory"})if(!requestedPlan_.contains(key)||plan.value(key)!=requestedPlan_.value(key)){reject("Stream start differs from requested plan");return;}
            const bool timed=requestedPlan_.value("mode")=="timed";
            if((timed&&plan.value("duration_seconds")!=requestedPlan_.value("duration_seconds"))||(!timed&&requestedPlan_.contains("duration_seconds"))||plan.value("segment_seconds").toInt()!=requestedPlan_.value("segment_seconds").toInt(10)){reject("Stream start duration/segment differs from plan");return;}
            const auto expected=quint64(requestedPlan_.value("sample_rate_hz").toInt())*quint64(timed?requestedPlan_.value("duration_seconds").toInt():0);
            try{if(stream_contract::decimal(data.value("total_records"))!=expected||stream_contract::decimal(plan.value("total_records"))!=expected)throw std::runtime_error("Stream total differs from requested plan");}
            catch(const std::exception& e){reject(QString::fromUtf8(e.what()));return;}
            const auto job=data.value("job_id").toString();if(!QRegularExpression("^[0-9a-f]{32}$").match(job).hasMatch()||data.value("session_id").toString()!=job){reject("Invalid stream session identity");return;}
            livePlan_=plan;liveJob_=job;liveIdentity_.clear();lastFrameSequence_=lastRecordEnd_=0;haveTickAnchor_=false;previewQuarantined_=false;previewTerminal_=false;streamSessionDone_=false;
        }
    }else data=object;
    emit responseReceived(command,data);
}
void CaptureTransport::streamPreview(QJsonObject object){
    if(previewTerminal_||liveJob_.isEmpty()||object.value("job_id").toString()!=liveJob_||previewQuarantined_)return;
    try{
        auto p=stream_contract::decode(object);
        const bool fixture=backendCapabilities_.value("backend")=="test_fixture";
        if(p.session!=liveJob_||p.rate!=quint32(livePlan_.value("sample_rate_hz").toInt())||p.ppr!=quint32(livePlan_.value("pulses_per_rev").toInt())||p.synthetic!=backendCapabilities_.value("synthetic").toBool()||p.source!=backendCapabilities_.value("source").toString()||(fixture&&p.hardware)){
            previewQuarantined_=true;throw std::runtime_error("Stream preview differs from confirmed plan/source");}
        const auto total=stream_contract::decimal(livePlan_.value("total_records"));if(total&&p.first+p.count>total)throw std::runtime_error("Stream preview exceeds timed target");
        QJsonObject identity{{"session_id",p.session},{"source",p.source},{"synthetic",p.synthetic},{"hardware_confirmed",p.hardware},{"rate",int(p.rate)},{"ppr",int(p.ppr)}};
        auto signature=QJsonDocument(identity).toJson(QJsonDocument::Compact);
        if((!liveIdentity_.isEmpty()&&signature!=liveIdentity_)||(haveTickAnchor_&&p.anchor!=tickAnchor_)){
            previewQuarantined_=true;throw std::runtime_error("Stream preview identity/tick anchor changed");}
        if(p.sequence<=lastFrameSequence_||p.first<lastRecordEnd_)throw std::runtime_error("Duplicate or overlapping stream preview");
        object["total_records"]=QString::number(total);object["observed_gap_records"]=QString::number(p.first-lastRecordEnd_);
        liveIdentity_=signature;lastFrameSequence_=p.sequence;lastRecordEnd_=p.first+p.count;tickAnchor_=p.anchor;haveTickAnchor_=true;
        profile_.count(diag::AcceptedFrames);emit liveFrame(object);
    }catch(const std::exception& e){profile_.count(diag::RejectedFrames);emit previewRejected(QString::fromUtf8(e.what()),previewQuarantined_);}
}
void CaptureTransport::previewFrame(QJsonObject object){
    if(previewTerminal_||liveJob_.isEmpty()||object.value("job_id").toString()!=liveJob_||previewQuarantined_)return;
    diag::Span verifyTime(profile_,diag::PreviewVerify);
    const auto reject=[this](const QString& reason,bool quarantine=false){profile_.count(diag::RejectedFrames);if(quarantine)previewQuarantined_=true;emit previewRejected(reason,quarantine);};
    if(object.value("schema")!=schema||object.value("type")!="event"){reject("Invalid preview envelope");return;}
    const QStringList required{"job_id","generation","frame_seq","first_record","record_count","committed_records","requested_records","sample_rate_hz","clock_hz","ppr","adc_range_uv","channel_map","record_bytes","source","hardware_confirmed","synthetic","source_flags","validity_flags","raw_base64","crc32","dropped_preview_records"};
    for(const auto& key:required)if(!object.contains(key)){reject("Missing preview field: "+key);return;}
    for(auto it=object.begin();it!=object.end();++it)if(!required.contains(it.key())&&it.key()!="schema"&&it.key()!="type"&&it.key()!="event"){reject("Unknown preview field: "+it.key());return;}
    const auto generation=object.value("generation").toString();bool generationOk=false;const auto generationNumber=generation.toULongLong(&generationOk);
    if(!object.value("generation").isString()||!generationOk||!generationNumber||!QRegularExpression("^[1-9][0-9]{0,19}$").match(generation).hasMatch()){reject("Invalid preview generation");return;}
    if(!number(object.value("frame_seq"),1,9007199254740991.0)||!number(object.value("first_record"),0,2097024)||!number(object.value("record_count"),1,4096)||!number(object.value("committed_records"),1,2097024)||!number(object.value("requested_records"),1,2097024)||!number(object.value("ppr"),1,4096)||!number(object.value("crc32"),0,4294967295.0)||!number(object.value("dropped_preview_records"),0,2097024)||!number(object.value("source_flags"),0,1)||!number(object.value("validity_flags"),0,31)){
        reject("Invalid preview numeric field");return;
    }
    const int rate=object.value("sample_rate_hz").toInt();
    if(!number(object.value("sample_rate_hz"),1,50000)||!std::set<int>{50000,25000,10000,5000}.count(rate)||!number(object.value("clock_hz"),50000000,50000000)||!number(object.value("adc_range_uv"),5000000,5000000)||!number(object.value("channel_map"),0x50301,0x50301)||!number(object.value("record_bytes"),32,32)){reject("Invalid preview acquisition metadata");return;}
    if(object.value("sample_rate_hz")!=livePlan_.value("sample_rate_hz")||object.value("ppr")!=livePlan_.value("pulses_per_rev")||object.value("requested_records")!=livePlan_.value("requested_records")){reject("Preview differs from user-confirmed start plan",true);return;}
    const auto first=quint32(object.value("first_record").toDouble()),count=quint32(object.value("record_count").toDouble()),committed=quint32(object.value("committed_records").toDouble()),requested=quint32(object.value("requested_records").toDouble());
    if(first+count>committed||committed>requested||(object.value("validity_flags").toInt()&27)!=27){reject("Preview is outside validated committed prefix");return;}
    if(!object.value("source").isString()||!object.value("hardware_confirmed").isBool()||!object.value("synthetic").isBool()){reject("Missing preview source types");return;}
    const auto source=object.value("source").toString();const bool hardware=object.value("hardware_confirmed").toBool(),synthetic=object.value("synthetic").toBool();const int flags=object.value("source_flags").toInt();
    if(!((source=="adc_unconfirmed"&&hardware&&!synthetic&&flags==0)||(source=="hardware_test"&&hardware&&synthetic&&flags==1)||(source=="test_fixture"&&!hardware&&synthetic&&flags==1))){reject("Unverified preview source");return;}
    const bool fixture=backendCapabilities_.value("backend")=="test_fixture",selfTest=backendCapabilities_.value("requested_source")=="hardware_self_test";
    if((fixture&&source!="test_fixture")||(!fixture&&(!hardware||source!=(selfTest?"hardware_test":"adc_unconfirmed")))){reject("Preview source differs from confirmed backend",true);return;}
    QJsonObject identity;
    for(const auto& key:QStringList{"generation","source","hardware_confirmed","synthetic","source_flags","sample_rate_hz","clock_hz","ppr","adc_range_uv","channel_map","record_bytes","requested_records"})identity[key]=object.value(key);
    const auto signature=QJsonDocument(identity).toJson(QJsonDocument::Compact);
    if(!liveIdentity_.isEmpty()&&signature!=liveIdentity_){reject("Preview identity/source changed within job",true);return;}
    const auto sequence=quint64(object.value("frame_seq").toDouble());
    if(sequence<=lastFrameSequence_||first<lastRecordEnd_){reject("Duplicate or overlapping preview sequence");return;}
    if(!object.value("raw_base64").isString()){reject("Missing preview raw bytes");return;}
    QString encoded;{diag::Span measured(profile_,diag::Base64Materialize);encoded=object.value("raw_base64").toString();}
    if(encoded.size()>174764){reject("Preview raw encoding exceeds record limit");return;}
    QByteArray raw;bool payloadOk;
    {diag::Span measured(profile_,diag::Base64Decode);payloadOk=vibration::transport::decodeCanonicalBase64(encoded,int(count*32),raw);}
    if(payloadOk){diag::Span measured(profile_,diag::FrameCrc);payloadOk=crc32(raw)==quint32(object.value("crc32").toDouble());}
    if(!payloadOk){reject("Preview base64/CRC validation failed");return;}
    const auto* data=reinterpret_cast<const unsigned char*>(raw.constData());const quint64 step=50000000u/quint32(rate),offset=quint64(first)*step,tick=read64(data+8);
    if(tick<offset){reject("Preview timestamp underflow");return;}const auto anchor=tick-offset;
    if((haveTickAnchor_&&anchor!=tickAnchor_)||anchor>std::numeric_limits<quint64>::max()-quint64(first+count-1)*step){reject("Preview timestamp anchor changed");return;}
    {diag::Span measured(profile_,diag::Records);
        for(quint32 i=0;i<count;++i){const auto* record=data+std::size_t(i)*32;const auto f=read16(record+22);
            if(read64(record)!=first+i||read64(record+8)!=anchor+quint64(first+i)*step||!(f&1u)||(f&~31u)||((f&2u)&&(!read32(record+24)||(f&4u)))){reject("Preview record index/tick/flags validation failed");return;}}}
    object["observed_gap_records"]=double(first-lastRecordEnd_);
    liveIdentity_=signature;liveGeneration_=generation;lastFrameSequence_=sequence;lastRecordEnd_=first+count;tickAnchor_=anchor;haveTickAnchor_=true;
    profile_.count(diag::AcceptedFrames);verifyTime.finish();
    {diag::Span measured(profile_,diag::SignalDelivery);emit liveFrame(object);}
}
}
