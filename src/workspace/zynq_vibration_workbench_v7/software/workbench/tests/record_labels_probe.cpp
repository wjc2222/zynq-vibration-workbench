#include "record_labels.hpp"
#include <vibration/core.hpp>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>
#include <functional>
#include <stdexcept>

using namespace vibration::workbench;
namespace {
QJsonArray checks;
void check(bool passed,const QString& description) {
    checks.append(QJsonObject{{"check",description},{"passed",passed}});
    if(!passed)throw std::runtime_error(description.toStdString());
}
QByteArray read(const QString& path) { QFile file(path);check(file.open(QIODevice::ReadOnly),"open fixture "+path);return file.readAll(); }
void write(const QString& path,const QByteArray& bytes) { QFile file(path);check(file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(bytes)==bytes.size(),"write isolated fixture "+QFileInfo(path).fileName()); }
void word(QByteArray& bytes,int offset,quint32 value) {qToLittleEndian(value,reinterpret_cast<uchar*>(bytes.data()+offset));}
QByteArray fixture(bool stream,bool synthetic) {
    QByteArray bytes(4096+32,0);bytes.replace(0,8,stream?QByteArray("CAPSTRM1",8):QByteArray("CAPDDR2\0",8));
    word(bytes,8,stream?1:2);word(bytes,12,4096);word(bytes,16,32);word(bytes,20,2);word(bytes,24,5000);word(bytes,28,50000000);
    word(bytes,32,1);word(bytes,36,1);word(bytes,40,32);word(bytes,48,!stream&&synthetic?64:0);word(bytes,60,1);word(bytes,80,328449);word(bytes,84,5000000);
    if(stream){bytes[112]=1;word(bytes,144,50000);word(bytes,148,synthetic?1:0);word(bytes,152,4096);}
    word(bytes,96,vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()+4096),32));
    word(bytes,100,vibration::crc32(reinterpret_cast<const std::uint8_t*>(bytes.constData()),stream?4096:128));
    return bytes;
}
QJsonObject object(const QString& path) {return QJsonDocument::fromJson(read(path)).object();}
void mutate(const QString& path,const std::function<void(QJsonObject&)>& change) {auto value=object(path);change(value);write(path,QJsonDocument(value).toJson());}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QJsonObject report{{"schema","v7-operator-label-storage-qa-v1"},{"scope","Isolated synthetic/header fixtures only; labels do not declare machine truth or validate payloads."}};
    try {
        QTemporaryDir work;check(work.isValid(),"isolated temporary storage");QString error;
        for(auto label:{RecordLabel::Unlabeled,RecordLabel::Normal,RecordLabel::Looseness,RecordLabel::Misalignment,RecordLabel::Imbalance}) {
            RecordLabel parsed=RecordLabel::Unlabeled;check(recordLabelFromKey(recordLabelKey(label),&parsed)&&parsed==label&&!recordLabelText(label).isEmpty(),"canonical label roundtrip "+recordLabelKey(label));
        }
        check(!recordLabelFromKey("model_prediction",nullptr)&&recordLabelKey(static_cast<RecordLabel>(99)).isEmpty(),"model/unknown labels are rejected");
        const auto path=work.filePath("record.BIN"),meta=work.filePath("record.meta.json"),sidecar=recordLabelSidecarPath(path);
        write(path,fixture(true,true));write(meta,"{\"synthetic\":true,\"source_kind\":\"synthetic\"}");
        const auto originalBin=read(path),originalMeta=read(meta);
        RecordLabelAnnotation annotation;
        check(loadRecordLabel(path,&annotation,&error)&&!annotation.exists&&annotation.label==RecordLabel::Unlabeled&&annotation.synthetic,"old records remain unlabelled with their synthetic guard");
        check(saveRecordLabel(path,RecordLabel::Normal,"session-one",&annotation,&error),"save operator normal "+error);
        check(annotation.exists&&annotation.label==RecordLabel::Normal&&annotation.sessionId=="session-one"&&annotation.synthetic&&annotation.history.size()==1,"save returns committed annotation");
        auto json=object(sidecar);
        check(json.value("source")=="operator"&&json.value("inference_input").isBool()&&!json.value("inference_input").toBool()&&json.value("purpose")=="dataset_organization_only","operator provenance and inference isolation persisted");
        check(json.value("training_use")=="prohibited_synthetic"&&json.value("synthetic_record").toBool(),"synthetic normal label never grants mechanical-training eligibility");
        check(saveRecordLabel(path,RecordLabel::Imbalance,{},&annotation,&error)&&annotation.history.size()==2,"relabel an existing record and append history");
        check(annotation.history[0].toObject().value("label")=="normal"&&annotation.history[1].toObject().value("label")=="imbalance"&&annotation.history[1].toObject().value("source")=="operator","manual history retains previous and new labels");
        check(saveRecordLabel(path,RecordLabel::Unlabeled,{},&annotation,&error)&&annotation.history.size()==3&&annotation.label==RecordLabel::Unlabeled,"clearing a label retains audit history");
        check(loadRecordLabel(path,&annotation,&error)&&annotation.exists&&annotation.label==RecordLabel::Unlabeled&&!annotation.updatedAt.isEmpty(),"annotation survives a fresh read");
        check(read(path)==originalBin&&read(meta)==originalMeta,"BIN and original source metadata remain byte-identical");
        const auto goodLabel=read(sidecar);
        {QLockFile lock(sidecar+".lock");check(lock.tryLock(),"hold isolated annotation lock");check(!saveRecordLabel(path,RecordLabel::Normal,{},nullptr,&error)&&!error.isEmpty(),"concurrent write fails visibly");}
        check(read(sidecar)==goodLabel,"failed concurrent write preserves committed sidecar");
        write(sidecar,"{truncated");
        check(!loadRecordLabel(path,&annotation,&error)&&!error.isEmpty()&&!annotation.exists,"corrupt label is an error, never an unlabelled success");
        check(!saveRecordLabel(path,RecordLabel::Normal,{},nullptr,&error)&&read(sidecar)=="{truncated","corrupt sidecar cannot be silently replaced by relabel");
        write(sidecar,goodLabel);
        mutate(sidecar,[](QJsonObject& value){value["source"]="model_prediction";});
        check(!loadRecordLabel(path,&annotation,&error)&&!error.isEmpty(),"model-generated annotation provenance rejected");
        write(sidecar,goodLabel);
        mutate(sidecar,[](QJsonObject& value){value["training_use"]="eligible";});
        check(!loadRecordLabel(path,&annotation,&error)&&!error.isEmpty(),"sidecar cannot promote synthetic labels into eligible training data");
        write(sidecar,goodLabel);
        mutate(sidecar,[](QJsonObject& value){auto history=value.value("history").toArray();auto entry=history.last().toObject();entry["updated_at"]="1970-01-01T00:01:00.000Z";entry["clock_valid"]=false;history[history.size()-1]=entry;value["history"]=history;value["updated_at"]=entry.value("updated_at");value["clock_valid"]=false;});
        check(loadRecordLabel(path,&annotation,&error)&&!annotation.clockValid&&annotation.updatedAt.startsWith("1970"),"unset board clock is explicitly retained as uncalibrated");
        mutate(sidecar,[](QJsonObject& value){value["clock_valid"]=true;auto history=value.value("history").toArray();auto entry=history.last().toObject();entry["clock_valid"]=true;history[history.size()-1]=entry;value["history"]=history;});
        check(!loadRecordLabel(path,&annotation,&error)&&!error.isEmpty(),"1970 board time cannot claim calibrated annotation date");
        write(sidecar,goodLabel);
        auto replacement=fixture(true,false);word(replacement,96,123);word(replacement,100,0);word(replacement,100,vibration::crc32(reinterpret_cast<const std::uint8_t*>(replacement.constData()),4096));write(path,replacement);
        check(!loadRecordLabel(path,&annotation,&error)&&error.contains(QStringLiteral("身份")),"replaced BIN cannot inherit labels by filename alone");
        write(path,originalBin);check(loadRecordLabel(path,&annotation,&error),"restoring exact BIN restores valid association");
        auto damaged=originalBin;damaged[30]=1;write(path,damaged);check(!saveRecordLabel(path,RecordLabel::Normal,{},nullptr,&error)&&error.contains("CRC"),"invalid header cannot receive successful annotation");write(path,originalBin);
        const auto legacy=work.filePath("legacy.BIN");write(legacy,fixture(false,true));
        check(saveRecordLabel(legacy,RecordLabel::Looseness,{},&annotation,&error)&&annotation.synthetic,"legacy hardware TEST header is protected without meta");
        const auto duplicate=work.filePath("duplicate.BIN");write(duplicate,fixture(false,false));write(work.filePath("duplicate.meta.json"),"{\"synthetic\":true,\"synthetic\":false}");
        check(saveRecordLabel(duplicate,RecordLabel::Misalignment,{},&annotation,&error)&&annotation.synthetic&&object(recordLabelSidecarPath(duplicate)).value("training_use")=="prohibited_synthetic","duplicate source key cannot erase known synthetic declaration");
        check(QFile::remove(work.filePath("duplicate.meta.json"))&&loadRecordLabel(duplicate,&annotation,&error)&&annotation.synthetic,"removing source sidecar cannot erase synthetic guard already recorded by annotation");
        check(saveRecordLabel(duplicate,RecordLabel::Normal,{},&annotation,&error)&&annotation.synthetic&&object(recordLabelSidecarPath(duplicate)).value("training_use")=="prohibited_synthetic","relabel normal keeps prior synthetic training prohibition");
        const auto undeclared=work.filePath("unconfirmed.BIN");write(undeclared,fixture(false,false));
        check(saveRecordLabel(undeclared,RecordLabel::Normal,{},&annotation,&error)&&!annotation.synthetic&&object(recordLabelSidecarPath(undeclared)).value("training_use")=="requires_source_validation","unconfirmed header fixture receives no physical-training authority");
        const auto brokenMeta=work.filePath("broken-meta.BIN");write(brokenMeta,fixture(false,false));write(work.filePath("broken-meta.meta.json"),"{not-json");
        check(!saveRecordLabel(brokenMeta,RecordLabel::Normal,{},nullptr,&error)&&!QFileInfo::exists(recordLabelSidecarPath(brokenMeta))&&!error.isEmpty(),"unreadable/invalid source metadata fails without publishing sidecar");
        const auto occupied=work.filePath("occupied.BIN");write(occupied,fixture(true,true));check(QDir().mkdir(recordLabelSidecarPath(occupied)),"occupy sidecar path with directory");
        check(!saveRecordLabel(occupied,RecordLabel::Normal,{},nullptr,&error)&&!error.isEmpty(),"non-file sidecar collision reports save failure");
        check(!saveRecordLabel("relative.BIN",RecordLabel::Normal,{},nullptr,&error)&&!error.isEmpty(),"relative record paths rejected");
        check(read(path)==originalBin&&read(meta)==originalMeta,"all error paths preserve original BIN and metadata");
        report["passed"]=true;
    } catch(const std::exception& e) {report["passed"]=false;report["error"]=QString::fromUtf8(e.what());}
    report["checks"]=checks;auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);std::fwrite(bytes.constData(),1,std::size_t(bytes.size()),stdout);return report.value("passed").toBool()?0:1;
}
