#include "vibration/candidate_model.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace vibration;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
// Mathematical feature fixtures test the domain policy, not machine accuracy.
CandidateWindow normal(unsigned index,double rpm){
    CandidateWindow w;w.setup_id="speed_domain_qa";w.configuration="abcd1234";
    w.sample_rate_hz=5000;w.first_index=index*10000;w.last_index=w.first_index+9999;
    w.rpm=w.rpm_min=w.rpm_max=rpm;w.fresh_periods=30;w.valid=true;
    for(unsigned ch=0;ch<3;++ch){auto* f=w.features.data()+12*ch;
        f[0]=.01;f[1]=1.5;f[2]=2;f[3]=.008;f[4]=.01;f[5]=.7;
        f[6]=.01;f[7]=.03;f[8]=.01;f[9]=.02;f[10]=.02;f[11]=.2;
    }return w;
}
CandidateModel baseline(double rpm){std::vector<CandidateWindow> windows;for(unsigned i=0;i<5;++i)windows.push_back(normal(i,rpm));return fit_normal_baseline(windows);}
void outside(const CandidateWindow& window,const CandidateModel& model){
    const auto r=infer_candidate(window,model);
    check(r.status=="outside_operating_range"&&r.classification=="unknown","out-of-domain input remains unknown");
    check(std::find(r.reasons.begin(),r.reasons.end(),"speed_outside_normal_training_domain")!=r.reasons.end(),"stable reason retained for existing clients");
    check(std::all_of(r.scores.begin(),r.scores.end(),[](double x){return x==0;}),"domain rejection precedes and suppresses pattern scores");
}
}
int main(){try{
    const auto m=baseline(1200);
    check(m.rpm_min==1080&&m.rpm_max==1320,"normal baseline retains existing minus/plus ten percent domain");
    for(double rpm:{1080.,1200.,1320.})check(infer_candidate(normal(8,rpm),m).classification=="normal","domain endpoints are inclusive");
    outside(normal(8,1351),m);outside(normal(8,2500),m);
    auto w=normal(8,1200);w.rpm_min=std::nextafter(m.rpm_min,0.);outside(w,m);
    w=normal(8,1200);w.rpm_max=std::nextafter(m.rpm_max,2000.);outside(w,m);
    // A permissible mean cannot hide one fresh speed period outside the domain.
    w=normal(8,1300);w.rpm_max=1330;outside(w,m);
    for(double rpm:{1351.,2500.}){
        const auto matching=baseline(rpm);
        check(infer_candidate(normal(8,rpm),matching).classification=="normal","matching baseline enables the new operating point without a false mechanical maximum");
    }
    std::vector<CandidateWindow> mixed;for(unsigned i=0;i<5;++i)mixed.push_back(normal(i,i==4?1400:1200));
    bool rejected=false;try{fit_normal_baseline(mixed);}catch(const AnalysisError&){rejected=true;}
    check(rejected,"one baseline cannot silently span unrelated speed operating points");
    const auto text=candidate_model_text(m);const auto restored=parse_candidate_model(text);
    check(candidate_model_text(restored)==text,"v1 model representation is unchanged");outside(normal(8,1351),restored);
    std::cout<<"PASS: mathematical RPM-domain boundary, full-window bounds, 1351/2500 mismatch, matching-baseline and v1 persistence regressions; no physical machine validation.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
