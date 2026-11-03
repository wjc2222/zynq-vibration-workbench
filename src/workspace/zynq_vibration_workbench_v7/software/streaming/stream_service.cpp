// Additive continuous acquisition service. Production uses the kernel ring ABI;
// --test-fixture is an explicit, synthetic host QA source only.
#include "stream_output.hpp"
#include <vibration/core.hpp>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef __linux__
#include <ax7z020_capture_stream.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace {
constexpr auto schema="vibration.stream.v1";
constexpr std::uint32_t clockHz=50000000;
const std::array<int,4> rates{50000,25000,10000,5000};
struct Failure:std::runtime_error {
    QString code;
    Failure(QString c,QString text):std::runtime_error(text.toStdString()),code(std::move(c)){}
};
[[noreturn]] void fail(const char* code,const QString& text){throw Failure(QString::fromLatin1(code),text);}
QString errnoText(const QString& text){return text+": "+QString::fromLocal8Bit(std::strerror(errno));}
QString number(std::uint64_t n){return QString::number(static_cast<qulonglong>(n));}
void keys(const QJsonObject& o,std::initializer_list<QString> allowed) {
    for(auto i=o.begin();i!=o.end();++i)if(std::find(allowed.begin(),allowed.end(),i.key())==allowed.end())fail("INVALID_REQUEST","Unknown field: "+i.key());
}
int integer(const QJsonValue& v,int lo,int hi,const QString& label) {
    const auto x=v.toDouble(std::numeric_limits<double>::quiet_NaN());
    if(!v.isDouble()||!std::isfinite(x)||std::floor(x)!=x||x<lo||x>hi)fail("INVALID_PLAN","Invalid integer "+label);
    return int(x);
}
std::uint16_t get16(const unsigned char* p){return std::uint16_t(p[0])|(std::uint16_t(p[1])<<8);}
std::uint32_t get32(const unsigned char* p){return std::uint32_t(p[0])|(std::uint32_t(p[1])<<8)|(std::uint32_t(p[2])<<16)|(std::uint32_t(p[3])<<24);}
std::uint64_t get64(const unsigned char* p){return get32(p)|(std::uint64_t(get32(p+4))<<32);}
void put32(unsigned char* p,std::uint32_t n){for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(8*i));}
void put64(unsigned char* p,std::uint64_t n){put32(p,std::uint32_t(n));put32(p+4,std::uint32_t(n>>32));}
std::uint32_t crcUpdate(std::uint32_t c,const unsigned char* p,std::size_t n) {
    static const auto table=[] {std::array<std::uint32_t,256> t{};for(unsigned i=0;i<256;++i){auto x=i;for(unsigned b=0;b<8;++b)x=(x>>1)^((x&1)?0xedb88320u:0);t[i]=x;}return t;}();
    for(std::size_t i=0;i<n;++i)c=table[(c^p[i])&255u]^(c>>8);
    return c;
}
// Qt accepts duplicate JSON keys; validate decoded key names before acting.
class UniqueKeys {
    const QByteArray& b;int pos=0;
    void ws(){while(pos<b.size()&&(b[pos]==' '||b[pos]=='\r'||b[pos]=='\n'||b[pos]=='\t'))++pos;}
    QString string(){int start=pos++;while(pos<b.size()){char c=b[pos++];if(c=='\\'){++pos;continue;}if(c=='"')break;}return QJsonDocument::fromJson("["+b.mid(start,pos-start)+"]").array().at(0).toString();}
    void value(unsigned depth){if(depth>32)fail("INVALID_REQUEST","JSON nesting too deep");ws();if(pos>=b.size())return;char c=b[pos];
        if(c=='{'){++pos;ws();std::vector<QString> seen;while(pos<b.size()&&b[pos]!='}'){ws();auto key=string();if(std::find(seen.begin(),seen.end(),key)!=seen.end())fail("INVALID_REQUEST","Duplicate JSON key");seen.push_back(key);ws();++pos;value(depth+1);ws();if(pos<b.size()&&b[pos]==','){++pos;continue;}break;}++pos;}
        else if(c=='['){++pos;ws();while(pos<b.size()&&b[pos]!=']'){value(depth+1);ws();if(pos<b.size()&&b[pos]==','){++pos;continue;}break;}++pos;}
        else if(c=='"')string();else while(pos<b.size()&&b[pos]!=','&&b[pos]!=']'&&b[pos]!='}'&&b[pos]!=' '&&b[pos]!='\r'&&b[pos]!='\n'&&b[pos]!='\t')++pos;
    }
public:explicit UniqueKeys(const QByteArray& raw):b(raw){}void check(){value(0);}
};
struct Plan {
    QString mode,directory;int rate=50000,seconds=0,ppr=1,segmentSeconds=10;
    std::uint64_t total=0;
    std::uint32_t segmentRecords()const{return std::uint32_t(rate*segmentSeconds);}
    QJsonObject json()const{return {{"mode",mode},{"sample_rate_hz",rate},{"duration_seconds",mode=="timed"?QJsonValue(seconds):QJsonValue()},{"pulses_per_rev",ppr},{"output_directory",directory},{"segment_seconds",segmentSeconds},{"total_records",number(total)}};}
};
#ifdef __linux__
struct Fd {
    int fd=-1;explicit Fd(int n=-1):fd(n){}~Fd(){if(fd>=0)::close(fd);}Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
    Fd(Fd&& x)noexcept:fd(x.fd){x.fd=-1;}Fd& operator=(Fd&& x)noexcept{if(fd>=0)::close(fd);fd=x.fd;x.fd=-1;return *this;}
};
Fd directoryFd(const QString& path,bool probe) {
    const auto p=std::filesystem::u8path(path.toUtf8().constData());
    if(!p.is_absolute()||path.contains(QChar('\0')))fail("INVALID_PLAN","Output directory must be absolute");
    Fd current(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    if(current.fd<0)fail("FILE_ERROR",errnoText("Open root"));
    for(const auto& part:p.relative_path()) {
        const auto name=part.u8string();if(name.empty()||name==".")continue;
        if(name=="..")fail("INVALID_PLAN","Output directory parent traversal is forbidden");
        Fd next(::openat(current.fd,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        if(next.fd<0)fail("INVALID_PLAN",errnoText("Output directory must exist and have no link components"));
        current=std::move(next);
    }
    if(probe) {
        auto name=(".stream-probe-"+QUuid::createUuid().toString(QUuid::Id128)).toStdString();
        Fd file(::openat(current.fd,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
        if(file.fd<0)fail("INVALID_PLAN",errnoText("Output directory is not writable"));
        if(::unlinkat(current.fd,name.c_str(),0))fail("FILE_ERROR",errnoText("Remove stream write probe"));
    }
    return current;
}
std::uint64_t freeBytes(int dir) {
    struct statvfs s{};if(::fstatvfs(dir,&s))fail("FILE_ERROR",errnoText("Read output free space"));
    if(s.f_frsize&&std::uint64_t(s.f_bavail)>std::numeric_limits<std::uint64_t>::max()/s.f_frsize)return std::numeric_limits<std::uint64_t>::max();
    return std::uint64_t(s.f_bavail)*s.f_frsize;
}
void syncFd(int fd,const char* text){if(::fsync(fd))fail("FILE_ERROR",errnoText(QString::fromLatin1(text)));}
#endif
struct Options {
    bool fixture=false,hardwareTest=false;QString fault;int fixtureChunkMs=40;std::uint64_t reserve=64ULL*1024*1024;
    bool synthetic()const{return fixture||hardwareTest;}
    QString source()const{return fixture?"test_fixture":hardwareTest?"hardware_test":"adc_unconfirmed";}
};
Plan parsePlan(const QJsonValue& v,const Options& options) {
    if(!v.isObject())fail("INVALID_PLAN","plan must be an object");
    const auto o=v.toObject();
    keys(o,{"mode","sample_rate_hz","duration_seconds","pulses_per_rev","output_directory","segment_seconds"});
    Plan p;p.mode=o.value("mode").toString();if(p.mode!="timed"&&p.mode!="continuous")fail("INVALID_PLAN","mode must be timed or continuous");
    p.rate=integer(o.value("sample_rate_hz"),1,50000,"sample_rate_hz");
    if(std::find(rates.begin(),rates.end(),p.rate)==rates.end())fail("INVALID_PLAN","Unsupported sample rate");
    if(p.mode=="timed"){p.seconds=integer(o.value("duration_seconds"),1,86400,"duration_seconds");p.total=std::uint64_t(p.rate)*std::uint64_t(p.seconds);}
    else if(o.contains("duration_seconds"))fail("INVALID_PLAN","continuous mode must omit duration_seconds");
    p.ppr=integer(o.value("pulses_per_rev"),1,4096,"pulses_per_rev");
    if(o.contains("segment_seconds"))p.segmentSeconds=integer(o.value("segment_seconds"),1,40,"segment_seconds");
    if(4096ULL+32ULL*p.segmentRecords()>64ULL*1024*1024)fail("INVALID_PLAN","Segment exceeds 64 MiB");
    if(!o.value("output_directory").isString())fail("INVALID_PLAN","output_directory must be a string");
    p.directory=o.value("output_directory").toString();
#ifdef __linux__
    auto dir=directoryFd(p.directory,true);
    const auto needed=options.reserve+4096ULL+32ULL*p.segmentRecords()+16384;
    if(options.fault=="low_space"||freeBytes(dir.fd)<needed)fail("LOW_SPACE","Insufficient free space for one segment and reserve");
#else
    (void)options;fail("UNSUPPORTED_PLATFORM","Stream storage requires POSIX/Linux; host Linux fixtures are supported");
#endif
    return p;
}
struct Job {
    QString id,requestId,state="preparing",errorCode,errorMessage;Plan plan;QByteArray uuid;
    std::uint64_t received=0,durable=0,segments=0;bool hardware=false;
    std::uint32_t hwStatus=0,dmaStatus=0;
    std::chrono::steady_clock::time_point began=std::chrono::steady_clock::now();
};
#ifdef __linux__
class SegmentWriter {
    const Options& options;const Job& job;Fd root,dir,file;std::string temporary,finalName;
    std::uint64_t first=0,firstTick=0,lastTick=0,index=0;std::uint32_t count=0,crc=~0u;bool published=false;
    void writeAll(int fd,const unsigned char* data,std::size_t bytes) {
        std::size_t offset=0;
        while(offset<bytes) {
            std::size_t request=bytes-offset;
            // QA short_write performs a real short positive write then ENOSPC.
            if(options.fault=="short_write"&&count==0&&bytes>4096&&offset>=17)fail("FILE_ERROR","Explicit fixture short write / ENOSPC");
            if(options.fault=="short_write"&&count==0&&bytes>4096)request=std::min<std::size_t>(17,request);
            auto n=::write(fd,data+offset,request);
            if(n>0){offset+=std::size_t(n);continue;}
            if(n<0&&errno==EINTR)continue;
            fail("FILE_ERROR",errnoText("Write segment"));
        }
    }
public:
    SegmentWriter(const Options& opts,const Job& j,std::uint64_t segment):options(opts),job(j),root(directoryFd(j.plan.directory,false)),index(segment) {
        if((options.fault=="storage_after_first"&&index>0)||freeBytes(root.fd)<options.reserve+4096ULL+32ULL*j.plan.segmentRecords()+16384)fail("LOW_SPACE","Free space reserve reached; previously published segments are retained");
        finalName=(j.id+"-"+QString::number(static_cast<qulonglong>(segment)).rightJustified(6,'0')).toStdString();
        temporary="."+finalName+".partial";
        if(::mkdirat(root.fd,temporary.c_str(),0700))fail("FILE_ERROR",errnoText("Create exclusive segment staging directory"));
        try {
            dir=Fd(::openat(root.fd,temporary.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
            if(dir.fd<0)fail("FILE_ERROR",errnoText("Open segment staging directory"));
            file=Fd(::openat(dir.fd,"data.BIN",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
            if(file.fd<0)fail("FILE_ERROR",errnoText("Create exclusive segment payload"));
            std::array<unsigned char,4096> zero{};writeAll(file.fd,zero.data(),zero.size());
        }catch(...) {
            if(dir.fd>=0)(void)::unlinkat(dir.fd,"data.BIN",0);
            (void)::unlinkat(root.fd,temporary.c_str(),AT_REMOVEDIR);throw;
        }
    }
    ~SegmentWriter() {
        // Remove only this writer's unpublished files; never touch user data.
        if(!published&&dir.fd>=0){(void)::unlinkat(dir.fd,"data.BIN",0);(void)::unlinkat(dir.fd,"data.meta.json",0);}
        if(!published&&root.fd>=0&&!temporary.empty())(void)::unlinkat(root.fd,temporary.c_str(),AT_REMOVEDIR);
    }
    void append(const QByteArray& raw) {
        const auto* b=reinterpret_cast<const unsigned char*>(raw.constData());
        const auto n=std::uint32_t(raw.size()/32);
        if(!n||raw.size()%32||count+n>job.plan.segmentRecords())fail("DATA_ERROR","Invalid segment append size");
        if(!count){first=get64(b);firstTick=get64(b+8);}lastTick=get64(b+(n-1)*32+8);
        writeAll(file.fd,b,std::size_t(raw.size()));crc=crcUpdate(crc,b,std::size_t(raw.size()));count+=n;
    }
    std::uint32_t records()const{return count;}
    QJsonObject commit() {
        if(!count)fail("FILE_ERROR","Cannot publish empty segment");
        std::array<unsigned char,4096> h{};std::memcpy(h.data(),"CAPSTRM1",8);
        put32(h.data()+8,1);put32(h.data()+12,4096);put32(h.data()+16,32);put32(h.data()+20,2);
        put32(h.data()+24,job.plan.rate);put32(h.data()+28,clockHz);put32(h.data()+32,count);put32(h.data()+36,count);put32(h.data()+40,count*32);
        put32(h.data()+48,job.hwStatus);put32(h.data()+52,job.dmaStatus);put32(h.data()+60,job.plan.ppr);
        put64(h.data()+64,firstTick);put64(h.data()+72,lastTick);put32(h.data()+80,0x50301);put32(h.data()+84,5000000);put32(h.data()+88,5);put32(h.data()+92,100000000);
        put32(h.data()+96,crc^~0u);put64(h.data()+104,first);std::memcpy(h.data()+112,job.uuid.constData(),16);put64(h.data()+128,index);put64(h.data()+136,job.plan.total);
        put32(h.data()+144,job.plan.segmentRecords());put32(h.data()+148,options.synthetic()?1:0);put32(h.data()+152,4096);put32(h.data()+100,vibration::crc32(h.data(),h.size()));
        if(::lseek(file.fd,0,SEEK_SET)!=0)fail("FILE_ERROR",errnoText("Seek final segment header"));
        writeAll(file.fd,h.data(),h.size());syncFd(file.fd,"Sync segment BIN");
        const auto segmentId=QString::fromStdString(finalName);
        QJsonObject info{{"schema",schema},{"session_id",job.id},{"job_id",job.id},{"segment_id",segmentId},{"segment_index",number(index)},
            {"global_first_index",number(first)},{"record_count",int(count)},{"count",int(count)},{"first_tick",number(firstTick)},{"last_tick",number(lastTick)},
            {"sample_rate_hz",job.plan.rate},{"clock_hz",int(clockHz)},{"ppr",job.plan.ppr},{"record_bytes",32},{"payload_crc32",double(crc^~0u)},
            {"header_crc32",double(get32(h.data()+100))},{"format","CAPSTRM1"},{"synthetic",options.synthetic()},{"source_kind",options.synthetic()?"synthetic":"unconfirmed_adc"},
            {"source",options.source()},{"hardware_confirmed",job.hardware},{"total_records",number(job.plan.total)},
            {"path",QDir(job.plan.directory).filePath(segmentId+"/data.BIN")},{"meta_path",QDir(job.plan.directory).filePath(segmentId+"/data.meta.json")}};
        auto bytes=QJsonDocument(info).toJson(QJsonDocument::Compact)+'\n';
        Fd meta(::openat(dir.fd,"data.meta.json",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
        if(meta.fd<0)fail("FILE_ERROR",errnoText("Create segment metadata"));
        writeAll(meta.fd,reinterpret_cast<const unsigned char*>(bytes.constData()),std::size_t(bytes.size()));syncFd(meta.fd,"Sync segment metadata");syncFd(dir.fd,"Sync segment staging directory");
#ifdef SYS_renameat2
        if(::syscall(SYS_renameat2,root.fd,temporary.c_str(),root.fd,finalName.c_str(),1u))fail("FILE_ERROR",errnoText("Publish segment pair without replacement"));
#else
        fail("UNSUPPORTED_FILESYSTEM","Atomic exclusive directory publication requires renameat2");
#endif
        // FAT rename first creates a destination entry with start cluster 0,
        // then dirties the moved inode. Sync that inode after rename before
        // syncing its parent; the pre-rename sync cannot persist its new entry.
        // After rename the pair exists. Never remove it if either sync fails;
        // caller reports a durability error and previously committed prefix.
        published=true;syncFd(dir.fd,"Sync renamed segment directory inode");syncFd(root.fd,"Sync published segment directory");return info;
    }
};
#endif
class Service {
    Options options;StreamOutput& output;std::mutex mutex;Job job;bool haveJob=false;std::atomic<bool> stop{false};std::thread worker;
    QJsonObject base(const QString& event)const{return {{"schema",schema},{"type","event"},{"event",event},{"id",job.requestId},{"request_id",job.requestId},{"job_id",job.id},{"session_id",job.id},{"source",options.source()},{"emitter","stream_service"},{"synthetic",options.synthetic()},{"hardware_confirmed",job.hardware}};}
    QJsonObject snapshotLocked()const {
        auto v=base("job_status");v["state"]=job.state;v["plan"]=job.plan.json();v["total_records"]=number(job.plan.total);
        v["records_received"]=number(job.received);v["records_captured"]=number(job.received);v["durable_records"]=number(job.durable);v["records_persisted"]=number(job.durable);v["segment_count"]=number(job.segments);
        v["elapsed_seconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-job.began).count();v["directory"]=job.plan.directory;v["error_code"]=job.errorCode;v["error_message"]=job.errorMessage;return v;
    }
    void statusEvent(){QJsonObject v;{std::lock_guard<std::mutex> guard(mutex);v=snapshotLocked();}output.send(v);}
    static bool terminal(const QString& state){return state=="completed"||state=="stopped"||state=="failed";}
    void preview(const QByteArray& raw,std::uint64_t frame) {
        const auto* p=reinterpret_cast<const unsigned char*>(raw.constData());
        QJsonObject v;{std::lock_guard<std::mutex> guard(mutex);v=base("preview");}
        v["frame_seq"]=number(frame);v["global_first_index"]=number(get64(p));v["first_tick"]=number(get64(p+8));v["last_tick"]=number(get64(p+raw.size()-24));v["record_count"]=raw.size()/32;
        v["sample_rate_hz"]=job.plan.rate;v["clock_hz"]=int(clockHz);v["ppr"]=job.plan.ppr;v["adc_range_uv"]=5000000;v["channel_map"]=0x50301;v["record_bytes"]=32;v["crc32"]=double(vibration::crc32(p,std::size_t(raw.size())));v["raw_base64"]=QString::fromLatin1(raw.toBase64());
        (void)output.send(v); // Latest-only preview may be replaced by backpressure.
    }
    void validate(const QByteArray& raw,std::uint64_t next,std::uint64_t& anchor)const {
        const auto expected=job.plan.rate/25;const auto step=clockHz/job.plan.rate;
        if(raw.size()!=expected*32)fail("DATA_ERROR","Driver returned a non-chunk payload");
        const auto* p=reinterpret_cast<const unsigned char*>(raw.constData());if(next==0)anchor=get64(p+8);
        if(next>std::numeric_limits<std::uint64_t>::max()-std::uint64_t(expected)||next+expected-1>(std::numeric_limits<std::uint64_t>::max()-anchor)/step)fail("DATA_ERROR","Global index/tick overflow");
        for(int i=0;i<expected;++i){const auto* r=p+i*32;auto flags=get16(r+22);
            if(get64(r)!=next+std::uint64_t(i)||get64(r+8)!=anchor+(next+std::uint64_t(i))*step||!(flags&1u)||(flags&~31u)||((flags&2u)&&(!get32(r+24)||(flags&4u))))fail("DATA_ERROR","Global index/tick/record flags discontinuity");}
    }
    QByteArray fixtureChunk(std::uint64_t first)const {
        QByteArray raw(job.plan.rate/25*32,'\0');auto* b=reinterpret_cast<unsigned char*>(raw.data());
        for(int i=0;i<raw.size()/32;++i){auto* p=b+i*32;const auto n=first+std::uint64_t(i);put64(p,n);put64(p+8,1000000+n*(clockHz/job.plan.rate));
            for(int ch=0;ch<3;++ch){auto value=static_cast<std::int16_t>(12000*std::sin(6.283185307179586*(31+ch*17)*double(n)/job.plan.rate));p[16+2*ch]=static_cast<unsigned char>(value);p[17+2*ch]=static_cast<unsigned char>(std::uint16_t(value)>>8);}
            p[22]=3;put32(p+24,500000);put32(p+28,std::uint32_t(n/500));}
        if(options.fault=="device_gap"&&first>=std::uint64_t(job.plan.rate))put64(b,first+1);
        return raw;
    }
    void run() {
        QString errorCode,error;bool normal=false;
        try {
#ifdef __linux__
            Fd device;
            std::uint64_t captureId=0;
            if(!options.fixture) {
                device=Fd(::open("/dev/ax7z020-capture",O_RDONLY|O_NONBLOCK|O_CLOEXEC));if(device.fd<0)fail("DEVICE_UNAVAILABLE",errnoText("Open stream capture device"));
                ax7z020_stream_caps caps{};if(::ioctl(device.fd,AX7Z020_STREAM_GET_CAPS,&caps)||caps.abi_version!=AX7Z020_STREAM_ABI||caps.record_bytes!=32||caps.clock_hz!=clockHz||caps.chunk_period_us!=40000||caps.max_chunk_records<2000||(caps.features&7u)!=7u)fail("UNSUPPORTED_DEVICE","Verified boundary-stop streaming ABI unavailable");
                ax7z020_stream_plan plan{};plan.abi_version=AX7Z020_STREAM_ABI;plan.sample_rate_hz=job.plan.rate;plan.total_records=job.plan.total;
                plan.flags=options.hardwareTest?AX7Z020_STREAM_START_TEST_PATTERN:0;
                if(::ioctl(device.fd,AX7Z020_STREAM_START,&plan))fail("DEVICE_ERROR",errnoText("Start streaming ring"));
                {std::lock_guard<std::mutex> guard(mutex);job.hardware=true;}
            }else {job.hwStatus=12u;job.dmaStatus=0;}
            {std::lock_guard<std::mutex> guard(mutex);job.state="running";}statusEvent();
            std::unique_ptr<SegmentWriter> segment;
            std::uint64_t next=0,anchor=0,frame=0;bool stopSent=false;
            auto commit=[&] {
                if(!segment||!segment->records())return;
                auto event=segment->commit();const auto count=segment->records();
                {std::lock_guard<std::mutex> guard(mutex);job.durable+=count;++job.segments;}
                auto envelope=base("segment_ready");for(auto it=event.begin();it!=event.end();++it)envelope[it.key()]=it.value();
                output.send(envelope);segment.reset();statusEvent();
            };
            for(;;) {
                // A reliable-control failure must stop capture, not block the
                // raw worker behind an unread stdout pipe. Drain and publish
                // the same boundary tail even though UI delivery has failed.
                if(output.failed())stop=true;
                if(stop.load()&&!stopSent){stopSent=true;{std::lock_guard<std::mutex> guard(mutex);job.state="stopping";}statusEvent();if(!options.fixture&&::ioctl(device.fd,AX7Z020_STREAM_STOP))fail("DEVICE_ERROR",errnoText("Request boundary stop"));}
                QByteArray raw;
                if(options.fixture) {
                    if((job.plan.total&&next>=job.plan.total)||(stopSent&&next>0))break;
                    if(options.fixtureChunkMs)std::this_thread::sleep_for(std::chrono::milliseconds(options.fixtureChunkMs));
                    raw=fixtureChunk(next);
                } else {
                    raw.resize(job.plan.rate/25*32);const auto got=::read(device.fd,raw.data(),std::size_t(raw.size()));
                    if(got==0)break;
                    if(got<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK){pollfd p{device.fd,POLLIN|POLLERR|POLLHUP,0};(void)::poll(&p,1,20);continue;}fail("DEVICE_ERROR",errnoText("Read verified stream chunk"));}
                    raw.resize(int(got));
                    ax7z020_stream_status status{};
                    if(::ioctl(device.fd,AX7Z020_STREAM_GET_STATUS,&status))fail("DEVICE_ERROR",errnoText("Read stream status"));
                    if(status.abi_version!=AX7Z020_STREAM_ABI||!status.capture_id||(captureId&&captureId!=status.capture_id)||status.sample_rate_hz!=std::uint32_t(job.plan.rate)||status.total_records!=job.plan.total||status.dropped_records||
                       bool(status.flags&AX7Z020_STREAM_STATUS_TEST_PATTERN)!=options.hardwareTest)fail("DATA_ERROR","Stream identity/source/dropped-record status mismatch");
                    captureId=status.capture_id;job.hwStatus=status.hw_status;job.dmaStatus=status.dma_status;
                }
                validate(raw,next,anchor);
                if(job.plan.total&&next+std::uint64_t(raw.size()/32)>job.plan.total)fail("DATA_ERROR","Driver exceeded timed total");
                next+=std::uint64_t(raw.size()/32);
                {std::lock_guard<std::mutex> guard(mutex);job.received=next;}
                if(!segment)segment=std::make_unique<SegmentWriter>(options,job,job.segments);
                segment->append(raw);
                preview(raw,++frame);
                if(segment->records()==job.plan.segmentRecords())commit();
            }
            commit();
            if(job.plan.total&&next!=job.plan.total&&!stopSent)fail("INCOMPLETE_CAPTURE","Device ended before the requested timed total");
            normal=true;
#else
            fail("UNSUPPORTED_PLATFORM","Production stream device and fixture storage require Linux");
#endif
        }catch(const Failure& f){errorCode=f.code;error=QString::fromUtf8(f.what());}
        catch(const std::exception& f){errorCode="INTERNAL_ERROR";error=QString::fromUtf8(f.what());}
        if(output.failed()){
            normal=false;
            if(errorCode.isEmpty()){errorCode=output.failureCode();error="Reliable control transport failed; received raw prefix was stopped and tail publication was attempted";}
        }
        QJsonObject done;
        {std::lock_guard<std::mutex> guard(mutex);job.errorCode=errorCode;job.errorMessage=error;
            job.state=normal?(job.plan.total&&job.received==job.plan.total?"completed":"stopped"):"failed";
            done=snapshotLocked();done["event"]="session_done";done["session_complete"]=normal&&job.durable==job.received;done["target_complete"]=normal&&job.plan.total&&job.received==job.plan.total;}
        output.clearPreview();statusEvent();output.send(done);
    }
public:
    Service(Options o,StreamOutput& writer):options(std::move(o)),output(writer){}
    ~Service(){finish();}
    void finish(){stop=true;if(worker.joinable())worker.join();}
    QString saveTransportFailure(){
        QJsonObject notice;QString directory,id;
        {std::lock_guard<std::mutex> guard(mutex);if(!haveJob)return {};
            job.state="failed";
            if(job.errorCode.isEmpty()){job.errorCode=output.failureCode();job.errorMessage="Reliable control delivery is unconfirmed; inspect the retained SD prefix";}
            notice=snapshotLocked();directory=job.plan.directory;id=job.id;
            notice["schema"]="vibration.stream.transport-error.v1";notice["session_schema"]=schema;notice["session_complete"]=false;notice["target_complete"]=false;
            notice["all_received_records_saved"]=job.durable==job.received;notice["control_delivery_confirmed"]=false;notice["transport"]=output.diagnostics();}
#ifdef __linux__
        auto root=directoryFd(directory,false);const auto finalName=(id+".transport-error.json").toStdString();const auto tempName=("."+id+".transport-error.partial").toStdString();
        Fd file(::openat(root.fd,tempName.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
        if(file.fd<0)fail("FILE_ERROR",errnoText("Create exclusive transport failure notice"));
        try{
            const auto bytes=QJsonDocument(notice).toJson(QJsonDocument::Indented);qint64 offset=0;
            while(offset<bytes.size()){auto n=::write(file.fd,bytes.constData()+offset,std::size_t(bytes.size()-offset));if(n>0){offset+=n;continue;}if(n<0&&errno==EINTR)continue;fail("FILE_ERROR",errnoText("Write transport failure notice"));}
            syncFd(file.fd,"Sync transport failure notice");
#ifdef SYS_renameat2
            if(::syscall(SYS_renameat2,root.fd,tempName.c_str(),root.fd,finalName.c_str(),1u))fail("FILE_ERROR",errnoText("Publish exclusive transport failure notice"));
#else
            fail("UNSUPPORTED_FILESYSTEM","Transport notice requires atomic exclusive renameat2");
#endif
            syncFd(file.fd,"Sync renamed transport failure notice inode");
            syncFd(root.fd,"Sync transport failure notice directory");
        }catch(...){(void)::unlinkat(root.fd,tempName.c_str(),0);throw;}
        return QDir(directory).filePath(QString::fromStdString(finalName));
#else
        return {};
#endif
    }
    QJsonObject capabilities()const {
        bool available=options.fixture;QString reason=options.fixture?"Explicit synthetic host fixture":"Stream device unavailable";
#ifdef __linux__
        if(!options.fixture){Fd fd(::open("/dev/ax7z020-capture",O_RDONLY|O_NONBLOCK|O_CLOEXEC));ax7z020_stream_caps caps{};
            available=fd.fd>=0&&!::ioctl(fd.fd,AX7Z020_STREAM_GET_CAPS,&caps)&&caps.abi_version==1&&caps.clock_hz==clockHz&&caps.record_bytes==32&&caps.chunk_period_us==40000&&(caps.features&7u)==7u;
            if(available)reason="Verified streaming driver ABI available";}
#else
        available=false;reason="Linux/POSIX stream storage backend required";
#endif
        QJsonArray r;for(auto rate:rates)r.append(rate);
        return {{"available",available},{"backend",options.fixture?"test_fixture":"linux_ring"},{"reason",reason},{"synthetic",options.synthetic()},{"hardware_confirmed",available&&!options.fixture},{"source",options.source()},{"supported_rates_hz",r},{"modes",QJsonArray{"timed","continuous"}},{"max_duration_seconds",86400},{"default_segment_seconds",10},{"max_segment_seconds",40},{"clock_hz",int(clockHz)},{"adc_range_uv",5000000},{"channel_map",0x50301},{"chunk_period_ms",40},{"preview_max_records",2000},{"reserve_bytes",number(options.reserve)},{"format","CAPSTRM1"}};
    }
    bool handle(const QByteArray& line) {
        QString id;QJsonObject response{{"schema",schema},{"type","response"},{"source",options.source()},{"emitter","stream_service"}};bool shutdown=false;
        try {
            QJsonParseError parseError;const auto document=QJsonDocument::fromJson(line,&parseError);if(parseError.error!=QJsonParseError::NoError||!document.isObject())fail("INVALID_REQUEST","Invalid JSON object");UniqueKeys(line).check();const auto o=document.object();
            if((o.contains("request_id")&&!o.value("request_id").isString())||(o.contains("id")&&!o.value("id").isString()))fail("INVALID_REQUEST","Request identifiers must be strings");
            id=o.value("request_id").toString();if(id.isEmpty())id=o.value("id").toString();if(id.isEmpty()||id.size()>128)fail("INVALID_REQUEST","request_id must contain 1..128 characters");
            if(o.contains("id")&&o.value("id").toString()!=id)fail("INVALID_REQUEST","id and request_id conflict");
            keys(o,{"schema","request_id","id","command","plan"});if(o.value("schema").toString()!=schema)fail("INVALID_REQUEST","Unsupported schema");const auto command=o.value("command").toString();
            if(command!="start"&&o.contains("plan"))fail("INVALID_REQUEST","plan is only valid with start");
            if(command=="capabilities")response["capabilities"]=capabilities();
            else if(command=="status") {std::lock_guard<std::mutex> guard(mutex);response["job"]=haveJob?QJsonValue(snapshotLocked()):QJsonValue();}
            else if(command=="start") {
                {std::lock_guard<std::mutex> guard(mutex);if(haveJob&&!terminal(job.state))fail("BUSY","A stream session is active");}
                auto plan=parsePlan(o.value("plan"),options);if(worker.joinable())worker.join();
                {std::lock_guard<std::mutex> guard(mutex);job=Job{};auto uuid=QUuid::createUuid();job.id=uuid.toString(QUuid::Id128);job.uuid=uuid.toRfc4122();job.requestId=id;job.plan=std::move(plan);haveJob=true;stop=false;response["job"]=snapshotLocked();}
                response["ok"]=true;response["request_id"]=id;response["id"]=id;output.send(response);worker=std::thread([this]{run();});return true;
            } else if(command=="stop") {
                stop=true;std::lock_guard<std::mutex> guard(mutex);response["job"]=haveJob?QJsonValue(snapshotLocked()):QJsonValue();
            } else if(command=="shutdown") {finish();shutdown=true;response["shutdown"]=true;}
            else fail("INVALID_REQUEST","Unknown command");
            response["ok"]=true;
        }catch(const Failure& f){response["ok"]=false;response["error_code"]=f.code;response["error_message"]=QString::fromUtf8(f.what());}
        catch(const std::exception& e){response["ok"]=false;response["error_code"]="INTERNAL_ERROR";response["error_message"]=QString::fromUtf8(e.what());}
        response["request_id"]=id;response["id"]=id;output.send(response);return !shutdown;
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);Options options;
    for(int i=1;i<argc;++i){const QString a=QString::fromLocal8Bit(argv[i]);
        if(a=="--test-fixture")options.fixture=true;
        else if(a=="--hardware-test")options.hardwareTest=true;
        else if(a.startsWith("--fixture-fault="))options.fault=a.mid(16);
        else if(a.startsWith("--fixture-chunk-ms=")){bool ok=false;const auto n=a.mid(19).toInt(&ok);if(!ok||n<0||n>1000){std::cerr<<"Invalid fixture chunk delay\n";return 2;}options.fixtureChunkMs=n;}
        else if(a.startsWith("--reserve-bytes=")){bool ok=false;auto n=a.mid(16).toULongLong(&ok);if(!ok||n>1024ULL*1024*1024*1024){std::cerr<<"Invalid reserve bytes\n";return 2;}options.reserve=n;}
        else{std::cerr<<"Unknown argument\n";return 2;}}
    if(options.fixture&&options.hardwareTest){std::cerr<<"--test-fixture and --hardware-test are mutually exclusive\n";return 2;}
    if(!options.fixture&&(!options.fault.isEmpty()||options.fixtureChunkMs!=40)){std::cerr<<"Fixture options require --test-fixture\n";return 2;}
    if(!options.fault.isEmpty()&&options.fault!="low_space"&&options.fault!="short_write"&&options.fault!="storage_after_first"&&options.fault!="device_gap"){std::cerr<<"Unknown fixture fault\n";return 2;}
    StreamOutput output;Service service(options,output);QByteArray line;bool oversized=false;
    const auto consume=[&](char c){
        if(c=='\n'){if(oversized){service.handle("{}");}else if(!line.trimmed().isEmpty()&&!service.handle(line))return false;line.clear();oversized=false;}
        else if(line.size()<8192&&!oversized)line.append(c);else{line.clear();oversized=true;}
        return true;
    };
#ifdef __linux__
    // Polling stdin also observes an output failure when the controller sends
    // no more commands. Do not leave a failed child waiting forever in getc().
    bool accepting=true;std::array<char,4096> input{};
    while(accepting&&!output.failed()){
        pollfd p{STDIN_FILENO,POLLIN|POLLHUP|POLLERR,0};const auto ready=::poll(&p,1,100);
        if(ready<0){if(errno==EINTR)continue;break;}if(!ready)continue;
        const auto n=::read(STDIN_FILENO,input.data(),input.size());
        if(n<0){if(errno==EINTR||errno==EAGAIN)continue;break;}if(!n)break;
        for(ssize_t i=0;i<n&&accepting&&!output.failed();++i)accepting=consume(input[std::size_t(i)]);
    }
#else
    char c;while(std::cin.get(c))if(!consume(c))break;
#endif
    // EOF is a graceful boundary stop too, so disconnects preserve the tail.
    service.finish();output.shutdown();
    if(output.failed()){
        QString notice;
        try{notice=service.saveTransportFailure();}
        catch(const std::exception& e){std::cerr<<"Transport failure notice could not be saved: "<<e.what()<<'\n';}
        std::cerr<<"Stream output failure "<<output.failureCode().toStdString()<<"; reliable control delivery is unconfirmed; retained SD data is not a successful complete-session report";
        if(!notice.isEmpty())std::cerr<<"; failure_notice="<<notice.toStdString();
        std::cerr<<'\n';
        return 2;
    }
    return 0;
}
