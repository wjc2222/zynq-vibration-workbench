#pragma once
#include "../acquisition/canonical_base64.hpp"
#include <vibration/core.hpp>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vibration::workbench::stream_contract {
inline quint64 decimal(const QJsonValue& value) {
    if(!value.isString()||!QRegularExpression("^(0|[1-9][0-9]{0,19})$").match(value.toString()).hasMatch())throw std::runtime_error("Invalid decimal uint64");
    bool ok=false;auto n=value.toString().toULongLong(&ok);if(!ok)throw std::runtime_error("Decimal uint64 overflow");return n;
}
inline quint32 integer(const QJsonValue& value,quint32 low,quint32 high) {
    const auto n=value.toDouble(-1);if(!value.isDouble()||!std::isfinite(n)||std::floor(n)!=n||n<low||n>high)throw std::runtime_error("Invalid stream numeric field");return quint32(n);
}
inline quint16 u16(const unsigned char* p){return quint16(p[0])|(quint16(p[1])<<8);}
inline quint32 u32(const unsigned char* p){return quint32(p[0])|(quint32(p[1])<<8)|(quint32(p[2])<<16)|(quint32(p[3])<<24);}
inline quint64 u64(const unsigned char* p){return u32(p)|(quint64(u32(p+4))<<32);}
struct Preview {
    QString job,session,source;
    quint64 sequence=0,first=0,firstTick=0,lastTick=0,total=0,anchor=0;
    quint32 count=0,rate=0,ppr=0;
    bool hardware=false,synthetic=false;
    QByteArray raw;
};
inline Preview decode(const QJsonObject& frame) {
    if(frame.value("schema")!="vibration.stream.v1"||frame.value("type")!="event"||frame.value("event")!="preview"||frame.value("emitter")!="stream_service")throw std::runtime_error("Invalid stream preview envelope");
    const QStringList required{"schema","type","event","emitter","id","request_id","job_id","session_id","source","synthetic","hardware_confirmed","frame_seq","global_first_index","first_tick","last_tick","record_count","sample_rate_hz","clock_hz","ppr","adc_range_uv","channel_map","record_bytes","crc32","raw_base64"};
    for(const auto& key:required)if(!frame.contains(key))throw std::runtime_error(("Missing stream preview field: "+key).toStdString());
    for(auto it=frame.begin();it!=frame.end();++it)if(!required.contains(it.key())&&!QStringList{"total_records","observed_gap_records","mailbox_dropped_records"}.contains(it.key()))throw std::runtime_error("Unknown stream preview field");
    Preview p;p.job=frame.value("job_id").toString();p.session=frame.value("session_id").toString();p.source=frame.value("source").toString();
    if(!QRegularExpression("^[0-9a-f]{32}$").match(p.session).hasMatch()||p.job!=p.session||!frame.value("id").isString()||frame.value("id")!=frame.value("request_id"))throw std::runtime_error("Invalid stream preview session identity");
    p.sequence=decimal(frame.value("frame_seq"));p.first=decimal(frame.value("global_first_index"));p.firstTick=decimal(frame.value("first_tick"));p.lastTick=decimal(frame.value("last_tick"));
    if(frame.contains("total_records"))p.total=decimal(frame.value("total_records"));
    p.count=integer(frame.value("record_count"),1,2000);p.rate=integer(frame.value("sample_rate_hz"),1,50000);p.ppr=integer(frame.value("ppr"),1,4096);
    if((p.rate!=50000&&p.rate!=25000&&p.rate!=10000&&p.rate!=5000)||p.count!=p.rate/25||integer(frame.value("clock_hz"),50000000,50000000)!=50000000||
       integer(frame.value("adc_range_uv"),5000000,5000000)!=5000000||integer(frame.value("channel_map"),0x50301,0x50301)!=0x50301||integer(frame.value("record_bytes"),32,32)!=32||!p.sequence)throw std::runtime_error("Unsupported stream preview acquisition metadata");
    if(!frame.value("synthetic").isBool()||!frame.value("hardware_confirmed").isBool())throw std::runtime_error("Missing stream source types");
    p.synthetic=frame.value("synthetic").toBool();p.hardware=frame.value("hardware_confirmed").toBool();
    if(!((p.source=="test_fixture"&&p.synthetic&&!p.hardware)||(p.source=="adc_unconfirmed"&&!p.synthetic&&p.hardware)||(p.source=="hardware_test"&&p.synthetic&&p.hardware)))throw std::runtime_error("Inconsistent stream preview source");
    if(p.first>std::numeric_limits<quint64>::max()-p.count||(p.total&&p.first+p.count>p.total))throw std::runtime_error("Stream preview exceeds session range");
    const auto crc=integer(frame.value("crc32"),0,0xffffffffu);
    if(!frame.value("raw_base64").isString()||!transport::decodeCanonicalBase64(frame.value("raw_base64").toString(),int(p.count*32),p.raw)||
       vibration::crc32(reinterpret_cast<const std::uint8_t*>(p.raw.constData()),std::size_t(p.raw.size()))!=crc)throw std::runtime_error("Stream preview base64/CRC mismatch");
    const quint64 step=50000000u/p.rate;
    if(p.first>std::numeric_limits<quint64>::max()/step||p.firstTick<p.first*step)throw std::runtime_error("Stream preview tick underflow");
    p.anchor=p.firstTick-p.first*step;
    if(p.first+p.count-1>(std::numeric_limits<quint64>::max()-p.anchor)/step||p.lastTick!=p.anchor+(p.first+p.count-1)*step)throw std::runtime_error("Stream preview last tick mismatch");
    for(quint32 i=0;i<p.count;++i){const auto* r=reinterpret_cast<const unsigned char*>(p.raw.constData())+i*32;const auto flags=u16(r+22);
        if(u64(r)!=p.first+i||u64(r+8)!=p.anchor+(p.first+i)*step||!(flags&1u)||(flags&~31u)||((flags&2u)&&(!u32(r+24)||(flags&4u))))throw std::runtime_error("Stream preview record continuity/flags mismatch");}
    return p;
}
}
