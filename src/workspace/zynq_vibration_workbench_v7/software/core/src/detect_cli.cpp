#include "vibration/candidate_model.hpp"
#include "vibration/detection.hpp"
#include "vibration/supervised_model.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <filesystem>

int main(int argc,char**argv){
    try{
        if(argc<3){std::cerr<<"vibration_detect <features|supervised-features|baseline|infer|supervised-infer|export-raw> --input data.BIN --output new.jsonl|normal.model|result.json|raw.s16le --setup rig_mount --confirm-mechanical [--confirm-normal] [--model model] [--backend reference|hardware]\n";return 2;}
        const std::string command=argv[1];std::map<std::string,std::string>args;for(int i=2;i<argc;++i){const std::string k=argv[i];if(k=="--confirm-mechanical"||k=="--confirm-normal"){args[k]="yes";continue;}if(i+1>=argc)throw vibration::AnalysisError("missing option value");if(!args.emplace(k,argv[++i]).second)throw vibration::AnalysisError("duplicate option");}
        for(const auto&kv:args)if(kv.first!="--input"&&kv.first!="--output"&&kv.first!="--setup"&&kv.first!="--model"&&kv.first!="--backend"&&kv.first!="--confirm-mechanical"&&kv.first!="--confirm-normal")throw vibration::AnalysisError("unknown option: "+kv.first);
        auto required=[&](const char*k){if(!args.count(k)||args[k].empty())throw vibration::AnalysisError(std::string("required ")+k);return args[k];};
        auto c=vibration::load_capture(required("--input"));auto setup=required("--setup");auto output=required("--output");vibration::AnalysisConfig cfg;cfg.amplitude_unit=vibration::AmplitudeUnit::Voltage;cfg.signal_kind=args.count("--confirm-mechanical")?vibration::SignalKind::VibrationConfirmed:vibration::SignalKind::Unknown;
        const auto backend=args.count("--backend")?args["--backend"]:"reference";if(backend!="reference"&&backend!="hardware")throw vibration::AnalysisError("backend must be reference or hardware");const auto mode=backend=="hardware"?vibration::accelerator::Backend::Hardware:vibration::accelerator::Backend::Reference;
        const auto inputPath=std::filesystem::u8path(c.source_path);auto sidecar=inputPath;sidecar.replace_extension(".meta.json");auto labels=inputPath;labels.replace_extension(".labels.json");std::vector<std::string> protectedPaths{c.source_path,sidecar.u8string(),labels.u8string()};if(args.count("--model"))protectedPaths.push_back(args["--model"]);vibration::validate_detection_outputs({output},protectedPaths);
        const auto n=vibration::accelerator::required_raw_frames(c.header.sample_rate_hz);if(c.records.size()<n)throw vibration::AnalysisError("record shorter than complete lowband window and FIR support");
        std::string text;
        if(command=="export-raw"){
            text.reserve(std::size_t(n)*6);for(unsigned i=0;i<n;++i)for(auto sample:c.records[i].raw){const auto u=std::uint16_t(sample);text+=char(u&255);text+=char(u>>8);}
            std::cerr<<"CRC_verified=true sample_rate_hz="<<c.header.sample_rate_hz<<" raw_frames="<<n<<" bytes="<<text.size()<<" first_index="<<c.records.front().index<<" last_index="<<c.records[n-1].index<<" source_synthetic="<<(c.source_metadata.synthetic?"true":"false")<<"; raw export assigns no mechanical or normal label\n";
        }else if(command=="supervised-features"){
            std::size_t count=0;for(std::size_t start=0;start+n<=c.records.size();start+=n){text+=vibration::supervised_window_json(vibration::extract_supervised_window(c,start,cfg,setup,mode))+"\n";++count;}
            std::cerr<<count<<" disjoint RPM-free feature windows; no model training or fault labels inferred\n";
        }else if(command=="features"||command=="baseline"){
            if(command=="baseline"&&!args.count("--confirm-normal"))throw vibration::AnalysisError("baseline requires actual normal-condition declaration --confirm-normal");
            std::vector<vibration::CandidateWindow> windows;for(std::size_t start=0;start+n<=c.records.size();start+=n){auto w=vibration::extract_candidate_window(c,start,cfg,setup,mode);if(command=="features")text+=vibration::candidate_window_json(w)+"\n";windows.push_back(std::move(w));}
            if(command=="baseline")text=vibration::candidate_model_text(vibration::fit_normal_baseline(windows));
            std::cerr<<windows.size()<<" disjoint windows processed; synthetic/invalid windows cannot establish a baseline\n";
        }else if(command=="infer"||command=="supervised-infer"){
            const auto path=std::filesystem::u8path(required("--model"));
            if(!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>vibration::candidate_model_max_bytes)throw vibration::AnalysisError("model is not a bounded regular file");
            std::ifstream file(path,std::ios::binary);if(!file)throw vibration::AnalysisError("cannot read model");
            const std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
            const bool supervised=command=="supervised-infer"||bytes.find("schema=zynq_vibration.supervised_model.v1")!=std::string::npos;
            if(supervised){const auto model=vibration::parse_supervised_model(bytes);const auto w=vibration::extract_supervised_window(c,c.records.size()-n,cfg,setup,mode);text="{\"window\":"+vibration::supervised_window_json(w)+",\"inference\":"+vibration::supervised_inference_json(vibration::infer_supervised(w,model))+"}\n";}
            else{const auto model=vibration::parse_candidate_model(bytes);const auto w=vibration::extract_candidate_window(c,c.records.size()-n,cfg,setup,mode);text="{\"window\":"+vibration::candidate_window_json(w)+",\"inference\":"+vibration::candidate_inference_json(vibration::infer_candidate(w,model))+"}\n";}
        }else throw vibration::AnalysisError("unknown command");
        vibration::write_detection_text(output,text,protectedPaths);std::cout<<output<<'\n';return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
