#include "records.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

using namespace streambridge;
static void put32(unsigned char* p,quint32 n){for(int i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(8*i));}
static void write(const QString& path,const QByteArray& data){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(data)==data.size(),"TEST","Unable to create owned test fixture");}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir temporary;require(temporary.isValid(),"TEST","No temporary directory");const auto root=temporary.path(),uuid=QString(32,'a'),id=uuid+"-000000",original=root+"/held-original",foreign=root+"/foreign-user-directory";
        require(QDir().mkdir(root+'/'+id)&&QDir().mkdir(foreign),"TEST","Fixture mkdir failed");
        QByteArray raw(32,'\0'),bin(4096,'\0');auto* h=reinterpret_cast<unsigned char*>(bin.data());std::memcpy(h,"CAPSTRM1",8);
        put32(h+8,1);put32(h+12,4096);put32(h+16,32);put32(h+20,2);put32(h+24,5000);put32(h+28,50000000);put32(h+32,1);put32(h+36,1);put32(h+40,32);put32(h+60,1);put32(h+80,0x50301);put32(h+84,5000000);put32(h+96,vibration::crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()),32));
        auto session=QByteArray::fromHex(uuid.toLatin1());std::memcpy(h+112,session.constData(),16);put32(h+144,5000);put32(h+148,1);put32(h+152,4096);put32(h+100,vibration::crc32(h,4096));
        const QJsonObject meta{{"schema","vibration.stream.v1"},{"format","CAPSTRM1"},{"segment_id",id},{"session_id",uuid},{"job_id",uuid},{"segment_index","0"},{"global_first_index","0"},{"record_count",1},{"first_tick","0"},{"last_tick","0"},{"sample_rate_hz",5000},{"clock_hz",50000000},{"ppr",1},{"payload_crc32",double(u32(h+96))},{"header_crc32",double(u32(h+100))},{"synthetic",true},{"hardware_confirmed",false},{"source","test_fixture"},{"path",root+'/'+id+"/data.BIN"},{"meta_path",root+'/'+id+"/data.meta.json"}};
        bin+=raw;write(root+'/'+id+"/data.BIN",bin);write(root+'/'+id+"/data.meta.json",QJsonDocument(meta).toJson());write(foreign+"/user-original.txt","preserve unknown original");
        const auto result=deleteRecords(root,{id},[&](const QString&){require(QDir().rename(root+'/'+id,original)&&QDir().rename(foreign,root+'/'+id),"TEST","Controlled directory replacement failed");});
        const auto item=result.value("results").toArray().at(0).toObject();require(!item.value("deleted").toBool()&&item.value("bytes")==0&&item.value("files_removed")==0&&item.value("error").toObject().value("code")=="RECORD_CHANGED","TEST","Replaced directory was not rejected");
        require(QFileInfo(original+"/data.BIN").size()==bin.size()&&QFileInfo(original+"/data.meta.json").isFile(),"TEST","Original selected pair was modified");
        const auto staged=item.value("remaining_staging_id").toString();require(staged.startsWith(".deleting-")&&QFileInfo(root+'/'+staged+"/user-original.txt").isFile()&&!QFileInfo(root+'/'+id).exists(),"TEST","Unknown replacement was removed or blindly restored");
        std::puts("PASS selected-directory replacement is detected after atomic rename; both original files and foreign content preserved");return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
