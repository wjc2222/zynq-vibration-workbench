#include "records.hpp"
#include "../../workbench/record_labels.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

using namespace streambridge;
namespace {
int checks=0;
void check(bool passed,const char* description){require(passed,"TEST",description);++checks;}
void put32(unsigned char* p,quint32 n){for(int i=0;i<4;++i)p[i]=static_cast<unsigned char>(n>>(8*i));}
void write(const QString& path,const QByteArray& data){QFile file(path);require(file.open(QIODevice::WriteOnly)&&file.write(data)==data.size(),"TEST","Unable to create owned test fixture");}
QByteArray read(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"TEST","Cannot read owned test fixture");return file.readAll();}
struct Fixture {
    QString root,id;QByteArray bin,meta,label;
    QString directory()const{return root+'/'+id;}
    QString path(const QString& name)const{return directory()+'/'+name;}
};
Fixture fixture(const QString& root,QChar token='a',bool labeled=true) {
    check(QDir(root).exists()||QDir().mkdir(root),"Create isolated case root");Fixture out;out.root=root;
    const auto uuid=QString(32,token);out.id=uuid+"-000000";check(QDir().mkdir(out.directory()),"Create isolated record directory");
    QByteArray raw(32,'\0'),bin(4096,'\0');auto* h=reinterpret_cast<unsigned char*>(bin.data());std::memcpy(h,"CAPSTRM1",8);
    put32(h+8,1);put32(h+12,4096);put32(h+16,32);put32(h+20,2);put32(h+24,5000);put32(h+28,50000000);put32(h+32,1);put32(h+36,1);put32(h+40,32);put32(h+60,1);put32(h+80,0x50301);put32(h+84,5000000);put32(h+96,vibration::crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()),32));
    const auto session=QByteArray::fromHex(uuid.toLatin1());std::memcpy(h+112,session.constData(),16);put32(h+144,5000);put32(h+148,1);put32(h+152,4096);put32(h+100,vibration::crc32(h,4096));
    const QJsonObject meta{{"schema","vibration.stream.v1"},{"format","CAPSTRM1"},{"segment_id",out.id},{"session_id",uuid},{"job_id",uuid},{"segment_index","0"},{"global_first_index","0"},{"record_count",1},{"first_tick","0"},{"last_tick","0"},{"sample_rate_hz",5000},{"clock_hz",50000000},{"ppr",1},{"payload_crc32",double(u32(h+96))},{"header_crc32",double(u32(h+100))},{"synthetic",true},{"hardware_confirmed",false},{"source","test_fixture"},{"path",out.path("data.BIN")},{"meta_path",out.path("data.meta.json")}};
    bin+=raw;out.bin=bin;out.meta=QJsonDocument(meta).toJson();write(out.path("data.BIN"),out.bin);write(out.path("data.meta.json"),out.meta);
    if(labeled){QString error;check(vibration::workbench::saveRecordLabel(out.path("data.BIN"),vibration::workbench::RecordLabel::Looseness,uuid,nullptr,&error),"Real operator-label writer creates fixture sidecar");out.label=read(out.path("data.labels.json"));}
    return out;
}
QJsonObject item(const QJsonObject& result){return result.value("results").toArray().at(0).toObject();}
void noRemoval(const QJsonObject& result){check(!result.value("deleted").toBool()&&result.value("bytes")==0&&result.value("files_removed")==0&&!result.value("durability_confirmed").toBool(),"Rejected operation reports no successful deletion");}
void unchanged(const Fixture& f){check(read(f.path("data.BIN"))==f.bin&&read(f.path("data.meta.json"))==f.meta&&read(f.path("data.labels.json"))==f.label,"Operation preserves all original unselected or rejected bytes");}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir temporary;check(temporary.isValid(),"Temporary fixture root");const auto root=temporary.path();
        {
            auto selected=fixture(root+"/selected"),kept=fixture(selected.root,'b');
            const auto reference=selected.root+"/reference-library.json";const auto referenceBytes=QByteArray("{\"source\":\"")+selected.path("data.BIN").toUtf8()+"\"}";write(reference,referenceBytes);
            const auto result=deleteRecords(selected.root,{selected.id});const auto r=item(result);
            check(r.value("deleted").toBool()&&r.value("durability_confirmed").toBool()&&r.value("files_removed")==3&&r.value("directory_removed").toBool(),"Selected real labelled record deletes exactly BIN/meta/label");
            check(r.value("bytes").toDouble()==selected.bin.size()+selected.meta.size()+selected.label.size()&&result.value("deleted_records")==1,"Selected label bytes included in deletion totals");
            check(!QFileInfo::exists(selected.directory()),"Selected record directory removed");unchanged(kept);
            check(read(reference)==referenceBytes&&!QFileInfo::exists(selected.path("data.BIN")),"Reference library remains intact with missing source for later validation");
            const auto unlabeled=fixture(selected.root,'c',false);const auto plain=item(deleteRecords(selected.root,{unlabeled.id}));check(plain.value("deleted").toBool()&&plain.value("files_removed")==2,"Unlabelled pair remains deletable");
        }
        for(const auto test:QStringList{"payload-identity","schema","unknown-file","writer-lock","symlink","hardlink"}) {
            auto f=fixture(root+'/'+test);QString outside;
            if(test=="payload-identity"||test=="schema") {
                auto label=parse(f.label);if(test=="schema")label["schema"]="foreign.document.v1";else {auto identity=label.value("record").toObject();identity["payload_crc32"]="1";label["record"]=identity;}
                write(f.path("data.labels.json"),QJsonDocument(label).toJson());
            } else if(test=="unknown-file"||test=="writer-lock")write(f.path(test=="unknown-file"?"user-original.txt":"data.labels.json.lock"),"preserve unknown content");
            else {
                outside=f.root+"/outside-original-label.json";check(QFile::rename(f.path("data.labels.json"),outside),"Hold original label outside selected directory");
                const auto target=QFile::encodeName(outside),link=QFile::encodeName(f.path("data.labels.json"));const int rc=test=="symlink" ? ::symlink(target.constData(),link.constData()) : ::link(target.constData(),link.constData());check(rc==0,"Create controlled link fixture");
            }
            const auto before=read(f.path("data.BIN")),meta=read(f.path("data.meta.json")),label=read(f.path("data.labels.json"));const auto r=item(deleteRecords(f.root,{f.id}));noRemoval(r);
            check(read(f.path("data.BIN"))==before&&read(f.path("data.meta.json"))==meta&&read(f.path("data.labels.json"))==label,"Invalid/foreign label and unknown entries prevent all deletion");
            if(!outside.isEmpty())check(read(outside)==f.label,"External link target preserved");
        }
        {
            auto f=fixture(root+"/label-before-move");const auto held=f.root+"/held-original-label.json";
            const auto r=item(deleteRecords(f.root,{f.id},[&](const QString&){check(QFile::rename(f.path("data.labels.json"),held),"Hold verified label inode");write(f.path("data.labels.json"),f.label);}));noRemoval(r);
            check(r.value("error").toObject().value("code")=="RECORD_CHANGED"&&read(held)==f.label,"Same-content replacement label inode is rejected");unchanged(f);
        }
        {
            auto f=fixture(root+"/label-after-move");const auto held=f.root+"/held-original-label.json",outside=f.root+"/outside-original.txt";write(outside,"external original");
            const auto r=item(deleteRecords(f.root,{f.id},{},[&](const QString&,const QString& phase,const QString& staging){if(phase!="after_move")return;const auto label=f.root+'/'+staging+"/data.labels.json";check(QFile::rename(label,held),"Hold label after staging");check(!::symlink(QFile::encodeName(outside).constData(),QFile::encodeName(label).constData()),"Replace staged label with symbolic link");}));noRemoval(r);
            check(read(f.path("data.BIN"))==f.bin&&read(f.path("data.meta.json"))==f.meta&&read(held)==f.label&&read(outside)=="external original","Post-staging label symlink prevents unlinks and preserves target");
        }
        {
            auto f=fixture(root+"/staging-directory-race");const auto held=f.root+"/held-selected-directory",foreign=f.root+"/foreign";check(QDir().mkdir(foreign),"Create staging replacement fixture");write(foreign+"/user-original.txt","foreign staging original");
            const auto r=item(deleteRecords(f.root,{f.id},{},[&](const QString&,const QString& phase,const QString& staging){if(phase!="after_move")return;check(QDir().rename(f.root+'/'+staging,held)&&QDir().rename(foreign,f.root+'/'+staging),"Swap staging directory after initial inode check");}));noRemoval(r);
            check(read(held+"/data.BIN")==f.bin&&read(held+"/data.labels.json")==f.label&&!QFileInfo::exists(f.directory()),"Replaced staging inode leaves original selected files untouched and unrestored");check(read(f.root+'/'+r.value("remaining_staging_id").toString()+"/user-original.txt")=="foreign staging original","Replacement staging contents preserved");
        }
        {
            auto f=fixture(root+"/partial-after-bin");
            const auto r=item(deleteRecords(f.root,{f.id},{},[&](const QString&,const QString& phase,const QString& staging){if(phase=="after_bin")write(f.root+'/'+staging+"/user-original.txt","race injected original");}));
            const auto staged=f.root+'/'+r.value("remaining_staging_id").toString();check(!r.value("deleted").toBool()&&r.value("files_removed")==1&&r.value("bytes")==f.bin.size()&&r.value("error").toObject().value("code")=="PARTIAL_DELETE","Partial failure reports only the actual removed BIN");
            check(!QFileInfo::exists(staged+"/data.BIN")&&read(staged+"/data.meta.json")==f.meta&&read(staged+"/data.labels.json")==f.label&&read(staged+"/user-original.txt")=="race injected original","Partial failure preserves metadata, label and unknown entry in explicit staging");
        }
        {
            auto f=fixture(root+"/partial-label-replacement");const auto held=f.root+"/held-original-label.json";
            const auto r=item(deleteRecords(f.root,{f.id},{},[&](const QString&,const QString& phase,const QString& staging){if(phase!="after_meta")return;const auto path=f.root+'/'+staging+"/data.labels.json";check(QFile::rename(path,held),"Hold original label after pair unlinks");write(path,f.label);}));
            const auto staged=f.root+'/'+r.value("remaining_staging_id").toString();check(!r.value("deleted").toBool()&&r.value("files_removed")==2&&r.value("bytes")==f.bin.size()+f.meta.size()&&r.value("error").toObject().value("code")=="PARTIAL_DELETE","Late label inode change reports partial pair deletion, never success");
            check(read(staged+"/data.labels.json")==f.label&&read(held)==f.label,"Late label replacement and original label both remain recoverable");
        }
        std::printf("PASS %d checks: real labels, explicit selection, unknown/link refusal, identity races and partial deletion protection; synthetic files only\n",checks);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
