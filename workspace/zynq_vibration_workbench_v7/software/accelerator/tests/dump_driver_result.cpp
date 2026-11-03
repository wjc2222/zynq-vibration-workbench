// Diagnostic only. Runs one raw window through the existing bounded driver and
// saves the fixed-width UAPI result verbatim for host/vendor-model comparison.
// No SD paths, capture controls, /dev/mem, GUI or production fallback.
#include "ax7z020_accel.h"
#include "ax7z020_accel_regs.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
static_assert(sizeof(ax7z020_accel_result)==197272,"UAPI layout mismatch");
struct Fd { int x=-1; explicit Fd(int v):x(v){} ~Fd(){if(x>=0)::close(x);} };
void need(bool b,const std::string&s){if(!b)throw std::runtime_error(s+": "+std::strerror(errno));}
int main(int argc,char**argv){try{
    if(argc!=4){std::fprintf(stderr,"usage: dump-driver-result RAW.s16le OUTPUT.bin FS\n");return 2;}
    char*end=nullptr;const auto fs=std::strtoul(argv[3],&end,10);
    unsigned taps=fs==50000?641:fs==25000?321:fs==10000?129:fs==5000?65:0;
    need(end&&!*end&&taps,"supported integer sample rate required");
    const auto bytes=(taps+8191*(fs/5000))*6;
    Fd input(::open(argv[1],O_RDONLY|O_CLOEXEC));need(input.x>=0,"open raw");
    struct stat info{};need(::fstat(input.x,&info)==0&&S_ISREG(info.st_mode)&&info.st_size==off_t(bytes),"raw length");
    struct stat existing{};need(::lstat(argv[2],&existing)<0&&errno==ENOENT,"output must be new");
    std::vector<unsigned char> raw(bytes);std::size_t offset=0;
    while(offset<bytes){auto n=::read(input.x,raw.data()+offset,bytes-offset);if(n<0&&errno==EINTR)continue;need(n>0,"read raw");offset+=n;}
    Fd device(::open("/dev/vibration-accel",O_RDWR|O_CLOEXEC));need(device.x>=0,"open accelerator");
    ax7z020_accel_caps caps{};need(::ioctl(device.x,AX7Z020_ACCEL_GET_CAPS,&caps)==0,"get caps");
    need(caps.abi_version==1&&caps.hardware_id==AX7Z020_ACCEL_HW_ID&&caps.revision==ACC_EXPECTED_REVISION,"hardware identity");
    need(::ioctl(device.x,AX7Z020_ACCEL_RESET)==0,"reset");offset=0;
    while(offset<bytes){auto n=::write(device.x,raw.data()+offset,bytes-offset);if(n<0&&errno==EINTR)continue;need(n>0&&n%6==0,"input write");offset+=n;}
    ax7z020_accel_plan plan{};plan.abi_version=1;plan.sample_rate_hz=fs;plan.raw_frames=bytes/6;plan.timeout_ms=10000;
    need(::ioctl(device.x,AX7Z020_ACCEL_RUN,&plan)==0,"run PL");
    auto result=std::unique_ptr<ax7z020_accel_result>(new ax7z020_accel_result{});offset=0;
    while(offset<sizeof(*result)){auto n=::read(device.x,reinterpret_cast<char*>(result.get())+offset,sizeof(*result)-offset);if(n<0&&errno==EINTR)continue;need(n>0,"result read");offset+=n;}
    need(result->abi_version==1&&result->hardware_id==AX7Z020_ACCEL_HW_ID&&result->revision==ACC_EXPECTED_REVISION&&result->sample_rate_hz==fs&&result->points==8192&&result->error_code==0,"result identity/error");
    Fd output(::open(argv[2],O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600));need(output.x>=0,"create output");offset=0;
    while(offset<sizeof(*result)){auto n=::write(output.x,reinterpret_cast<char*>(result.get())+offset,sizeof(*result)-offset);if(n<0&&errno==EINTR)continue;need(n>0,"output write");offset+=n;}
    need(::fsync(output.x)==0,"fsync output");
    std::printf("result_bytes=%zu hardware_id=%08x revision=%08x fs=%lu exponents=%u,%u,%u pre_shift=%u,%u,%u\n",sizeof(*result),result->hardware_id,result->revision,fs,result->channel[0].fft_exponent,result->channel[1].fft_exponent,result->channel[2].fft_exponent,result->channel[0].fft_pre_shift,result->channel[1].fft_pre_shift,result->channel[2].fft_pre_shift);
    return 0;
}catch(const std::exception&e){std::fprintf(stderr,"diagnostic failed: %s\n",e.what());return 1;}}
