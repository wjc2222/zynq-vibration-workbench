#pragma once
#include "protocol.hpp"
#include <vibration/core.hpp>
#include <algorithm>
#include <cstring>
#include <limits>

namespace streambridge {
// Display transport only. These functions neither resample nor modify raw SD
// records. Detail contains adjacent ADC samples; envelopes are intervals, never
// vertices of a continuous waveform.
inline QJsonObject waveformDetail(const std::vector<vibration::Record>& records,
        int rate,int range,quint64 origin,bool newest=true,std::size_t limit=512) {
    const auto count=std::min(limit,records.size());
    const auto first=newest?records.size()-count:std::size_t(0);
    QByteArray bytes;bytes.reserve(int(count*6));
    for(std::size_t i=first;i<first+count;++i)for(auto raw:records[i].raw) {
        const auto value=std::uint16_t(raw);bytes.append(char(value&255));bytes.append(char(value>>8));
    }
    return {{"schema","vibration.waveform.raw.v1"},{"encoding","s16le-interleaved-base64"},
        {"sample_count",int(count)},{"first_index",number(records[first].index)},
        {"sample_rate_hz",rate},{"adc_range_uv",range},{"time_origin_index",number(origin)},
        {"raw_base64",QString::fromLatin1(bytes.toBase64())},{"unit","V"},
        {"representation","contiguous_original_samples"}};
}
inline QJsonObject waveformEnvelope(const std::vector<vibration::Record>& records,
        int rate,int range,quint64 origin,int budget) {
    QJsonArray channels;
    const auto count=records.size(),buckets=std::min(count,std::size_t(std::max(1,budget)));
    const double scale=double(range)/1e6/32768.;
    for(int ch=0;ch<3;++ch) {
        QJsonArray values;
        for(std::size_t b=0;b<buckets;++b) {
            const auto begin=count*b/buckets,end=count*(b+1)/buckets;
            std::int16_t lo=std::numeric_limits<std::int16_t>::max(),hi=std::numeric_limits<std::int16_t>::min();
            for(auto i=begin;i<end;++i){lo=std::min(lo,records[i].raw[std::size_t(ch)]);hi=std::max(hi,records[i].raw[std::size_t(ch)]);}
            values.append(QJsonArray{double(records[begin].index-origin)/rate,
                double(records[end-1].index-origin)/rate,lo*scale,hi*scale});
        }
        channels.append(values);
    }
    return {{"schema","vibration.waveform.envelope.v1"},{"unit","V"},
        {"start_s",double(records.front().index-origin)/rate},{"end_s",double(records.back().index-origin)/rate},
        {"channels",channels},{"representation","min_max_intervals"},{"connect_buckets",false},
        {"sample_count",double(count)},{"time_origin_index",number(origin)}};
}
inline QString float32Base64(const std::vector<double>& values,double scale=1.) {
    static_assert(sizeof(float)==4&&std::numeric_limits<float>::is_iec559,"IEEE float32 required");
    QByteArray bytes;bytes.reserve(int(values.size()*4));
    for(double value:values) {
        const float f=float(value*scale);std::uint32_t bits=0;std::memcpy(&bits,&f,4);
        for(int j=0;j<4;++j)bytes.append(char((bits>>(8*j))&255));
    }
    return QString::fromLatin1(bytes.toBase64());
}
inline QJsonObject packedSpectrum(const vibration::Spectrum& spectrum,const QString& unit="V",double scale=1.) {
    return {{"encoding","f32le-base64"},{"peak_amplitude_base64",float32Base64(spectrum.peak_amplitude,scale)},
        {"bin_count",int(spectrum.peak_amplitude.size())},{"first_hz",0},{"bin_width_hz",spectrum.bin_width_hz},
        {"fft_size",double(spectrum.fft_size)},{"segment_count",double(spectrum.segment_count)},
        {"unit",unit},{"display_decimated",false},{"window","hann"},
        {"window_duration_s",spectrum.window_duration_s},{"enbw_hz",spectrum.enbw_hz}};
}
}
