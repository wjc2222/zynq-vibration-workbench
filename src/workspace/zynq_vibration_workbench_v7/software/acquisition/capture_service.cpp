// Local JSONL capture service. No shell commands or legacy PC protocol are used.
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>
#include <vibration/core.hpp>
#include "capture_output.hpp"
#include "capture_profile.hpp"
#include "preview_schedule.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef __linux__
#include <vibration/ax7z020_capture.h>
#include <vibration/ax7z020_capture_preview.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
namespace diag=vibration::capture_diag;
const QString schema = QStringLiteral("vibration.capture.v2");
constexpr std::uint32_t maxRecords = 2097024, clockHz = 50000000;
const std::vector<int> rates{50000,25000,10000,5000};
struct Failure : std::runtime_error {
    QString code;
    Failure(QString c, const QString& m):std::runtime_error(m.toStdString()),code(std::move(c)){}
};
[[noreturn]] void fail(const char* c, const QString& m) { throw Failure(QString::fromLatin1(c),m); }
QString errorText(const QString& prefix) { return prefix+QStringLiteral(": ")+QString::fromLocal8Bit(std::strerror(errno)); }
CaptureOutput* output=nullptr;
void emitJson(const QJsonObject& object) {
    static std::atomic<bool> warned{false};
    if(!output||!output->enqueue(object))if(!warned.exchange(true))std::cerr<<"Capture output queue/line limit reached; acquisition remains independent\n";
}
void keys(const QJsonObject& o, const std::set<QString>& allowed) {
    for(auto i=o.begin();i!=o.end();++i)if(!allowed.count(i.key()))fail("INVALID_REQUEST","Unknown field: "+i.key());
}
int integer(const QJsonValue& v,int lo,int hi,const QString& label) {
    if(!v.isDouble()||!std::isfinite(v.toDouble())||std::floor(v.toDouble())!=v.toDouble()||v.toDouble()<lo||v.toDouble()>hi)
        fail("INVALID_PLAN","Invalid integer "+label);
    return int(v.toDouble());
}
// QJsonDocument accepts duplicate keys. This additional bounded scanner rejects
// them, including escaped spellings, before any request is acted upon.
class UniqueKeys {
    const QByteArray& b;int p=0;
    void ws(){while(p<b.size()&&(b[p]==' '||b[p]=='\t'||b[p]=='\r'||b[p]=='\n'))++p;}
    QString string(){int start=p++;while(p<b.size()){char c=b[p++];if(c=='\\'){++p;continue;}if(c=='"')break;}
        auto d=QJsonDocument::fromJson("["+b.mid(start,p-start)+"]");return d.array().at(0).toString();}
    void value(){ws();if(p>=b.size())return;char c=b[p];if(c=='{'){++p;ws();std::set<QString> seen;
        while(p<b.size()&&b[p]!='}'){ws();auto k=string();if(!seen.insert(k).second)fail("INVALID_REQUEST","Duplicate JSON key");ws();++p;value();ws();if(p<b.size()&&b[p]==','){++p;continue;}break;}++p;
        }else if(c=='['){++p;ws();while(p<b.size()&&b[p]!=']'){value();ws();if(p<b.size()&&b[p]==','){++p;continue;}break;}++p;
        }else if(c=='"'){string();}else{while(p<b.size()&&b[p]!=','&&b[p]!=']'&&b[p]!='}'&&b[p]!=' '&&b[p]!='\r'&&b[p]!='\n'&&b[p]!='\t')++p;}}
public:explicit UniqueKeys(const QByteArray& bytes):b(bytes){} void check(){value();}
};
struct Plan {
    int rate=50000,seconds=10,ppr=1;QString directory;std::uint32_t count=500000;
    QJsonObject json()const{return {{"sample_rate_hz",rate},{"duration_seconds",seconds},{"pulses_per_rev",ppr},{"output_directory",directory},{"requested_records",double(count)},{"expected_file_bytes",4096.0+32.0*count}};}
};
#ifdef __linux__
struct Fd {
    int value=-1;explicit Fd(int n=-1):value(n){}~Fd(){if(value>=0)::close(value);}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
    Fd(Fd&& f)noexcept:value(f.value){f.value=-1;}
    Fd& operator=(Fd&& f)noexcept{if(value>=0)::close(value);value=f.value;f.value=-1;return *this;}
};
Fd directoryFd(const QString& name,bool probe) {
    const std::filesystem::path path(name.toStdString());
    if(!path.is_absolute()||name.contains(QChar('\0')))fail("INVALID_PLAN","Output directory must be absolute");
    Fd current(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    if(current.value<0)fail("FILE_ERROR",errorText("Open root"));
    for(const auto& part:path.relative_path()) {
        auto s=part.string();if(s.empty()||s==".")continue;
        if(s=="..")fail("INVALID_PLAN","Parent traversal is not permitted");
        Fd next(::openat(current.value,s.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        if(next.value<0)fail("INVALID_PLAN",errorText("Output directory must exist and have no symlink components"));
        current=std::move(next);
    }
    if(probe){const auto n=(".capture-probe-"+QUuid::createUuid().toString(QUuid::Id128)).toStdString();
        Fd f(::openat(current.value,n.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
        if(f.value<0)fail("INVALID_PLAN",errorText("Output directory is not writable"));
        if(::unlinkat(current.value,n.c_str(),0))fail("FILE_ERROR",errorText("Remove write probe"));}
    return current;
}
int publishExclusive(int dir,const std::string& temporary,const std::string& final){
    // RENAME_NOREPLACE also works on Linux FAT data volumes, which have no
    // hard links. Never use ordinary rename(), since that could overwrite.
#ifdef SYS_renameat2
    constexpr unsigned noReplace=1;
    if(::syscall(SYS_renameat2,dir,temporary.c_str(),dir,final.c_str(),noReplace)==0)return 0;
    if(errno!=ENOSYS&&errno!=EINVAL&&errno!=EOPNOTSUPP)return -1;
#endif
    // Old kernels/filesystems may still support exclusive hard-link creation.
    // If neither primitive exists, fail closed instead of weakening safety.
    return ::linkat(dir,temporary.c_str(),dir,final.c_str(),0);
}
#endif
Plan parsePlan(const QJsonValue& v) {
    if(!v.isObject())fail("INVALID_PLAN","plan must be an object");
    auto o=v.toObject();
    keys(o,{"sample_rate_hz","duration_seconds","pulses_per_rev","output_directory"});
    Plan p;p.rate=integer(o.value("sample_rate_hz"),1,100000,"sample_rate_hz");
    if(std::find(rates.begin(),rates.end(),p.rate)==rates.end())fail("INVALID_PLAN","Unsupported sample rate");
    p.seconds=integer(o.value("duration_seconds"),1,40,"duration_seconds");
    p.ppr=integer(o.value("pulses_per_rev"),1,4096,"pulses_per_rev");
    if(!o.value("output_directory").isString())fail("INVALID_PLAN","output_directory must be a string");
    p.directory=o.value("output_directory").toString();p.count=std::uint32_t(p.rate*p.seconds);
    if(p.count>maxRecords)fail("INVALID_PLAN","Capture exceeds reserved DMA capacity");
#ifdef __linux__
    (void)directoryFd(p.directory,true);
#else
    if(!QDir::isAbsolutePath(p.directory)||!QDir(p.directory).exists())fail("INVALID_PLAN","Output directory must exist and be absolute");
#endif
    return p;
}
std::uint16_t get16(const unsigned char* p){return std::uint16_t(p[0])|(std::uint16_t(p[1])<<8);}
std::uint32_t get32(const unsigned char* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
std::uint64_t get64(const unsigned char* p){return get32(p)|(std::uint64_t(get32(p+4))<<32);}
void put32(unsigned char* p,std::uint32_t v){for(int i=0;i<4;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
void put64(unsigned char* p,std::uint64_t v){put32(p,std::uint32_t(v));put32(p+4,std::uint32_t(v>>32));}
std::uint32_t crc32(const unsigned char* data,std::size_t n,const std::atomic<bool>* cancelled=nullptr){
    // Preview still checks every slice and aggregate CRC. Reuse the verified
    // core's endian-safe slicing-by-8 implementation instead of rescanning one
    // byte per dependency step. Large final-file CRC retains cancel checkpoints.
    if(!cancelled)return vibration::crc32(data,n);
    static const auto table=[] {
        std::array<std::uint32_t,256> t{};
        for(std::uint32_t i=0;i<256;++i){auto c=i;for(int bit=0;bit<8;++bit)c=(c>>1)^(0xedb88320u&(0u-(c&1u)));t[i]=c;}
        return t;
    }();
    std::uint32_t c=~0u;
    for(std::size_t i=0;i<n;++i){
        if(!(i%65536)&&cancelled&&cancelled->load())fail("CANCELLED","Capture cancelled during CRC validation");
        c=table[(c^data[i])&255u]^(c>>8);
    }
    return ~c;
}
struct SourceResult {std::vector<unsigned char> payload;std::uint32_t hw=6,dma=0x1002;bool synthetic=false;std::optional<std::uint32_t> driverCrc;};
struct Job {QString id,state="preparing",output,errorCode,error,generation,previewReason;Plan plan;std::uint32_t completed=0;bool hardware=false,synthetic=false,previewAvailable=false;};
struct PreviewCursor {quint64 generation=0,sequence=0;std::uint32_t next=0;std::optional<std::uint64_t> anchor;bool enabled=false;};
class Service {
    mutable diag::Profile profile;
    bool fixture=false,hardwareSelfTest=false;QString fault;std::mutex mutex;Job job;bool haveJob=false;std::thread worker;std::atomic<bool> cancel{false};std::set<QString> requestIds;quint64 fixtureGeneration=0;
    QJsonObject jobJsonLocked()const{return {{"job_id",job.id},{"state",job.state},{"records_completed",double(job.completed)},{"requested_records",double(job.plan.count)},{"hardware_confirmed",job.hardware},{"synthetic",job.synthetic},{"output_path",job.output},{"error_code",job.errorCode},{"error_message",job.error},{"preview_available",job.previewAvailable},{"preview_reason",job.previewReason},{"generation",job.generation},{"sample_rate_hz",job.plan.rate},{"ppr",job.plan.ppr},{"plan",job.plan.json()}};}
    static bool terminal(const QString& s){return s=="completed"||s=="failed"||s=="cancelled";}
    void eventLocked(){auto v=jobJsonLocked();v["schema"]=schema;v["type"]="event";v["event"]="job_status";emitJson(v);}
    void stage(const char* s,std::uint32_t n=0){std::lock_guard<std::mutex> lock(mutex);job.state=QString::fromLatin1(s);job.completed=n;eventLocked();}
    void checkCancel(){if(cancel.load())fail("CANCELLED","Capture cancelled");}
    QJsonObject baseCaps()const {
        QJsonArray r;for(auto rate:rates)r.append(rate);
        return {{"backend",fixture?"test_fixture":"linux_device"},{"available",fixture},{"hardware_confirmed",false},{"synthetic",fixture},{"requested_source",fixture?"host_fixture":hardwareSelfTest?"hardware_self_test":"adc"},{"actual_source",fixture?"host_fixture":"unknown"},{"device","/dev/ax7z020-capture"},{"supported_rates_hz",r},{"max_records",double(maxRecords)},{"clock_hz",double(clockHz)},{"adc_range_uv",5000000},{"channel_map",328449},{"speed_filter_cycles",5},{"speed_timeout_cycles",100000000},{"reason",fixture?"Explicit host test fixture":"Capture device unavailable"},{"preview_available",fixture&&fault!="preview_unavailable"},{"preview_reason",fixture?(fault=="preview_unavailable"?"Explicit fixture without committed preview":""):"Committed SG preview not confirmed"},{"preview_interval_ms",40},{"preview_max_records",4096},{"preview_line_max_bytes",196608}};
    }
    void disablePreview(PreviewCursor& cursor,const QString& reason){cursor.enabled=false;std::lock_guard<std::mutex> lock(mutex);job.previewAvailable=false;job.previewReason=reason;eventLocked();}
    void previewFrame(const Plan& p,PreviewCursor& cursor,const QByteArray& raw,std::uint32_t first,std::uint32_t committed,bool test,std::uint32_t validity){
        checkCancel();const auto count=std::uint32_t(raw.size()/32);
        if(raw.isEmpty()||raw.size()%32||count>4096||first+count>committed||committed>p.count||(validity&27u)!=27u||(validity&~31u))fail("PREVIEW_ERROR","Invalid committed preview bounds/validity");
        const auto* data=reinterpret_cast<const unsigned char*>(raw.constData());const auto step=clockHz/p.rate;
        const auto tick=get64(data+8),offset=std::uint64_t(first)*step;
        if(tick<offset)fail("PREVIEW_ERROR","Preview timestamp underflow");
        const auto anchor=tick-offset;
        if(cursor.anchor&&*cursor.anchor!=anchor)fail("PREVIEW_ERROR","Preview timestamp anchor changed");
        if(anchor>std::numeric_limits<std::uint64_t>::max()-std::uint64_t(first+count-1)*step)fail("PREVIEW_ERROR","Preview timestamp overflow");
        diag::Span recordTime(profile,diag::PreviewRecords);
        for(std::uint32_t i=0;i<count;++i){const auto* record=data+std::size_t(i)*32;const auto flags=get16(record+22);
            if(get64(record)!=first+i||get64(record+8)!=anchor+std::uint64_t(first+i)*step||!(flags&1u)||(flags&~31u)||((flags&2u)&&(!get32(record+24)||(flags&4u))))fail("PREVIEW_ERROR","Committed preview record validation failed");}
        recordTime.finish();
        cursor.anchor=anchor;cursor.next=first+count;
        std::uint32_t crc;{diag::Span measured(profile,diag::FrameCrc);crc=crc32(data,std::size_t(raw.size()));}if(fixture&&fault=="preview_crc")crc^=1;
        QString encoded;{diag::Span measured(profile,diag::Base64Encode);encoded=QString::fromLatin1(raw.toBase64());}
        diag::Span buildTime(profile,diag::BuildEvent);
        QJsonObject frame{{"schema",schema},{"type","event"},{"event","preview"},{"job_id",job.id},{"generation",QString::number(cursor.generation)},{"frame_seq",double(++cursor.sequence)},{"first_record",double(first)},{"record_count",double(count)},{"committed_records",double(committed)},{"requested_records",double(p.count)},{"sample_rate_hz",p.rate},{"clock_hz",double(clockHz)},{"ppr",p.ppr},{"adc_range_uv",5000000},{"channel_map",328449},{"record_bytes",32},{"source",fixture?"test_fixture":test?"hardware_test":"adc_unconfirmed"},{"hardware_confirmed",!fixture},{"synthetic",fixture||test},{"source_flags",test||fixture?1:0},{"validity_flags",double(validity)},{"raw_base64",encoded},{"crc32",double(crc)},{"dropped_preview_records",0}};
        buildTime.finish();profile.count(diag::AcceptedFrames);
        {diag::Span measured(profile,diag::PreviewEnqueue);emitJson(frame);}
    }
#ifdef __linux__
    bool previewInfo(int fd,const ax7z020_capture_caps& caps,ax7z020_capture_preview_info& info)const{
        constexpr auto required=AX7Z020_CAPTURE_F_PREVIEW_COMMITTED|AX7Z020_CAPTURE_F_SG_CHAIN;
        if((caps.features&required)!=required)return false;
        {diag::Span measured(profile,diag::InfoIoctl);if(::ioctl(fd,AX7Z020_CAPTURE_GET_PREVIEW_INFO,&info))return false;}
        return info.abi_version==AX7Z020_CAPTURE_PREVIEW_ABI&&info.record_bytes==32&&(info.features&required)==required;
    }
    void collectPreview(int fd,const ax7z020_capture_caps& caps,const Plan& p,PreviewCursor& cursor){
        if(!cursor.enabled)return;
        diag::Span pollTime(profile,diag::PreviewPoll);
        try{
            ax7z020_capture_preview_info info{};
            if(!previewInfo(fd,caps,info))fail("PREVIEW_ERROR","Committed SG INFO unavailable");
            if(!info.capture_id||info.sample_rate_hz!=std::uint32_t(p.rate)||info.requested_records!=p.count||info.committed_records>p.count||(info.source_flags&~1u)||bool(info.source_flags&1u)!=hardwareSelfTest)fail("PREVIEW_ERROR","Committed preview identity/source mismatch");
            if(cursor.generation&&cursor.generation!=info.capture_id)fail("PREVIEW_ERROR","Preview generation changed");
            cursor.generation=info.capture_id;
            {std::lock_guard<std::mutex> lock(mutex);job.generation=QString::number(cursor.generation);}
            const auto end=info.committed_records;if(end<=cursor.next)return;
            const auto first=end-cursor.next>4096?end-4096:cursor.next;
            QByteArray raw;raw.reserve(int((end-first)*32));std::uint32_t validity=31;
            for(std::uint32_t next=first;next<end;){
                checkCancel();const auto wanted=std::min(256u,end-next);ax7z020_capture_preview_copy copy{};copy.abi_version=AX7Z020_CAPTURE_PREVIEW_ABI;copy.capture_id=cursor.generation;copy.first_record=next;copy.requested_records=wanted;
                int result;{diag::Span measured(profile,diag::CopyIoctl);result=::ioctl(fd,AX7Z020_CAPTURE_COPY_PREVIEW,&copy);}
                if(result){if(errno==EAGAIN)return;fail("PREVIEW_ERROR",errorText("Copy committed preview"));}
                if(copy.abi_version!=1||copy.capture_id!=cursor.generation||copy.first_record!=next||copy.requested_records!=wanted||!copy.record_count||copy.record_count>wanted||copy.record_bytes!=32||copy.sample_rate_hz!=std::uint32_t(p.rate)||copy.source_flags!=info.source_flags||copy.error_code||copy.committed_records<next+copy.record_count||copy.committed_records>p.count||(copy.validity_flags&27u)!=27u||(copy.validity_flags&~31u))fail("PREVIEW_ERROR","Committed COPY validation failed");
                const auto bytes=std::size_t(copy.record_count)*32;{diag::Span measured(profile,diag::SliceCrc);if(crc32(copy.records,bytes)!=copy.payload_crc32)fail("PREVIEW_ERROR","Committed COPY CRC mismatch");}profile.count(diag::CopiedRecords,copy.record_count);
                raw.append(reinterpret_cast<const char*>(copy.records),int(bytes));validity&=copy.validity_flags;next+=copy.record_count;
            }
            previewFrame(p,cursor,raw,first,end,bool(info.source_flags&1u),validity);
        }catch(const Failure& e){if(e.code=="CANCELLED")throw;disablePreview(cursor,QString::fromUtf8(e.what()));}
    }
    static void validateCaps(const ax7z020_capture_caps& c){
        if(c.abi_version!=AX7Z020_CAPTURE_ABI||c.hardware_id!=AX7Z020_CAPTURE_HW_ID||c.revision!=AX7Z020_CAPTURE_REVISION||
            c.clock_hz!=clockHz||c.max_records!=maxRecords||c.record_bytes!=32||c.rate_mask!=15||!(c.features&AX7Z020_CAPTURE_F_SELECTABLE_RATE)||
            c.adc_range_uv!=5000000||c.channel_map!=0x50301||c.speed_filter_cycles!=5||c.speed_timeout_cycles!=100000000)
            fail("UNAVAILABLE","Capture driver/hardware capability mismatch");
    }
    static bool sourceFromStatus(const ax7z020_capture_status& s){
        if(s.abi_version!=AX7Z020_CAPTURE_ABI)fail("INTEGRITY_ERROR","Status ABI mismatch");
        const bool test=bool(s.flags&AX7Z020_CAPTURE_F_TEST_PATTERN);
        if(test!=bool(s.hw_status&(1u<<6)))fail("INTEGRITY_ERROR","Driver/hardware test-source flags disagree");
        return test;
    }
    Fd device(ax7z020_capture_caps& c,ax7z020_capture_status& s){
        Fd d(::open("/dev/ax7z020-capture",O_RDWR|O_CLOEXEC|O_NOFOLLOW));
        if(d.value<0)fail("UNAVAILABLE",errorText("Open capture device"));
        if(::ioctl(d.value,AX7Z020_CAPTURE_GET_CAPS,&c)||::ioctl(d.value,AX7Z020_CAPTURE_GET_STATUS,&s))fail("UNAVAILABLE",errorText("Capture capability ioctl"));
        validateCaps(c);(void)sourceFromStatus(s);
        if(hardwareSelfTest&&!(c.features&AX7Z020_CAPTURE_F_TEST_PATTERN))fail("UNAVAILABLE","Hardware self-test capability unavailable");
        return d;
    }
#endif
    QJsonObject capabilities(){auto c=baseCaps();if(fixture)return c;
        {std::lock_guard<std::mutex> lock(mutex);if(haveJob&&!terminal(job.state)){c["available"]=true;c["hardware_confirmed"]=job.hardware;c["synthetic"]=job.synthetic;c["actual_source"]=fixture?"host_fixture":job.synthetic?"hardware_self_test":"adc";c["reason"]="Capture device owned by active job";c["preview_available"]=job.previewAvailable;c["preview_reason"]=job.previewReason;return c;}}
#ifdef __linux__
        try{ax7z020_capture_caps cap{};ax7z020_capture_status status{};auto fd=device(cap,status);c["available"]=true;c["hardware_confirmed"]=true;c["synthetic"]=sourceFromStatus(status);c["actual_source"]=sourceFromStatus(status)?"hardware_self_test":"adc";c["reason"]="";ax7z020_capture_preview_info info{};const bool available=previewInfo(fd.value,cap,info);c["preview_available"]=available;c["preview_reason"]=available?"":"Driver has no verified committed SG preview";}
        catch(const Failure& e){c["reason"]=QString::fromUtf8(e.what());}
#endif
        return c;
    }
    SourceResult acquire(const Plan& p){diag::Span acquireTime(profile,diag::Acquire);SourceResult r;r.synthetic=fixture;PreviewCursor preview;{std::lock_guard<std::mutex> lock(mutex);preview.enabled=job.previewAvailable;}
        stage("running");
        if(fixture){if(fault=="driver")fail("DEVICE_ERROR","Injected fixture device failure");r.payload.resize(std::size_t(p.count)*32);
            for(std::uint32_t i=0;i<p.count;++i){if(!(i%1024))checkCancel();auto* b=r.payload.data()+std::size_t(i)*32;put64(b,i);put64(b+8,100000+std::uint64_t(i)*(clockHz/p.rate));
                auto sample=std::int16_t(1000*std::sin(2*3.141592653589793*100*i/p.rate));for(int ch=0;ch<3;++ch){b[16+ch*2]=sample&255;b[17+ch*2]=(std::uint16_t(sample)>>8)&255;}b[22]=5;}
            preview.generation=++fixtureGeneration;{std::lock_guard<std::mutex> lock(mutex);job.generation=QString::number(preview.generation);}
            auto deadline=std::chrono::steady_clock::now();
            for(std::uint32_t completed=0;completed<p.count;){
                deadline+=std::chrono::milliseconds(40);
                while(std::chrono::steady_clock::now()<deadline){checkCancel();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
                completed=std::min(p.count,completed+std::uint32_t(p.rate/25));stage("running",completed);
                if(preview.enabled){const auto first=completed-preview.next>4096?completed-4096:preview.next;const auto raw=QByteArray(reinterpret_cast<const char*>(r.payload.data()+std::size_t(first)*32),int((completed-first)*32));previewFrame(p,preview,raw,first,completed,true,completed==p.count?31u:27u);}
            }
            if(fault=="index")r.payload[0]=1;
            if(fault=="tick")r.payload[40]^=1;
            if(fault=="flags")r.payload[22]=0;
            if(fault=="short")r.payload.resize(r.payload.size()-32);
        }else{
#ifdef __linux__
            ax7z020_capture_caps cap{};ax7z020_capture_status s{};auto d=device(cap,s);
            ax7z020_capture_plan plan{};plan.abi_version=AX7Z020_CAPTURE_ABI;plan.sample_rate_hz=p.rate;plan.requested_records=p.count;plan.flags=hardwareSelfTest?AX7Z020_CAPTURE_START_TEST_PATTERN:0;
            checkCancel();if(::ioctl(d.value,AX7Z020_CAPTURE_START,&plan))fail("DEVICE_ERROR",errorText("Start capture"));
            struct CancelGuard {int fd;bool done=false;~CancelGuard(){if(!done)(void)::ioctl(fd,AX7Z020_CAPTURE_CANCEL);}} guard{d.value};
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(p.seconds+8);std::uint32_t last=std::numeric_limits<std::uint32_t>::max();auto nextPreview=std::chrono::steady_clock::now()+std::chrono::milliseconds(40);
            for(;;){checkCancel();{diag::Span measured(profile,diag::StatusIoctl);if(::ioctl(d.value,AX7Z020_CAPTURE_GET_STATUS,&s))fail("DEVICE_ERROR",errorText("Capture status"));}
                const bool actualTest=sourceFromStatus(s);
                if(actualTest!=hardwareSelfTest)fail("INTEGRITY_ERROR","Requested capture source differs from driver/hardware readback");
                {std::lock_guard<std::mutex> lock(mutex);job.synthetic=actualTest;}
                if(s.captured_records!=last){last=s.captured_records;stage("running",last);}
                if(std::chrono::steady_clock::now()>=nextPreview||s.state==AX7Z020_CAPTURE_READY){collectPreview(d.value,cap,p,preview);nextPreview=vibration::transport::nextPreviewSlot(nextPreview,std::chrono::steady_clock::now());}
                if(s.state==AX7Z020_CAPTURE_READY)break;
                if(s.state==AX7Z020_CAPTURE_FAILED||s.state==AX7Z020_CAPTURE_CANCELLED)fail("DEVICE_ERROR",QString("Driver terminal state %1 error %2").arg(s.state).arg(s.error_code));
                if(std::chrono::steady_clock::now()>deadline)fail("DEVICE_ERROR","Capture deadline exceeded");
                auto wake=std::chrono::steady_clock::now()+std::chrono::milliseconds(20);
                if(preview.enabled)wake=std::min(wake,nextPreview);
                if(wake>std::chrono::steady_clock::now())std::this_thread::sleep_until(wake);
                else std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            if(s.error_code||s.sample_rate_hz!=std::uint32_t(p.rate)||s.requested_records!=p.count||s.captured_records!=p.count||s.sent_records!=p.count||s.dropped_records||s.dma_length!=p.count*32u||(s.dma_status&0x770u)||!(s.dma_status&0x1000u))
                fail("INTEGRITY_ERROR","Driver count/rate/length/error readback mismatch");
            r.hw=s.hw_status;r.dma=s.dma_status;r.synthetic=sourceFromStatus(s);
            {std::lock_guard<std::mutex> lock(mutex);job.synthetic=r.synthetic;}
            r.payload.resize(std::size_t(p.count)*32);std::size_t off=0;
            diag::Span readTime(profile,diag::FinalRead);
            while(off<r.payload.size()){checkCancel();auto n=::read(d.value,r.payload.data()+off,std::min<std::size_t>(65536,r.payload.size()-off));if(n<0&&errno==EINTR)continue;if(n<=0)fail("DEVICE_ERROR",errorText("Read capture payload"));off+=std::size_t(n);}
            readTime.finish();
            constexpr auto requiredPreview=AX7Z020_CAPTURE_F_PREVIEW_COMMITTED|AX7Z020_CAPTURE_F_SG_CHAIN;
            if((cap.features&requiredPreview)==requiredPreview){
                ax7z020_capture_preview_info finalInfo{};
                if(!previewInfo(d.value,cap,finalInfo)||!finalInfo.capture_id||(preview.generation&&finalInfo.capture_id!=preview.generation)||finalInfo.state!=AX7Z020_CAPTURE_READY||finalInfo.committed_records!=p.count||finalInfo.requested_records!=p.count||finalInfo.sample_rate_hz!=std::uint32_t(p.rate)||finalInfo.source_flags!=(r.synthetic?1u:0u)||finalInfo.error_code)fail("INTEGRITY_ERROR","Final committed driver prefix cannot be verified");
                r.driverCrc=finalInfo.committed_crc32;
            }
            guard.done=true;
#else
            fail("UNAVAILABLE","Linux capture device backend required");
#endif
        }
        return r;
    }
    std::vector<unsigned char> header(const Plan& p,const SourceResult& r){diag::Span headerTime(profile,diag::FinalHeader);stage("validating",p.count);checkCancel();
        if(r.payload.size()!=std::size_t(p.count)*32)fail("INTEGRITY_ERROR","Payload record count mismatch");
        const auto first=get64(r.payload.data()+8);const auto step=clockHz/p.rate;
        if(first>std::numeric_limits<std::uint64_t>::max()-std::uint64_t(p.count-1)*step)fail("INTEGRITY_ERROR","Timestamp overflow");
        for(std::uint32_t i=0;i<p.count;++i){if(!(i%1024))checkCancel();const auto* b=r.payload.data()+std::size_t(i)*32;const auto flags=get16(b+22);
            if(get64(b)!=i||get64(b+8)!=first+std::uint64_t(i)*step||!(flags&1u)||(flags&~31u)||((flags&2u)&&(!get32(b+24)||(flags&4u))))fail("INTEGRITY_ERROR",QString("Invalid record %1").arg(i));}
        std::vector<unsigned char> h(4096);std::memcpy(h.data(),"CAPDDR2",8);
        const std::uint32_t values[]={2,4096,32,2,std::uint32_t(p.rate),clockHz,p.count,p.count,p.count*32u,0,r.hw,r.dma,0,std::uint32_t(p.ppr)};
        for(int i=0;i<14;++i)put32(h.data()+8+i*4,values[i]);
        put64(h.data()+64,first);put64(h.data()+72,get64(r.payload.data()+r.payload.size()-24));
        put32(h.data()+80,0x50301);put32(h.data()+84,5000000);put32(h.data()+88,5);put32(h.data()+92,100000000);
        auto crc=crc32(r.payload.data(),r.payload.size(),&cancel);if(r.driverCrc&&crc!=*r.driverCrc)fail("INTEGRITY_ERROR","Full raw payload differs from driver committed CRC");if(fixture&&fault=="crc")crc^=1;put32(h.data()+96,crc);put32(h.data()+100,crc32(h.data(),128));return h;
    }
    QString save(const Plan& p,const SourceResult& r,const std::vector<unsigned char>& h){diag::Span saveTime(profile,diag::FinalSave);stage("saving",p.count);checkCancel();
#ifdef __linux__
        auto dir=directoryFd(p.directory,false);const auto uuid=QUuid::createUuid().toString(QUuid::Id128);
        const auto stem="CAP_"+QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz")+"_"+uuid;
        const auto final=(stem+".BIN").toStdString(),meta=(stem+".meta.json").toStdString();
        const auto temp=(".capture-"+uuid+".part").toStdString(),mt=(".capture-"+uuid+".meta.part").toStdString();
        bool tempCreated=false,metaTemp=false,metaPublished=false,binPublished=false,committed=false;
        struct Cleanup {int fd;const std::string& t;const std::string& mt;const std::string& m;const std::string& f;bool &tc,&mc,&mp,&bp,&ok;
            ~Cleanup(){if(tc)(void)::unlinkat(fd,t.c_str(),0);if(mc)(void)::unlinkat(fd,mt.c_str(),0);if(!ok){if(bp)(void)::unlinkat(fd,f.c_str(),0);if(mp)(void)::unlinkat(fd,m.c_str(),0);(void)::fsync(fd);}}}cleanup{dir.value,temp,mt,meta,final,tempCreated,metaTemp,metaPublished,binPublished,committed};
        Fd f(::openat(dir.value,temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));if(f.value<0)fail("FILE_ERROR",errorText("Create capture staging file"));tempCreated=true;
        auto writeAll=[this](int fd,const unsigned char* b,std::size_t n){std::size_t off=0;while(off<n){checkCancel();auto done=::write(fd,b+off,std::min<std::size_t>(65536,n-off));if(done<0&&errno==EINTR)continue;if(done<=0)fail("FILE_ERROR",errorText("Write staging file"));off+=std::size_t(done);}};
        writeAll(f.value,h.data(),h.size());writeAll(f.value,r.payload.data(),r.payload.size());if(::fsync(f.value))fail("FILE_ERROR",errorText("Flush capture"));
        QJsonObject metadata{{"synthetic",r.synthetic},{"source_kind",r.synthetic?"synthetic":"unknown"},{"calibration_confirmed",false},{"mechanical_vibration_confirmed",false},{"backend",fixture?"test_fixture":"linux_device"},{"hardware_test_source",r.synthetic&&!fixture},{"hardware_confirmed",!fixture},{"schema",schema},{"capture_parameters",p.json()},{"driver_hw_status",double(r.hw)},{"driver_dma_status",double(r.dma)},{"payload_crc32",double(get32(h.data()+96))},{"ppr_source","explicit_user_confirmed"}};
        metadata["requested_source"]=fixture?"host_fixture":hardwareSelfTest?"hardware_self_test":"adc";
        metadata["actual_source"]=fixture?"host_fixture":r.synthetic?"hardware_self_test":"adc";
        auto mb=QJsonDocument(metadata).toJson(QJsonDocument::Indented);Fd mf(::openat(dir.value,mt.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));if(mf.value<0)fail("FILE_ERROR",errorText("Create metadata"));metaTemp=true;
        writeAll(mf.value,reinterpret_cast<const unsigned char*>(mb.constData()),std::size_t(mb.size()));if(::fsync(mf.value))fail("FILE_ERROR",errorText("Flush metadata"));
        const auto staging="/proc/self/fd/"+std::to_string(dir.value)+"/"+temp;
        try{auto capture=vibration::load_capture(staging);if(capture.header.pulses_per_rev!=std::uint32_t(p.ppr))fail("INTEGRITY_ERROR","PPR header mismatch");}catch(const vibration::AnalysisError& e){fail("INTEGRITY_ERROR",QString::fromUtf8(e.what()));}
        checkCancel();std::lock_guard<std::mutex> lock(mutex);checkCancel();
        if(publishExclusive(dir.value,mt,meta))fail("FILE_ERROR",errorText("Publish unique metadata"));
        metaPublished=true;
        // On FAT rename dirties the moved inode's new directory entry. Its
        // pre-rename fsync and a parent-only fsync do not persist that inode.
        if(::fsync(mf.value))fail("FILE_ERROR",errorText("Flush renamed metadata inode"));
        if(::fsync(dir.value))fail("FILE_ERROR",errorText("Flush metadata directory"));
        if(fixture&&fault=="publication")fail("FILE_ERROR","Injected failure after metadata publication");
        if(publishExclusive(dir.value,temp,final))fail("FILE_ERROR",errorText("Publish unique capture"));
        binPublished=true;
        if(::fsync(f.value))fail("FILE_ERROR",errorText("Flush renamed capture inode"));
        if(::fsync(dir.value))fail("FILE_ERROR",errorText("Flush capture directory"));
        // No cancellation can enter after commit while this lock is held.
        job.output=QDir(p.directory).absoluteFilePath(QString::fromStdString(final));job.state="completed";job.completed=p.count;committed=true;eventLocked();return job.output;
#else
        (void)p;(void)r;(void)h;fail("UNAVAILABLE","Safe publication requires Linux filesystem backend");
#endif
    }
    void run(Plan p){try{auto r=acquire(p);auto h=header(p,r);(void)save(p,r,h);}catch(const Failure& e){std::lock_guard<std::mutex> lock(mutex);job.state=e.code=="CANCELLED"?"cancelled":"failed";job.errorCode=e.code;job.error=QString::fromUtf8(e.what());job.output.clear();eventLocked();}
        catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);job.state="failed";job.errorCode="DEVICE_ERROR";job.error=QString::fromUtf8(e.what());job.output.clear();eventLocked();}
        if(profile.enabled())diag::emitDiagnostic({{"component","service"},{"phase","job_terminal"},{"profile",profile.snapshot()},{"output",output?output->transportProfile():QJsonObject{}}});}
public:
    Service(bool f,bool selfTest,QString failure):fixture(f),hardwareSelfTest(selfTest),fault(std::move(failure)){}
    ~Service(){cancel=true;if(worker.joinable())worker.join();if(profile.enabled())diag::emitDiagnostic({{"component","service"},{"phase","service_exit"},{"profile",profile.snapshot()},{"output",output?output->transportProfile():QJsonObject{}}});}
    bool request(const QByteArray& line){QJsonValue id=QJsonValue(QJsonValue::Null);QString command;
        auto response=[&](bool ok,const QJsonObject& data,const QString& code={},const QString& message={}){QJsonObject o{{"schema",schema},{"type","response"},{"request_id",id},{"command",command},{"ok",ok}};if(ok)o["data"]=data;else o["error"]=QJsonObject{{"code",code},{"message",message}};emitJson(o);};
        try{QJsonParseError error;auto d=QJsonDocument::fromJson(line,&error);if(error.error!=QJsonParseError::NoError||!d.isObject())fail("INVALID_JSON","Request must be a JSON object");UniqueKeys(line).check();auto o=d.object();
            if(!o.value("request_id").isString()||!QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(o.value("request_id").toString()).hasMatch())fail("INVALID_REQUEST","Invalid request_id");
            id=o.value("request_id");
            if(!o.value("command").isString())fail("INVALID_REQUEST","command must be a string");
            command=o.value("command").toString();
            if(o.value("schema")!=schema)fail("INVALID_REQUEST","Unsupported schema");
            if(requestIds.size()>=65536)fail("INVALID_REQUEST","Request count limit reached; restart service");
            if(!requestIds.insert(id.toString()).second)fail("DUPLICATE_REQUEST","request_id already used");
            if(command=="capabilities"){keys(o,{"schema","request_id","command"});response(true,capabilities());}
            else if(command=="validate_plan"||command=="start"){
                if(command=="start"){keys(o,{"schema","request_id","command","plan","preview"});if(o.contains("preview")&&!o.value("preview").isBool())fail("INVALID_REQUEST","preview must be a boolean");}
                else keys(o,{"schema","request_id","command","plan"});
                const bool wantsPreview=command=="start"&&o.value("preview").toBool(false);auto p=parsePlan(o.value("plan"));
                if(command=="validate_plan"){auto c=capabilities();response(true,{{"valid",true},{"ready",c.value("available")},{"hardware_confirmed",c.value("hardware_confirmed")},{"plan",p.json()},{"capabilities",c}});}
                else{{std::lock_guard<std::mutex> lock(mutex);if(haveJob&&!terminal(job.state))fail("BUSY","Capture already active");}if(worker.joinable())worker.join();auto c=capabilities();if(!c.value("available").toBool())fail("UNAVAILABLE",c.value("reason").toString());
                    {std::lock_guard<std::mutex> lock(mutex);if(output)output->discardPreview();job=Job{};job.id=QUuid::createUuid().toString(QUuid::Id128);job.plan=p;job.hardware=c.value("hardware_confirmed").toBool();job.synthetic=fixture||c.value("synthetic").toBool();job.previewAvailable=wantsPreview&&c.value("preview_available").toBool();job.previewReason=wantsPreview?c.value("preview_reason").toString():"Preview not requested";haveJob=true;cancel=false;response(true,{{"job_id",job.id},{"state",job.state},{"plan",p.json()},{"preview_available",job.previewAvailable},{"preview_reason",job.previewReason}});eventLocked();}worker=std::thread([this,p]{run(p);});}}
            else if(command=="status"||command=="cancel"){keys(o,{"schema","request_id","command","job_id"});if(!o.value("job_id").isString())fail("INVALID_REQUEST","job_id must be a string");std::lock_guard<std::mutex> lock(mutex);if(!haveJob||o.value("job_id").toString()!=job.id)fail("UNKNOWN_JOB","Unknown job_id");
                if(command=="status")response(true,jobJsonLocked());else{bool accepted=!terminal(job.state);if(accepted)cancel=true;response(true,{{"job_id",job.id},{"accepted",accepted},{"state",job.state}});}}
            else if(command=="shutdown"){keys(o,{"schema","request_id","command"});cancel=true;response(true,{{"accepted",true}});return false;}
            else fail("INVALID_REQUEST","Unknown command");
        }catch(const Failure& e){response(false,{},e.code,QString::fromUtf8(e.what()));}
        catch(const std::exception& e){response(false,{},"INVALID_REQUEST",QString::fromUtf8(e.what()));}return true;
    }
    void overlong(){emitJson({{"schema",schema},{"type","response"},{"request_id",QJsonValue(QJsonValue::Null)},{"command",""},{"ok",false},{"error",QJsonObject{{"code","LINE_TOO_LONG"},{"message","JSONL line exceeds 8192 bytes"}}}});}
};
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);bool fixture=false,hardwareSelfTest=false;QString fault;
    for(int i=1;i<argc;++i){QString a=QString::fromLocal8Bit(argv[i]);if(a=="--test-fixture")fixture=true;else if(a=="--hardware-self-test")hardwareSelfTest=true;else if(a.startsWith("--fixture-fault="))fault=a.mid(16);else{std::cerr<<"Unknown option\n";return 2;}}
    if(fixture&&hardwareSelfTest){std::cerr<<"Host fixture and hardware self-test are mutually exclusive\n";return 2;}
    if(!fault.isEmpty()&&(!fixture||!QStringList{"driver","index","tick","flags","short","crc","publication","preview_crc","preview_unavailable"}.contains(fault))){std::cerr<<"Invalid explicit fixture fault\n";return 2;}
    if(fixture)std::cerr<<"Explicit synthetic host test fixture enabled; no hardware confirmation\n";
    if(hardwareSelfTest)std::cerr<<"Explicit hardware self-test requested; real driver/hardware readback is still required\n";
    CaptureOutput channel;output=&channel;Service service(fixture,hardwareSelfTest,fault);QByteArray line;bool discard=false;char c;
    while(std::cin.get(c)){if(c=='\n'){if(discard)service.overlong();else{if(line.endsWith('\r'))line.chop(1);if(!service.request(line))break;}line.clear();discard=false;}
        else if(!discard){if(line.size()>=8192){line.clear();discard=true;}else line.append(c);}}
    // EOF cancels in-flight work. Unterminated input is deliberately not a command.
    return 0;
}
