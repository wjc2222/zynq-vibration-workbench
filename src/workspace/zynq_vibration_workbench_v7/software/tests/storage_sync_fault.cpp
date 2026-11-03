// Host-only LD_PRELOAD fixture: observe and fail syncs inside one explicit
// temporary output directory. Never linked into a production executable.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int fsync(int fd) {
    static const auto realSync=reinterpret_cast<int(*)(int)>(::dlsym(RTLD_NEXT,"fsync"));
    const auto* root=std::getenv("AX7_QA_SYNC_ROOT");
    if(!root||!*root)return realSync(fd);
    char descriptor[64],target[4096];std::snprintf(descriptor,sizeof(descriptor),"/proc/self/fd/%d",fd);
    const auto n=::readlink(descriptor,target,sizeof(target)-1);
    if(n<0)return realSync(fd);target[n]='\0';
    const std::string path(target),directory(root);std::string phase;
    if(path==directory)phase="parent";
    else if(path.rfind(directory+"/",0)==0){
        const auto relative=path.substr(directory.size()+1);
        if(relative.find('/')==std::string::npos&&relative.front()!='.'){
            struct stat status{};
            if(::fstat(fd,&status)==0&&S_ISDIR(status.st_mode))phase="renamed_directory";
            else if(relative.rfind("CAP_",0)==0&&relative.size()>=10&&relative.compare(relative.size()-10,10,".meta.json")==0)phase="renamed_metadata";
            else if(relative.rfind("CAP_",0)==0&&relative.size()>=4&&relative.compare(relative.size()-4,4,".BIN")==0)phase="renamed_bin";
        }
    }
    if(!phase.empty()){
        if(const auto* trace=std::getenv("AX7_QA_SYNC_TRACE")){
            const auto log=::open(trace,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
            if(log>=0){const auto line=phase+"\n";const auto written=::write(log,line.data(),line.size());(void)written;::close(log);}
        }
        const auto* fault=std::getenv("AX7_QA_SYNC_FAIL");
        if(fault&&phase==fault){
            const auto* error=std::getenv("AX7_QA_SYNC_ERRNO");errno=error?std::atoi(error):EIO;return -1;
        }
    }
    return realSync(fd);
}
