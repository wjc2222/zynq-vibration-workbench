#pragma once
#include <QByteArray>
#include <QString>
#include <array>

namespace vibration::transport {
// Strict RFC4648 standard-alphabet decoder for bounded raw-preview payloads.
// Reads QString UTF16 directly: no Latin1 copy and no decode/re-encode pass.
// Exact length, ASCII, padding placement/count and zero pad bits are required.
// On failure no partial decoded payload is exposed. CRC remains the caller's
// independent responsibility; this function establishes encoding canonicality.
inline bool decodeCanonicalBase64(const QString& encoded,int expectedBytes,QByteArray& decoded){
    constexpr int maximumBytes=4096*32;
    const auto reject=[&decoded]{decoded.clear();return false;};
    if(expectedBytes<0||expectedBytes>maximumBytes||encoded.size()!=4*((expectedBytes+2)/3))return reject();
    static constexpr auto alphabet=[] {
        std::array<unsigned char,128> table{};
        for(auto& value:table)value=255;
        for(int i=0;i<26;++i){table['A'+i]=static_cast<unsigned char>(i);table['a'+i]=static_cast<unsigned char>(i+26);}
        for(int i=0;i<10;++i)table['0'+i]=static_cast<unsigned char>(i+52);
        table['+']=62;table['/']=63;return table;
    }();
    decoded.resize(expectedBytes);
    const auto* input=encoded.utf16();auto* output=reinterpret_cast<unsigned char*>(decoded.data());
    const int completeGroups=expectedBytes/3;
    for(int group=0;group<completeGroups;++group,input+=4,output+=3){
        const auto a=input[0],b=input[1],c=input[2],d=input[3];
        if((a|b|c|d)&~127u)return reject();
        const auto x=alphabet[a],y=alphabet[b],z=alphabet[c],w=alphabet[d];
        if((x|y|z|w)&192u)return reject();
        output[0]=static_cast<unsigned char>((x<<2)|(y>>4));
        output[1]=static_cast<unsigned char>((y<<4)|(z>>2));
        output[2]=static_cast<unsigned char>((z<<6)|w);
    }
    const int remainder=expectedBytes%3;
    if(remainder){
        const auto a=input[0],b=input[1],c=input[2],d=input[3];
        if((a|b|c|d)&~127u)return reject();
        const auto x=alphabet[a],y=alphabet[b];
        if((x|y)&192u)return reject();
        if(remainder==1){
            if(c!='='||d!='='||(y&15u))return reject();
            output[0]=static_cast<unsigned char>((x<<2)|(y>>4));
        }else{
            const auto z=alphabet[c];
            if((z&192u)||d!='='||(z&3u))return reject();
            output[0]=static_cast<unsigned char>((x<<2)|(y>>4));
            output[1]=static_cast<unsigned char>((y<<4)|(z>>2));
        }
    }
    return true;
}
}
