// mathematical_fixture_only: execute the frozen production model loader and
// inference with precomputed QA feature vectors. This is not a raw-input
// extraction path, a production synthetic-gate bypass, or a machine test.
#include "vibration/candidate_model.hpp"
#include "vibration/sha256.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool okay,const std::string&message){if(!okay)throw std::runtime_error(message);}
bool identifier(const std::string&s){return !s.empty()&&s.size()<=96&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';});}
}
int main(int argc,char**argv){
    try{
        require(argc==3,"usage: interop_probe candidate.model vectors.txt");
        const auto model=vibration::load_candidate_model(argv[1]);
        require(model.kind=="linear_softmax","QA requires a genuinely trained linear model");
        const auto nonzero=std::count_if(model.weights.begin(),model.weights.end(),[](double v){return v!=0;});
        require(nonzero==144,"QA requires all 144 loaded weights to be nonzero");
        std::ifstream modelFile(std::filesystem::u8path(argv[1]),std::ios::binary);
        const std::string modelBytes((std::istreambuf_iterator<char>(modelFile)),std::istreambuf_iterator<char>());
        const auto modelHash=vibration::sha256_hex(reinterpret_cast<const std::uint8_t*>(modelBytes.data()),modelBytes.size());
        std::ifstream input(std::filesystem::u8path(argv[2]));require(bool(input),"cannot open QA vectors");
        std::string line;unsigned count=0;
        while(std::getline(input,line)){
            require(line.size()<=16384,"oversized vector line");if(line.empty()||line[0]=='#')continue;
            require(++count<=1000,"too many QA vectors");std::istringstream row(line);row.imbue(std::locale::classic());
            std::string id,mode;vibration::CandidateWindow w;row>>id>>mode>>w.rpm_min>>w.rpm_max;
            require(bool(row)&&identifier(id),"invalid QA row identity/header");
            for(auto&v:w.features){row>>v;require(bool(row)&&std::isfinite(v)&&std::abs(v)<1e100,"invalid QA feature");}
            std::string extra;require(!(row>>extra),"unexpected extra vector fields");
            require(std::isfinite(w.rpm_min)&&std::isfinite(w.rpm_max)&&w.rpm_min>0&&w.rpm_min<=w.rpm_max,"invalid QA RPM bounds");
            w.setup_id=model.setup_id;w.configuration=model.configuration;w.sample_rate_hz=model.fs;
            w.rpm=(w.rpm_min+w.rpm_max)/2;w.fresh_periods=32;w.valid=true;
            w.source="mathematical_fixture_only";w.input_kind="qa_precomputed_feature_vector";w.source_path=argv[2];
            // The QA harness constructs a feature object directly, as unit
            // tests do. It never represents these vectors as machine records.
            if(mode=="quality_invalid"){w.valid=false;w.reasons={"mathematical_fixture_quality_rejection"};}
            else if(mode=="setup_mismatch")w.setup_id+="_mismatch";
            else if(mode=="configuration_mismatch")w.configuration+="00";
            else if(mode=="fs_mismatch")w.sample_rate_hz=model.fs==50000?5000:50000;
            else require(mode=="valid","unsupported QA mode");
            const auto result=vibration::infer_candidate(w,model);
            std::cout<<"{\"scope\":\"mathematical_fixture_only\",\"case_id\":\""<<id
              <<"\",\"model_sha256\":\""<<modelHash<<"\",\"loaded_nonzero_weights\":"<<nonzero
              <<",\"inference\":"<<vibration::candidate_inference_json(result)<<"}\n";
        }
        require(count>0,"no QA vectors");return 0;
    }catch(const std::exception&e){std::cerr<<"mathematical_fixture_only: "<<e.what()<<'\n';return 1;}
}
