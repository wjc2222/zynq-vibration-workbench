#include "vibration/detection.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;
unsigned cases=0,skipped=0;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void put(const fs::path& path,const std::string& value){std::ofstream file(path,std::ios::binary);file<<value;check(bool(file),"fixture write failed");}
std::string read(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
void reject(const std::string& name,const std::string& output,const std::vector<std::string>& protectedPaths){
    bool blocked=false;try{vibration::validate_detection_outputs({output},protectedPaths);}catch(const vibration::AnalysisError&){blocked=true;}
    check(blocked,name+": preflight accepted a protected output");
    blocked=false;try{vibration::write_detection_text(output,"must not be written",protectedPaths);}catch(const vibration::AnalysisError&){blocked=true;}
    check(blocked,name+": writer accepted a protected output");++cases;
}
void rejectBatch(const std::string& name,const std::vector<std::string>& outputs,const std::vector<std::string>& protectedPaths){
    bool blocked=false;try{vibration::validate_detection_outputs(outputs,protectedPaths);}catch(const vibration::AnalysisError&){blocked=true;}
    check(blocked,name+": batch alias accepted");++cases;
}
void allow(const std::string& name,const std::string& output,const std::vector<std::string>& protectedPaths){
    vibration::validate_detection_outputs({output},protectedPaths);vibration::write_detection_text(output,"allowed QA result",protectedPaths);
    check(read(fs::u8path(output))=="allowed QA result",name+": ordinary output was not written");++cases;
}
struct RestoreCwd{fs::path original=fs::current_path();~RestoreCwd(){std::error_code error;fs::current_path(original,error);}};
}
int main(int argc,char** argv){
    try{
        check(argc==2,"Usage: path_protection_test NEW_WORKSPACE (test creates it and leaves QA files for caller cleanup)");
        const auto root=fs::absolute(fs::u8path(argv[1]));check(!fs::exists(root),"Refusing existing QA workspace");
        check(fs::create_directory(root),"Cannot create isolated QA workspace");RestoreCwd restore;fs::current_path(root);fs::create_directory("sub");
        put("capture.BIN","software path QA fixture, not a physical record");put("example.profile","software path QA profile sentinel");
        const auto capture=(root/"capture.BIN").u8string(),metadata=(root/"capture.meta.json").u8string(),profile=(root/"example.profile").u8string();
        const std::vector<std::string> absolute{capture,metadata,profile},relative{"capture.BIN","capture.meta.json","example.profile"};
        const auto captureBefore=read("capture.BIN"),profileBefore=read("example.profile");
        for(const auto& output:std::vector<std::string>{"capture.meta.json","./capture.meta.json","sub/../capture.meta.json",metadata,(root/"sub/../capture.meta.json").u8string()})
            reject("absolute protected, relative/absolute/dot missing sidecar",output,absolute);
        reject("relative protected, absolute missing sidecar",metadata,relative);
        reject("relative protected, dot missing sidecar","./capture.meta.json",relative);
        reject("mixed protected namespace",metadata,{capture,"capture.meta.json",profile});
        fs::create_hard_link("capture.BIN","capture-hard.BIN");reject("relative BIN hardlink","capture-hard.BIN",absolute);
        fs::create_hard_link("example.profile","profile-hard.json");reject("relative profile hardlink","profile-hard.json",absolute);
        std::error_code symlinkError;fs::create_symlink("capture.meta.json","missing-meta-link.json",symlinkError);
#if defined(_WIN32)
        if(symlinkError){skipped=6;}else{
#else
        check(!symlinkError,"Required Linux RAM symlink fixture unavailable");{
#endif
            reject("relative dangling output symlink","missing-meta-link.json",absolute);
            reject("absolute dangling output symlink",(root/"missing-meta-link.json").u8string(),absolute);
            fs::create_symlink("missing-meta-link.json","missing-meta-chain.json");reject("relative dangling symlink chain","missing-meta-chain.json",absolute);
            fs::create_directory_symlink(root,"directory-alias");reject("directory symlink with missing leaf","directory-alias/capture.meta.json",absolute);
            fs::create_symlink("capture.BIN","input-alias.BIN");
            const std::vector<std::string> aliasProtected{(root/"input-alias.BIN").u8string(),(root/"input-alias.meta.json").u8string(),profile};
            reject("relative input alias owns its missing sidecar","input-alias.meta.json",aliasProtected);
            reject("absolute input alias owns its missing sidecar",(root/"input-alias.meta.json").u8string(),aliasProtected);
        }
        reject("existing relative original BIN","capture.BIN",absolute);reject("existing dot profile","sub/../example.profile",absolute);
        rejectBatch("relative/absolute missing output alias",{"batch.json",(root/"batch.json").u8string()},absolute);
        rejectBatch("relative/dot missing output alias",{"batch.json","./batch.json"},absolute);
        // Unlike hardlinks, a dangling alias can point to a nonexistent output.
        if(!symlinkError){fs::create_symlink("batch.json","batch-link.json");rejectBatch("missing output dangling alias",{"batch.json","batch-link.json"},absolute);}
        else ++skipped;
        allow("new relative output","allowed-relative.json",absolute);allow("new absolute output",(root/"allowed-absolute.json").u8string(),relative);
        allow("dot output","sub/../allowed-dot.json",absolute);put("allowed-existing.json","previous QA output");allow("ordinary existing output","allowed-existing.json",absolute);
        vibration::validate_detection_outputs({"distinct-one.json",(root/"distinct-two.json").u8string()},absolute);++cases;
        check(!fs::exists("capture.meta.json")&&!fs::exists("input-alias.meta.json"),"A rejected output created reserved metadata");
        check(!fs::exists("batch.json"),"Batch preflight wrote an output");
        check(read("capture.BIN")==captureBefore&&read("example.profile")==profileBefore,"Protected source/profile bytes changed");
        std::cout<<"PASS: "<<cases<<" relative/absolute path-protection cases; "<<skipped<<" optional Windows alias cases skipped\n";
        std::cout<<"WORKSPACE="<<root.u8string()<<'\n';
    }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
