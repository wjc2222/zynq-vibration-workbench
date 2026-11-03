// Host-only display fidelity/budget checks. No device or user recording access.
#define main bridgeExecutableMain
#include "../main.cpp"
#undef main
#include <iostream>

void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);QJsonArray cases;
    try {
        std::uint32_t rng=0x781217u;
        for(int rate:{5000,10000,25000,50000})for(bool noise:{false,true}) {
            Preview view;view.rate=rate;view.clock=50000000;view.ppr=1;view.range=5000000;
            view.job=view.session=QString(32,'a');view.source="adc_unconfirmed";view.hardware=true;view.revision=1;
            const auto first=quint64(rate)*935/100;
            for(int i=0;i<rate/4;++i) {
                vibration::Record r;r.index=first+quint64(i);r.tick=r.index*quint64(view.clock/rate);r.flags=1;
                for(int ch=0;ch<3;++ch) {
                    rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;
                    r.raw[std::size_t(ch)]=noise?std::int16_t(rng):std::int16_t(std::lround(2./5*32768*std::sin(2*3.141592653589793*1000*double(r.index)/rate+ch*.2)));
                }
                view.records.push_back(r);
            }
            auto payload=summarize(view,32);const auto detail=payload.value("waveform_detail").toObject();
            check(detail.value("sample_count")==512,"detail count");
            check(decimal(detail.value("first_index"))==view.records[view.records.size()-512].index,"detail original index");
            check(detail.value("time_origin_index")=="0","live global seconds");
            auto raw=QByteArray::fromBase64(detail.value("raw_base64").toString().toLatin1());
            check(raw.size()==512*6,"detail bytes");
            const auto* bytes=reinterpret_cast<const unsigned char*>(raw.constData());
            for(int i=0;i<512;++i)for(int ch=0;ch<3;++ch)
                check(std::int16_t(u16(bytes+6*i+2*ch))==view.records[view.records.size()-512+std::size_t(i)].raw[std::size_t(ch)],"original adjacent ADC sample mismatch");
            const auto envelope=payload.value("waveform_envelope").toObject();
            check(envelope.value("connect_buckets")==false,"envelope must not connect");
            const auto channels=envelope.value("channels").toArray();
            for(int ch=0;ch<3;++ch) {
                const auto buckets=channels[ch].toArray();check(buckets.size()==32,"envelope bounded buckets");
                for(int b=0;b<32;++b) {
                    const auto item=buckets[b].toArray();const auto begin=view.records.size()*std::size_t(b)/32,end=view.records.size()*std::size_t(b+1)/32;
                    check(item[0].toDouble()==double(view.records[begin].index)/rate&&item[1].toDouble()==double(view.records[end-1].index)/rate,"bucket true time bounds");
                    for(auto i=begin;i<end;++i){const auto v=view.records[i].raw[std::size_t(ch)]*5./32768.;check(v>=item[2].toDouble()&&v<=item[3].toDouble(),"envelope excludes raw point");}
                }
            }
            const auto spectrum=payload.value("spectrum").toObject();const auto spectra=spectrum.value("channels").toArray();
            int fft=8192;while(fft>rate/4)fft/=2;
            const int expected=int(std::floor(2000./(double(rate)/fft)))+1;
            for(const auto& item:spectra) {
                const auto s=item.toObject();check(s.value("fft_size")==fft&&s.value("bin_count")==expected,"full uniform FFT bins");
                check(s.value("display_decimated")==false,"FFT must retain all bins");
                auto data=QByteArray::fromBase64(s.value("peak_amplitude_base64").toString().toLatin1());check(data.size()==expected*4,"float32 exact size");
                float peak=0;int index=0;
                for(int k=0;k<expected;++k) {
                    const auto bits=u32(reinterpret_cast<const unsigned char*>(data.constData()+k*4));float value=0;std::memcpy(&value,&bits,4);
                    check(std::isfinite(value)&&value>=0,"finite spectral amplitude");if(value>peak){peak=value;index=k;}
                }
                if(!noise){check(std::abs(index*double(rate)/fft-1000)<=double(rate)/fft,"1 kHz peak");check(peak>1.65f&&peak<2.05f,"1 kHz amplitude");}
            }
            const auto wire=encode({{"event","preview"},{"data",payload}},123);
            check(wire.size()<=18432,"115200 preview must fit two-second 80-percent UART budget");
            auto local=waveformDetail(view.records,rate,view.range,first,false);check(decimal(local.value("first_index"))==first&&decimal(local.value("time_origin_index"))==first,"record local zero origin");
            auto localEnvelope=waveformEnvelope(view.records,rate,view.range,first,128);check(localEnvelope.value("start_s")==0.,"record envelope local time");
            cases.append(QJsonObject{{"sample_rate_hz",rate},{"input",noise?"deterministic_noise":"1kHz_2V_sine"},{"input_count",int(view.records.size())},
                {"detail_count",512},{"fft_size",fft},{"bin_count",expected},{"wire_bytes",wire.size()},
                {"uart_115200_seconds_at_80_percent",wire.size()/9216.},
                {"legacy_m4_voltage_vertices",points(view.records,0,view.range,rate,view.clock,1,32).size()}});
        }
        std::cout<<QJsonDocument(QJsonObject{{"status","passed"},{"cases",cases},{"synthetic_method_demonstration",true}}).toJson().constData();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
