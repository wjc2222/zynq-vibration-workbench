#include "raw_window_stats.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace vibration;
void check(bool value) { if(!value) throw std::runtime_error("raw-window amplitude oracle mismatch"); }
bool close(double a,double b){return std::abs(a-b)<1e-10;}
int main(){
    using workbench::PlotSeries;
    const double nan=std::numeric_limits<double>::quiet_NaN();
    QVector<PlotSeries> axes{{"CH1",{},{{0,-3},{1,4},{1.5,nan},{2,0}}},{"CH2",{},{}},{"CH3",{},{{0,2},{1,2},{2,2}}}};
    const auto stats=serial::rawWindowStats(axes,0,2,7);
    check(stats.value("derived_before_display_decimation").toBool()&&stats.value("unit")=="V"&&stats.value("input_revision").toInt()==7);
    const auto ch=stats.value("channels").toArray();const auto first=ch[0].toObject();
    check(first.value("sample_count").toInt()==3&&first.value("has_gaps").toBool());
    check(close(first.value("mean").toDouble(),1.0/3)&&close(first.value("rms").toDouble(),std::sqrt(25.0/3)));
    check(close(first.value("ac_rms").toDouble(),std::sqrt(74.0/9))&&first.value("peak_to_peak").toDouble()==7);
    check(first.value("minimum").toDouble()==-3&&first.value("maximum").toDouble()==4);
    check(ch[1].toObject().value("sample_count").toInt()==0&&ch[1].toObject().value("rms").isNull());
    check(ch[2].toObject().value("rms").toDouble()==2&&ch[2].toObject().value("ac_rms").toDouble()==0);
    const auto one=serial::rawWindowStats(axes,.5,1.2,8).value("channels").toArray()[0].toObject();
    check(one.value("sample_count").toInt()==1&&one.value("rms").toDouble()==4&&one.value("peak_to_peak").toDouble()==0);
    axes[0].points.clear();for(int i=0;i<101;++i)axes[0].points.push_back({double(i),i==50?10.0:0.0});
    const auto impulse=serial::rawWindowStats(axes,0,100,9).value("channels").toArray()[0].toObject();
    check(impulse.value("sample_count").toInt()==101&&close(impulse.value("rms").toDouble(),10/std::sqrt(101.0)));
    check(axes[0].points.size()==101&&axes[0].points[50].y()==10);
    std::puts("raw_window_stats independent DC/impulse/empty/gap/range tests passed");
}
