// RAM diagnostic: full fixed-width analysis UAPI, synthetic inputs only.
// No capture/stream/framebuffer/SD resource and no implicit driver fallback.
#include "ax7z020_accel.h"
#include "ax7z020_accel_regs.h"
#include "publisher-sha256.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

struct Header { char magic[8];unsigned version,fs,kind,result_bytes;char raw_sha256[64]; };
struct Dump { Header header;ax7z020_accel_result result; };
static_assert(sizeof(Header)==88&&sizeof(ax7z020_accel_result)==197272&&sizeof(Dump)==197360,"fixed ABI layout");
struct Fd {int n;explicit Fd(int x):n(x){}~Fd(){if(n>=0)::close(n);}};
static void need(bool ok,const std::string& s){if(!ok)throw std::runtime_error(s);}
static std::string hash(const void* p,std::size_t n){rootfs_update::Sha256 h;h.update(static_cast<const unsigned char*>(p),n);return h.finish();}
static void ramPath(const std::string& p){need(p.rfind("/tmp/",0)==0&&p.find("/../")==std::string::npos&&p.find("/./")==std::string::npos&&p.substr(p.size()-1)!="/","explicit ordinary /tmp file required");}
static void transfer(int fd,void* p,std::size_t bytes,bool writing){
    auto* out=static_cast<unsigned char*>(p);std::size_t pos=0;
    while(pos<bytes){auto n=writing ? ::write(fd,out+pos,bytes-pos) : ::read(fd,out+pos,bytes-pos);if(n<0&&errno==EINTR)continue;need(n>0,"complete file/device transfer failed");pos+=std::size_t(n);}
}
static void validateResult(const ax7z020_accel_result& r,const std::vector<short>& raw,unsigned fs,unsigned taps,const ax7z020_accel_plan& plan){
    need(r.abi_version==1&&r.hardware_id==AX7Z020_ACCEL_HW_ID&&r.revision==ACC_EXPECTED_REVISION&&r.sample_rate_hz==fs&&r.points==8192&&r.fir_taps==taps&&r.decimation==fs/5000&&r.flags==plan.flags&&!r.error_code,"result identity/profile/error mismatch");
    unsigned saturation=0;
    for(unsigned axis=0;axis<3;++axis){
        const auto& c=r.channel[axis];long long sum=0;unsigned long long squares=0,clips=0;
        int minimum=8388607,maximum=-8388608;
        for(unsigned n=0;n<8192;++n){const int v=c.lowband_q7[n];need(v>=-8388608&&v<=8388607,"noncanonical signed24 Q7 sample");sum+=v;squares+=static_cast<unsigned long long>(1LL*v*v);minimum=std::min(minimum,v);maximum=std::max(maximum,v);}
        for(std::size_t n=axis;n<raw.size();n+=3)if(raw[n]==-32768||raw[n]==32767)++clips;
        need(c.sum_q7==sum&&c.sumsq_q14==squares&&c.minimum_q7==minimum&&c.maximum_q7==maximum&&c.raw_endpoint_count==clips,"full lowband/statistics/endpoint consistency mismatch");
        need(c.fft_exponent<=31&&c.fft_pre_shift==1,"FFT scale mismatch");saturation+=unsigned(c.saturation_count);
        std::array<unsigned long long,18> bands{};
        for(unsigned bin=0;bin<4097;++bin){
            const auto p=c.fft_magnitude_squared[bin];need(p<=(1ULL<<48),"invalid FFT magnitude range");
            const auto w=p*((bin==0||bin==4096)?1:2);bool any=false,integer=false;
            for(unsigned b=0;b<13;++b)if(bin>=plan.bands[b].first&&bin<=plan.bands[b].last){bands[b]+=w;any=true;if(b>=6)integer=true;}
            if(bin<=3276)bands[14]+=w;
            if(bin>=4&&bin<=819){bands[13]+=w;if(any)bands[15]+=w;else bands[17]+=w;if(integer)bands[16]+=w;}
        }
        for(unsigned b=0;b<18;++b)need(bands[b]==c.band_magnitude_squared[b],"full FFT-power/band consistency mismatch");
    }
    need(saturation==r.saturation_count,"saturation counter mismatch");
}
static std::string canonical(Dump& dump){const auto cycles=dump.result.cycles;dump.result.cycles=0;auto value=hash(&dump,sizeof(dump));dump.result.cycles=cycles;return value;}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--self-test"){
        need(hash("abc",3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA self-test");
        auto a=std::make_unique<Dump>(),b=std::make_unique<Dump>();a->result.cycles=5;b->result.cycles=7;need(canonical(*a)==canonical(*b),"cycles normalization");b->result.channel[2].lowband_q7[8191]=-1;need(canonical(*a)!=canonical(*b),"full payload mismatch detection");
        std::puts("{\"status\":\"self_test_passed\",\"scope\":\"host_memory_only_no_device\",\"wire_bytes\":197272,\"dump_bytes\":197360}");return 0;
    }
    unsigned fs=0,kind=0;std::string name,out,baseline;
    for(int i=1;i<argc;++i){need(i+1<argc,"explicit --fs/--case/--out [--baseline] values required");std::string key=argv[i],value=argv[++i];if(key=="--fs")fs=unsigned(std::stoul(value));else if(key=="--case")name=value;else if(key=="--out")out=value;else if(key=="--baseline")baseline=value;else need(false,"unknown option");}
    const unsigned taps=fs==50000?641:fs==25000?321:fs==10000?129:fs==5000?65:0;
    need(taps&&!out.empty(),"supported rate and new output required");ramPath(out);if(!baseline.empty())ramPath(baseline);
    kind=name=="normal"?1:name=="fullscale"?2:name=="tiny"?3:name=="random"?4:0;need(kind,"unknown synthetic case");
    const unsigned frames=taps+8191*(fs/5000);std::vector<short> raw(frames*3);unsigned seed=0x128f0ab3u^fs;
    for(unsigned n=0;n<frames;++n)for(unsigned axis=0;axis<3;++axis){int v=0;if(kind==1)v=int(std::lrint(100+10000*(1.-.2*axis)*std::sin(2*std::acos(-1.)*25*n/fs+axis*.2)));else if(kind==2)v=axis==1?-32768:32767;else if(kind==3)v=int((n+axis*2)%5)-2;else{seed=seed*1664525u+1013904223u;v=int(seed>>16)-32768;}raw[n*3+axis]=short(v);}
    auto dump=std::make_unique<Dump>();std::memcpy(dump->header.magic,"AX7MMIO1",8);dump->header.version=1;dump->header.fs=fs;dump->header.kind=kind;dump->header.result_bytes=sizeof(dump->result);
    const auto rawHash=hash(raw.data(),raw.size()*sizeof(short));std::memcpy(dump->header.raw_sha256,rawHash.data(),64);
    Fd dev(::open("/dev/vibration-accel",O_RDWR|O_CLOEXEC|O_NOFOLLOW));need(dev.n>=0,"open dedicated analysis device failed");
    ax7z020_accel_caps caps{};need(::ioctl(dev.n,AX7Z020_ACCEL_GET_CAPS,&caps)==0&&caps.abi_version==1&&caps.hardware_id==AX7Z020_ACCEL_HW_ID&&caps.revision==ACC_EXPECTED_REVISION&&caps.clock_hz==100000000,"analysis caps mismatch");
    need(::ioctl(dev.n,AX7Z020_ACCEL_RESET)==0,"analysis reset failed");transfer(dev.n,raw.data(),raw.size()*sizeof(short),true);
    ax7z020_accel_plan plan{};plan.abi_version=1;plan.sample_rate_hz=fs;plan.raw_frames=frames;plan.timeout_ms=10000;plan.flags=AX7Z020_ACCEL_PLAN_BANDS_VALID;
    const double orders[]={.5,1,1.5,2,2.5,3,4,5,6,7,8,9,10};
    for(unsigned b=0;b<13;++b){const auto center=orders[b]*25*8192/5000;plan.bands[b].first=unsigned(std::max(4.,std::ceil(center-1.5)));plan.bands[b].last=unsigned(std::floor(center+1.5));}
    need(::ioctl(dev.n,AX7Z020_ACCEL_RUN,&plan)==0,"analysis RUN failed");transfer(dev.n,&dump->result,sizeof(dump->result),false);
    validateResult(dump->result,raw,fs,taps,plan);
    const auto canonicalHash=canonical(*dump);bool equal=true;std::size_t differences=0,first=sizeof(Dump);
    if(!baseline.empty()){
        auto old=std::make_unique<Dump>();Fd input(::open(baseline.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));struct stat st{};need(input.n>=0&&!::fstat(input.n,&st)&&S_ISREG(st.st_mode)&&st.st_size==off_t(sizeof(Dump)),"baseline dump size/type mismatch");transfer(input.n,old.get(),sizeof(Dump),false);
        need(!std::memcmp(&old->header,&dump->header,sizeof(Header)),"baseline case/rate/raw-input identity differs");
        const auto currentCycles=dump->result.cycles;old->result.cycles=0;dump->result.cycles=0;
        auto* a=reinterpret_cast<unsigned char*>(old.get());auto* b=reinterpret_cast<unsigned char*>(dump.get());
        for(std::size_t i=0;i<sizeof(Dump);++i)if(a[i]!=b[i]){if(!differences)first=i;++differences;}
        dump->result.cycles=currentCycles;equal=!differences;
    }
    Fd output(::open(out.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));need(output.n>=0,"new RAM output creation failed");transfer(output.n,dump.get(),sizeof(Dump),true);need(!::fsync(output.n),"RAM output fsync failed");
    std::printf("{\"schema\":\"ax7z020.v7.complete-wire-probe.v1\",\"status\":\"%s\",\"fs\":%u,\"case\":\"%s\",\"raw_sha256\":\"%s\",\"dump_sha256\":\"%s\",\"canonical_sha256\":\"%s\",\"baseline_compared\":%s,\"equal_excluding_cycles\":%s,\"different_bytes\":%zu,\"first_difference\":%zu,\"pl_cycles\":%llu,\"lowband_words\":24576,\"fft_power_words\":12291,\"band_sums\":54,\"full_stat_consistency\":true,\"scope\":\"synthetic_dedicated_analysis_RAM_only\"}\n",equal?(baseline.empty()?"captured":"matched"):"mismatch",fs,name.c_str(),rawHash.c_str(),hash(dump.get(),sizeof(Dump)).c_str(),canonicalHash.c_str(),baseline.empty()?"false":"true",equal?"true":"false",differences,first,static_cast<unsigned long long>(dump->result.cycles));
    return equal?0:3;
}catch(const std::exception& e){std::fprintf(stderr,"{\"status\":\"failed\",\"message\":\"%s\",\"errno\":%d}\n",e.what(),errno);return 1;}}
