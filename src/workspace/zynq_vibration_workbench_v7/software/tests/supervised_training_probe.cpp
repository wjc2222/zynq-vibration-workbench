#include "vibration/supervised_model.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
using namespace vibration;
namespace {
void need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string decode(const std::string& value){if(value=="-")return {};need(value.size()%2==0&&value.find_first_not_of("0123456789abcdef")==std::string::npos,"bad matrix text encoding");std::string out;for(std::size_t i=0;i<value.size();i+=2)out+=char(std::stoul(value.substr(i,2),nullptr,16));return out;}
std::string quote(const std::string& value){std::ostringstream out;out<<'"';for(unsigned char ch:value){if(ch=='"'||ch=='\\')out<<'\\'<<char(ch);else if(ch<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(ch)<<std::dec;else out<<char(ch);}return out.str()+'"';}
void write(const std::string& path,const std::string& value){std::ofstream out(std::filesystem::u8path(path),std::ios::binary);need(bool(out),"cannot open experiment output");out<<value;need(bool(out),"experiment output write failed");}
}
int main(int argc,char** argv){try{
    need(argc==6,"usage: supervised_training_probe matrix.txt model.txt validation.json predictions.json rf42|linear42");
    std::set<std::filesystem::path> outputs;for(unsigned i=2;i<=4;++i){const auto path=std::filesystem::weakly_canonical(std::filesystem::u8path(argv[i]));need(!std::filesystem::exists(path),"refusing to overwrite an existing experiment output");need(outputs.insert(path).second,"experiment outputs must be distinct");}
    SupervisedTrainOptions options;if(std::string(argv[5])=="linear42")options.algorithm=SupervisedAlgorithm::LinearSoftmax;else need(std::string(argv[5])=="rf42","unknown fixed experiment protocol");
    std::ifstream in(std::filesystem::u8path(argv[1]));std::string line;std::getline(in,line);const bool completeAudit=line=="SUPERVISED_FEATURE_MATRIX_V2";need(completeAudit||line=="SUPERVISED_FEATURE_MATRIX_V1","bad matrix protocol");const char* labels[]{"normal","imbalance","misalignment","looseness"};std::map<std::string,SupervisedSample> sources;std::vector<std::pair<unsigned,SupervisedWindow>> queries;
    while(std::getline(in,line)){std::istringstream row(line);row.imbue(std::locale::classic());unsigned truth=4;SupervisedWindow window;row>>truth>>window.sample_rate_hz>>window.valid>>window.synthetic>>window.first_index>>window.last_index;need(bool(row)&&truth<=4,"bad matrix numerical metadata");auto text=[&](){std::string value;row>>value;need(bool(row),"missing matrix text metadata");return decode(value);};window.source_path=text();window.capture_sha256=text();window.source_session_id=text();window.raw_window_sha256=text();window.configuration=text();window.setup_id=text();window.source="actual_preextracted_features_matrix";for(double& value:window.features)row>>value;need(bool(row),"bad matrix feature count");if(completeAudit){for(auto* values:{&window.reasons,&window.warnings}){unsigned count=0;row>>count;need(bool(row)&&count<=128,"bad matrix audit field count");for(unsigned i=0;i<count;++i)values->push_back(text());}}std::string extra;need(!(row>>extra),"extra matrix field");queries.emplace_back(truth,window);
        if(truth<4){auto& source=sources[window.capture_sha256];if(source.path.empty()){source.path=window.source_path;source.capture_sha256=window.capture_sha256;source.session_id=window.source_session_id;source.label=labels[truth];source.operator_updated_at="operator_label_snapshot_after_source_identity_verification";}need(source.path==window.source_path&&source.label==labels[truth]&&source.session_id==window.source_session_id,"one matrix source has conflicting label/path/session");if(window.valid&&!window.synthetic&&window.reasons.empty())source.windows.push_back(window);else ++source.excluded_invalid_windows;}
    }
    std::vector<SupervisedSample> samples;for(auto& entry:sources)if(!entry.second.windows.empty())samples.push_back(std::move(entry.second));const auto model=fit_supervised_model(samples,"four_class_supervised",options);const auto validation=evaluate_supervised_models(samples,options);write(argv[2],supervised_model_text(model));write(argv[3],supervised_validation_json(validation)+"\n");
    std::ostringstream predictions;predictions.imbue(std::locale::classic());predictions<<std::setprecision(17)<<"{\"schema\":\"zynq_vibration.supervised_full_fit_policy_replay.v1\",\"validated\":false,\"accuracy_measured\":false,\"scope\":\"full_fit_training_replay_and_unlabelled_capture_behavior_only\",\"windows\":[";bool first=true;for(const auto& query:queries){const auto& w=query.second;if(!first)predictions<<',';first=false;predictions<<"{\"capture_sha256\":"<<quote(w.capture_sha256)<<",\"source_path\":"<<quote(w.source_path)<<",\"raw_window_sha256\":"<<quote(w.raw_window_sha256)<<",\"label_for_organization_only\":"<<(query.first<4?quote(labels[query.first]):"null")<<",\"result\":"<<supervised_inference_json(infer_supervised(w,model))<<'}';}predictions<<"]}\n";write(argv[4],predictions.str());
    std::cout<<"trained="<<model.kind<<" active_features="<<supervised_active_feature_indices(options).size()<<" source_files="<<samples.size()<<" training_windows="<<model.training_windows<<" heldout_sessions="<<validation.heldout_sessions<<" internal_session_equal_window_accuracy="<<validation.accuracy<<" machine_validated=false\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
