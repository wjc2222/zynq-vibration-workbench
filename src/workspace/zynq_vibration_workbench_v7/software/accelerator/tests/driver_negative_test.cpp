// Board-only analysis-driver diagnostic. No capture/display/SD operations.
// Intentionally exercises invalid ABI calls, exclusive ownership, bounded
// timeout/reset recovery and one zero-input computation. Not a CTest auto-test.
#include "ax7z020_accel.h"
#include "ax7z020_accel_regs.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
static_assert(sizeof(ax7z020_accel_result)==197272,"UAPI layout");
struct Fd {int v=-1;explicit Fd(int n):v(n){}~Fd(){if(v>=0)::close(v);}};
unsigned checks=0;
void check(bool ok,const char*name){if(!ok)throw std::runtime_error(std::string(name)+": errno="+std::to_string(errno)+" "+std::strerror(errno));++checks;std::printf("CHECK_PASS %s\n",name);}
template<class F>void fails(F f,int code,const char*name){errno=0;const auto r=f();const int got=errno;check(r==-1&&got==code,name);}
void write_all(int fd,const std::vector<unsigned char>& b){std::size_t n=0;while(n<b.size()){auto k=::write(fd,b.data()+n,b.size()-n);if(k<0&&errno==EINTR)continue;check(k>0&&k%6==0,"aligned input transfer");n+=std::size_t(k);}}
ax7z020_accel_plan plan(unsigned fs,unsigned timeout=10000){ax7z020_accel_plan p{};p.abi_version=1;p.sample_rate_hz=fs;p.raw_frames=(fs==50000?641:65)+8191*(fs/5000);p.timeout_ms=timeout;return p;}
int main(int argc,char**argv){
    if(argc>1&&std::string(argv[1])=="--help"){std::puts("vibration-accel-driver-test [DEVICE]; BOARD-ONLY: invalid ABI, concurrent exclusive open, 50k 10ms timeout, reset, 5k zeros. Does not touch capture/SD.");return 0;}
    const char*path=argc>1?argv[1]:"/dev/vibration-accel";
    Fd fd(::open(path,O_RDWR|O_CLOEXEC));if(fd.v<0){std::fprintf(stderr,"hardware_unavailable_or_busy: %s\n",std::strerror(errno));return 3;}
    try{
        ax7z020_accel_caps caps{};check(::ioctl(fd.v,AX7Z020_ACCEL_GET_CAPS,&caps)==0,"get caps");
        check(caps.abi_version==1&&caps.hardware_id==AX7Z020_ACCEL_HW_ID&&caps.revision==ACC_EXPECTED_REVISION,"ID revision ABI match");
        check(::ioctl(fd.v,AX7Z020_ACCEL_RESET)==0,"initial analysis-only reset");
        unsigned char byte=0;
        fails([&]{return ::read(fd.v,&byte,1);},ENODATA,"no stale result before RUN");
        fails([&]{return ::write(fd.v,&byte,1);},EINVAL,"reject unaligned input bytes");
        fails([&]{return ::ioctl(fd.v,0x12345678UL,0);},ENOTTY,"reject unknown ioctl");
        fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_GET_CAPS,static_cast<void*>(nullptr));},EFAULT,"reject inaccessible caps pointer");
        fails([&]{return ::lseek(fd.v,0,SEEK_SET);},ESPIPE,"no arbitrary seek");
        errno=0;void*map=::mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd.v,0);
        check(map==MAP_FAILED&&(errno==ENODEV||errno==EINVAL),"physical mmap unavailable");
        auto p=plan(5000);p.abi_version=2;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"reject ABI mismatch");
        p=plan(5000);p.reserved[1]=1;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"reject nonzero reserved");
        p=plan(5000);p.flags=2;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"reject unknown flags");
        p=plan(5000);p.timeout_ms=9;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"timeout lower bound");
        p=plan(5000);p.timeout_ms=30001;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"timeout upper bound");
        p=plan(5000);p.sample_rate_hz=44100;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"reject unsupported Fs");
        p=plan(5000);--p.raw_frames;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"reject incorrect FIR support");
        p=plan(5000);fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EMSGSIZE,"reject absent raw payload");
        p=plan(5000);p.flags=1;for(unsigned i=0;i<13;++i)p.bands[i]={10+i*5,12+i*5};p.bands[1]={11,14};
        fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"reject overlapping bands");
        p=plan(5000);p.flags=1;p.bands[0]={0,3};
        fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"reject band outside reference");
        std::vector<unsigned char> oversized((AX7Z020_ACCEL_MAX_RAW_FRAMES+1u)*6u);
        fails([&]{return ::write(fd.v,oversized.data(),oversized.size());},EINVAL,"reject oversized buffer");
        p=plan(50000,10);std::vector<unsigned char> zeros(p.raw_frames*6u);write_all(fd.v,zeros);
        std::atomic<int> concurrent_errno{0};
        std::thread competitor([&]{std::this_thread::sleep_for(std::chrono::milliseconds(2));errno=0;Fd other(::open(path,O_RDWR|O_CLOEXEC));concurrent_errno=(other.v<0?errno:-1);});
        // 8192*641 FIR cycles alone exceed 10ms at the frozen 100MHz core.
        errno=0;const int run_result=::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);const int run_errno=errno;
        competitor.join();check(run_result==-1&&run_errno==ETIMEDOUT,"bounded long-window timeout");
        check(concurrent_errno==EBUSY,"second opener rejected while owner lives");
        fails([&]{return ::read(fd.v,&byte,1);},ENODATA,"timeout exposes no prior result");
        check(::ioctl(fd.v,AX7Z020_ACCEL_RESET)==0,"analysis reset after timeout");
        ax7z020_accel_status status{};check(::ioctl(fd.v,AX7Z020_ACCEL_GET_STATUS,&status)==0,"get recovery status");
        check((status.status&ACC_STATUS_IDLE)&&!status.errors&&status.raw_frames==0&&status.lowband_frames==0,"reset clears counters/errors");
        p=plan(5000);zeros.assign(p.raw_frames*6u,0);write_all(fd.v,zeros);check(::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p)==0,"valid computation after timeout");
        auto result=std::unique_ptr<ax7z020_accel_result>(new ax7z020_accel_result{});std::size_t n=0;
        while(n<sizeof(*result)){auto k=::read(fd.v,reinterpret_cast<char*>(result.get())+n,sizeof(*result)-n);if(k<0&&errno==EINTR)continue;check(k>0,"bounded result transfer");n+=std::size_t(k);}
        check(result->error_code==0&&result->saturation_count==0&&result->points==8192,"clean recovery identity");
        for(const auto& c:result->channel){
            check(c.sum_q7==0&&c.sumsq_q14==0&&c.minimum_q7==0&&c.maximum_q7==0&&c.raw_endpoint_count==0&&c.saturation_count==0,"zero recovery statistics");
            bool allzero=true;for(auto v:c.lowband_q7)allzero=allzero&&v==0;for(auto v:c.fft_magnitude_squared)allzero=allzero&&v==0;for(auto v:c.band_magnitude_squared)allzero=allzero&&v==0;
            check(allzero,"zero recovery samples FFT powers and bands");
        }
        p.abi_version=0;fails([&]{return ::ioctl(fd.v,AX7Z020_ACCEL_RUN,&p);},EINVAL,"invalid RUN clears prior valid result");
        fails([&]{return ::read(fd.v,&byte,1);},ENODATA,"no stale successful payload after invalid RUN");
        std::printf("DRIVER_NEGATIVE_PASS checks=%u scope=analysis_driver_only capture_sd_untouched\n",checks);return 0;
    }catch(const std::exception&e){std::fprintf(stderr,"DRIVER_NEGATIVE_FAIL after=%u: %s\n",checks,e.what());return 1;}
}
