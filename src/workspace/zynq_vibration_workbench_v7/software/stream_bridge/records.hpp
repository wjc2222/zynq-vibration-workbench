#pragma once
#include "protocol.hpp"
#include "display_payload.hpp"
#include <vibration/core.hpp>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <array>
#include <cerrno>
#include <cstring>
#ifdef VIBRATION_STREAM_DELETE_TEST_HOOKS
#include <functional>
#endif
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace streambridge {
constexpr qint64 maxFile=64LL*1024*1024;
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd){}
    ~Fd(){if(value>=0)::close(value);}
    Fd(const Fd&)=delete; Fd& operator=(const Fd&)=delete;
};
inline bool same(const struct stat& a,const struct stat& b) {
    return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size &&
        a.st_mtim.tv_sec==b.st_mtim.tv_sec && a.st_mtim.tv_nsec==b.st_mtim.tv_nsec &&
        a.st_ctim.tv_sec==b.st_ctim.tv_sec && a.st_ctim.tv_nsec==b.st_ctim.tv_nsec;
}
// Walk each component without following a symlink. Never create/fallback/mount.
inline void safeDirectory(const QString& path) {
    require(QDir::isAbsolutePath(path) && QDir::cleanPath(path)==path,"UNSAFE_STORAGE","Storage path must be canonical and absolute");
    QString current;
    for(const auto& part:path.split('/',Qt::SkipEmptyParts)) {
        current+='/'+part; struct stat s{};
        require(::lstat(QFile::encodeName(current).constData(),&s)==0 && S_ISDIR(s.st_mode),"UNSAFE_STORAGE","Storage has a missing or symbolic-link directory component");
    }
}
inline std::shared_ptr<Fd> openDirectory(const QString& path) {
    require(QDir::isAbsolutePath(path)&&QDir::cleanPath(path)==path,"UNSAFE_STORAGE","Invalid managed directory path");
    auto current=std::make_shared<Fd>(::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));
    require(current->value>=0,"UNSAFE_STORAGE","Cannot open filesystem root");
    for(const auto& part:path.split('/',Qt::SkipEmptyParts)) {
        auto next=std::make_shared<Fd>(::openat(current->value,QFile::encodeName(part).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        require(next->value>=0,"UNSAFE_STORAGE","Managed directory component cannot be followed safely");current=std::move(next);
    }
    return current;
}
inline bool recordName(const QString& name) {return QRegularExpression("^[0-9a-f]{32}-[0-9]{6,20}$").match(name).hasMatch();}
struct File {
    QString root,id,name,kind,sha; std::shared_ptr<Fd> fd,directory; struct stat identity{},directoryIdentity{};
    QString path()const{return root+'/'+id+'/'+name;}
    qint64 bytes()const{return identity.st_size;}
    bool unchanged()const {
        struct stat current{},p{},d{};
        try{safeDirectory(root+'/'+id);}catch(...){return false;}
        return ::fstat(fd->value,&current)==0 && same(identity,current) &&
            ::fstatat(directory->value,QFile::encodeName(name).constData(),&p,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(p.st_mode) && same(identity,p) &&
            ::lstat(QFile::encodeName(root+'/'+id).constData(),&d)==0 && S_ISDIR(d.st_mode) && d.st_ino==directoryIdentity.st_ino && d.st_dev==directoryIdentity.st_dev;
    }
    QJsonObject json()const {
        QJsonObject out{{"kind",kind},{"name",name},{"bytes",double(bytes())}};
        if(!sha.isEmpty())out["sha256"]=sha;
        return out;
    }
};
inline File openFile(const QString& root,const QString& id,const QString& name,const QString& kind,qint64 limit) {
    File out;out.root=root;out.id=id;out.name=name;out.kind=kind;out.directory=openDirectory(root+'/'+id);
    require(::fstat(out.directory->value,&out.directoryIdentity)==0,"UNSAFE_STORAGE","Cannot inspect opened segment directory");
    out.fd=std::make_shared<Fd>(::openat(out.directory->value,QFile::encodeName(name).constData(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
    require(out.fd->value>=0 && ::fstat(out.fd->value,&out.identity)==0 && S_ISREG(out.identity.st_mode) &&
        out.identity.st_nlink==1 && out.bytes()>0 && out.bytes()<=limit,"INVALID_RECORD","Capture files must be bounded ordinary single-link files");
    require(out.unchanged(),"RECORD_CHANGED","Capture identity changed while opening");return out;
}
inline QByteArray readAt(const File& file,qint64 offset,int length) {
    require(offset>=0 && length>=0 && offset<=file.bytes()-length,"INVALID_RANGE","Read outside capture file");
    require(file.unchanged(),"RECORD_CHANGED","Capture pair identity changed");
    QByteArray data(length,'\0');int got=0;
    while(got<length) {
        auto n=::pread(file.fd->value,data.data()+got,std::size_t(length-got),offset+got);
        if(n<0&&errno==EINTR)continue;
        require(n>0,"FILE_ERROR","Capture file read failed");got+=int(n);
    }
    require(file.unchanged(),"RECORD_CHANGED","Capture changed during read");return data;
}
inline void fullHash(File& file) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for(qint64 offset=0;offset<file.bytes();) {
        auto block=readAt(file,offset,int(std::min(qint64(65536),file.bytes()-offset)));
        hash.addData(block);offset+=block.size();
    }
    require(file.unchanged(),"RECORD_CHANGED","Capture changed during SHA256");file.sha=QString::fromLatin1(hash.result().toHex());
}
struct Record {
    QString id,job; File bin,meta; QJsonObject metadata; quint64 first{},index{};
    QJsonObject json()const {return {{"record_id",id},{"job_id",job},{"session_id",metadata.value("session_id")},{"bytes",double(bin.bytes())},
        {"synthetic",metadata.value("synthetic")},{"hardware_confirmed",metadata.value("hardware_confirmed")},
        {"source",metadata.value("source")},{"actual_source",metadata.value("source")},{"segment_index",number(index)},
        {"global_first_index",number(first)},{"record_count",metadata.value("record_count")},
        {"files",QJsonArray{bin.json(),meta.json()}},{"scope","completed_segment"},
        {"integrity",bin.sha.isEmpty()?"header_and_metadata_only":"complete_file_verified"}};}
};
inline std::shared_ptr<Record> inspectRecord(const QString& root,const QString& id,bool full=false,vibration::Capture* loaded=nullptr) {
    require(recordName(id),"UNKNOWN_RECORD","record_id must be a listed opaque segment ID");
    auto r=std::make_shared<Record>();r->id=id;
    r->bin=openFile(root,id,"data.BIN","bin",maxFile);r->meta=openFile(root,id,"data.meta.json","meta",65536);
    r->metadata=parse(readAt(r->meta,0,int(r->meta.bytes())));
    auto header=readAt(r->bin,0,4096);auto* p=reinterpret_cast<unsigned char*>(header.data());
    require(std::memcmp(p,"CAPSTRM1",8)==0 && u32(p+8)==1 && u32(p+12)==4096 && u32(p+16)==32 && u32(p+20)==2 &&
        u32(p+32)==u32(p+36) && u32(p+36)>0 && quint64(u32(p+36))*32==u32(p+40) &&
        quint64(r->bin.bytes())==4096+quint64(u32(p+40)) && u32(p+44)==0 && u32(p+56)==0 && u32(p+152)==4096 && !(u32(p+148)&~1u),
        "INVALID_RECORD","Invalid CAPSTRM1 segment header/length");
    const auto headerCrc=u32(p+100);std::memset(p+100,0,4);
    require(vibration::crc32(p,4096)==headerCrc,"INVALID_RECORD","Segment header CRC mismatch");
    const auto& m=r->metadata;const bool synthetic=u32(p+148)&1u;r->first=u64(p+104);r->index=u64(p+128);r->job=m.value("job_id").toString();
    const auto uuid=QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(p+112),16).toHex());
    require(m.value("schema")=="vibration.stream.v1" && m.value("format")=="CAPSTRM1" && m.value("segment_id")==id &&
        m.value("session_id")==uuid && r->job==uuid && id.startsWith(uuid+'-') &&
        decimal(m.value("segment_index"))==r->index && decimal(m.value("global_first_index"))==r->first &&
        m.value("record_count").toDouble(-1)==u32(p+36) && decimal(m.value("first_tick"))==u64(p+64) && decimal(m.value("last_tick"))==u64(p+72) &&
        m.value("sample_rate_hz").toDouble(-1)==u32(p+24) && m.value("clock_hz").toDouble(-1)==u32(p+28) && m.value("ppr").toDouble(-1)==u32(p+60) &&
        m.value("payload_crc32").toDouble(-1)==u32(p+96) && m.value("header_crc32").toDouble(-1)==headerCrc &&
        validSource(m) && m.value("synthetic").toBool()==synthetic &&
        m.value("path")==r->bin.path() && m.value("meta_path")==r->meta.path(),"METADATA_MISMATCH","Metadata is not bound to this complete segment");
    if(full) {
        fullHash(r->bin);fullHash(r->meta);
        auto capture=vibration::load_capture(r->bin.path().toUtf8().toStdString());
        require(capture.integrity_verified && capture.header.stream_segment && capture.header.stream_flags==quint32(synthetic) &&
            capture.source_metadata.synthetic==synthetic,"INVALID_RECORD","Core segment validation failed");
        if(loaded)*loaded=std::move(capture);
    }
    require(r->bin.unchanged()&&r->meta.unchanged(),"RECORD_CHANGED","Capture changed while validating");return r;
}
struct Page {QJsonArray records;QString next;int rejected=0;};
inline Page listPage(const QString& root,const QString& cursor,int limit) {
    safeDirectory(root);require(cursor.isEmpty()||recordName(cursor),"INVALID_CURSOR","Invalid record cursor");
    // Only limit+1 names are retained, regardless of the number of SD files.
    std::set<QString> names;auto rootFd=openDirectory(root);DIR* directory=::fdopendir(::dup(rootFd->value));
    require(directory,"UNSAFE_STORAGE","Cannot enumerate SD record directory");
    while(auto* entry=::readdir(directory)) {
        const auto name=QString::fromLocal8Bit(entry->d_name);
        if(name<=cursor||!recordName(name))continue;
        names.insert(name);if(int(names.size())>limit+1)names.erase(std::prev(names.end()));
    }
    ::closedir(directory);Page out;int count=0;
    for(const auto& name:names) {
        if(count==limit)break;
        ++count;
        try{out.records.append(inspectRecord(root,name)->json());}catch(...){++out.rejected;}
        out.next=name;
    }
    if(int(names.size())<=limit)out.next.clear();
    return out;
}
inline std::unique_ptr<File> optionalOperatorLabel(const Record& record) {
    struct stat entry{};
    if(::fstatat(record.bin.directory->value,"data.labels.json",&entry,AT_SYMLINK_NOFOLLOW)) {
        require(errno==ENOENT,"UNSAFE_RECORD","Cannot inspect optional operator label");return {};
    }
    auto file=std::make_unique<File>();file->root=record.bin.root;file->id=record.id;file->name="data.labels.json";file->kind="operator_label";
    file->directory=record.bin.directory;file->directoryIdentity=record.bin.directoryIdentity;
    file->fd=std::make_shared<Fd>(::openat(file->directory->value,"data.labels.json",O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
    require(file->fd->value>=0&&!::fstat(file->fd->value,&file->identity)&&S_ISREG(file->identity.st_mode)&&file->identity.st_nlink==1&&
        file->bytes()>0&&file->bytes()<=4LL*1024*1024&&same(entry,file->identity),"UNSAFE_RECORD","Operator label must be an unchanged bounded ordinary single-link file");
    const auto label=parse(readAt(*file,0,int(file->bytes())));
    const QJsonObject identity{{"file_name","data.BIN"},{"bytes",QString::number(record.bin.bytes())},
        {"header_crc32",QString::number(quint32(record.metadata.value("header_crc32").toDouble()))},
        {"payload_crc32",QString::number(quint32(record.metadata.value("payload_crc32").toDouble()))}};
    const auto validEntry=[](const QJsonObject& value) {
        const auto text=value.value("updated_at").toString();const auto time=QDateTime::fromString(text,Qt::ISODateWithMs);
        const auto key=value.value("label").toString();
        return (key=="unlabeled"||key=="normal"||key=="looseness"||key=="misalignment"||key=="imbalance")&&value.value("source")=="operator"&&
            text.endsWith('Z')&&time.isValid()&&value.value("clock_valid").isBool()&&value.value("clock_valid").toBool()==(time.date().year()>=2020)&&
            value.value("session_id").isString()&&value.value("session_id").toString().size()<=128;
    };
    require(label.value("schema")=="vibration.operator-record-label.v1"&&label.value("record").isObject()&&label.value("record").toObject()==identity&&
        validEntry(label)&&label.value("purpose")=="dataset_organization_only"&&label.value("inference_input").isBool()&&!label.value("inference_input").toBool()&&
        label.value("synthetic_record").isBool()&&label.value("history").isArray(),"INVALID_OPERATOR_LABEL","Optional label schema, provenance or BIN identity does not match selected record");
    const auto history=label.value("history").toArray();
    require(!history.isEmpty(),"INVALID_OPERATOR_LABEL","Optional operator label has no history");
    for(const auto& item:history)require(item.isObject()&&validEntry(item.toObject()),"INVALID_OPERATOR_LABEL","Optional operator label history is invalid");
    for(const auto& key:{"label","source","updated_at","clock_valid","session_id"})
        require(history.last().toObject().value(key)==label.value(key),"INVALID_OPERATOR_LABEL","Optional operator label differs from latest history");
    const bool synthetic=label.value("synthetic_record").toBool();const auto training=label.value("training_use").toString();
    require((training=="prohibited_synthetic"||training=="requires_source_validation")&&(!synthetic||training=="prohibited_synthetic")&&
        (!record.metadata.value("synthetic").toBool()||synthetic),"INVALID_OPERATOR_LABEL","Optional operator label lacks source-use protection");
    require(file->unchanged(),"RECORD_CHANGED","Operator label changed while validating");return file;
}
inline void exactRecordFilesDirectory(int directory,const std::set<QString>& expected,const QString& code="UNSAFE_RECORD") {
    Fd copy(::openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    require(copy.value>=0,"UNSAFE_RECORD","Cannot inspect selected segment directory");
    DIR* entries=::fdopendir(::dup(copy.value));require(entries,"UNSAFE_RECORD","Cannot enumerate selected segment directory");
    std::set<QString> names;bool unsafe=false;errno=0;
    while(auto* entry=::readdir(entries)) {
        const auto name=QString::fromLocal8Bit(entry->d_name);if(name=="."||name=="..")continue;
        if(!expected.count(name)){unsafe=true;break;}
        struct stat item{};
        if(::fstatat(directory,entry->d_name,&item,AT_SYMLINK_NOFOLLOW)||!S_ISREG(item.st_mode)||item.st_nlink!=1){unsafe=true;break;}
        names.insert(name);
    }
    const int directoryError=errno;::closedir(entries);
    require(!unsafe&&!directoryError&&names==expected,code,"Selected directory differs from the verified ordinary record files; unknown entries are preserved");
}
inline void exactPairDirectory(int directory,const File* label=nullptr) {
    std::set<QString> names{"data.BIN","data.meta.json"};if(label)names.insert("data.labels.json");exactRecordFilesDirectory(directory,names);
}
inline bool sameFileAt(int directory,const File& file) {
    struct stat open{},entry{};
    return !::fstat(file.fd->value,&open)&&same(file.identity,open)&&
        !::fstatat(directory,QFile::encodeName(file.name).constData(),&entry,AT_SYMLINK_NOFOLLOW)&&S_ISREG(entry.st_mode)&&same(file.identity,entry);
}
inline QJsonObject deleteRecords(const QString& root,const QStringList& ids
#ifdef VIBRATION_STREAM_DELETE_TEST_HOOKS
    ,const std::function<void(const QString&)>& beforeMove={}
    ,const std::function<void(const QString&,const QString&,const QString&)>& atPhase={}
#endif
    ) {
    // Root and each component are descriptor anchored; the wire never supplies
    // paths. Only the explicit selected records are touched. No recursive unlink.
    auto rootFd=openDirectory(root);QJsonArray results;qint64 totalBytes=0;int deleted=0;
    for(const auto& id:ids) {
        QJsonObject result{{"record_id",id},{"deleted",false},{"bytes",0},{"files_removed",0},{"directory_removed",false},{"durability_confirmed",false}};
        QString quarantine;std::shared_ptr<Record> record;std::unique_ptr<File> label;bool moved=false,stagedIdentityConfirmed=false;int removed=0;qint64 bytes=0;
        try {
            require(recordName(id),"UNSUPPORTED_RECORD","Only opaque CAPSTRM1 segment IDs are deletable; legacy IDs and paths are preserved");
            record=inspectRecord(root,id);label=optionalOperatorLabel(*record);exactPairDirectory(record->bin.directory->value,label.get());
            struct stat parentEntry{};
            require(!::fstatat(rootFd->value,QFile::encodeName(id).constData(),&parentEntry,AT_SYMLINK_NOFOLLOW)&&S_ISDIR(parentEntry.st_mode)&&
                parentEntry.st_dev==record->bin.directoryIdentity.st_dev&&parentEntry.st_ino==record->bin.directoryIdentity.st_ino&&
                record->bin.directoryIdentity.st_dev==record->meta.directoryIdentity.st_dev&&record->bin.directoryIdentity.st_ino==record->meta.directoryIdentity.st_ino&&
                sameFileAt(record->bin.directory->value,record->bin)&&sameFileAt(record->bin.directory->value,record->meta)&&
                (!label||sameFileAt(record->bin.directory->value,*label)),"RECORD_CHANGED","Selected segment or operator-label identity changed before deletion");
            quarantine=".deleting-"+id+'-'+QUuid::createUuid().toString(QUuid::Id128);
#ifdef VIBRATION_STREAM_DELETE_TEST_HOOKS
            if(beforeMove)beforeMove(id); // Test build only; never a wire/CLI option.
#endif
#ifdef SYS_renameat2
            require(!::syscall(SYS_renameat2,rootFd->value,QFile::encodeName(id).constData(),rootFd->value,QFile::encodeName(quarantine).constData(),1u),"DELETE_FAILED","Cannot exclusively move selected segment into private deletion staging");
#else
            fail("UNSUPPORTED_FILESYSTEM","Safe selected deletion requires renameat2(RENAME_NOREPLACE)");
#endif
            moved=true;
            struct stat staged{};
            const auto verifyStaging=[&] {
                stagedIdentityConfirmed=!::fstatat(rootFd->value,QFile::encodeName(quarantine).constData(),&staged,AT_SYMLINK_NOFOLLOW)&&S_ISDIR(staged.st_mode)&&
                    staged.st_dev==record->bin.directoryIdentity.st_dev&&staged.st_ino==record->bin.directoryIdentity.st_ino;
                require(stagedIdentityConfirmed,removed?"PARTIAL_DELETE":"RECORD_CHANGED","Deletion staging differs from the selected verified inode; replacement directory is preserved");
            };
            verifyStaging();
#ifdef VIBRATION_STREAM_DELETE_TEST_HOOKS
            if(atPhase)atPhase(id,"after_move",quarantine);
#endif
            verifyStaging();
            // Recheck after the atomic move, before either known file unlink.
            exactPairDirectory(record->bin.directory->value,label.get());
            require(sameFileAt(record->bin.directory->value,record->bin)&&sameFileAt(record->bin.directory->value,record->meta)&&
                (!label||sameFileAt(record->bin.directory->value,*label)),"RECORD_CHANGED","Selected record files changed after deletion staging");
            require(!::fsync(rootFd->value),"DELETE_FAILED","Unable to sync selected deletion staging");
            require(!::unlinkat(record->bin.directory->value,"data.BIN",0),"DELETE_FAILED","Unable to remove selected BIN");
            ++removed;bytes+=record->bin.bytes();
#ifdef VIBRATION_STREAM_DELETE_TEST_HOOKS
            if(atPhase)atPhase(id,"after_bin",quarantine);
#endif
            verifyStaging();
            std::set<QString> remaining{"data.meta.json"};if(label)remaining.insert("data.labels.json");
            exactRecordFilesDirectory(record->bin.directory->value,remaining,"PARTIAL_DELETE");
            require(sameFileAt(record->bin.directory->value,record->meta)&&(!label||sameFileAt(record->bin.directory->value,*label)),
                "PARTIAL_DELETE","BIN removed but remaining selected files changed; replacements are preserved");
            require(!::unlinkat(record->bin.directory->value,"data.meta.json",0),"PARTIAL_DELETE","BIN removed but selected metadata removal failed");
            ++removed;bytes+=record->meta.bytes();
#ifdef VIBRATION_STREAM_DELETE_TEST_HOOKS
            if(atPhase)atPhase(id,"after_meta",quarantine);
#endif
            verifyStaging();
            if(label) {
                exactRecordFilesDirectory(record->bin.directory->value,{"data.labels.json"},"PARTIAL_DELETE");
                require(sameFileAt(record->bin.directory->value,*label),"PARTIAL_DELETE","BIN and metadata removed but operator label changed; replacement is preserved");
                require(!::unlinkat(record->bin.directory->value,"data.labels.json",0),"PARTIAL_DELETE","BIN and metadata removed but operator label removal failed");
                ++removed;bytes+=label->bytes();
            }
            // An injected unknown entry can make rmdir fail; never remove it.
            require(!::fsync(record->bin.directory->value),"PARTIAL_DELETE","Selected files removed but directory sync failed");
            require(!::fstatat(rootFd->value,QFile::encodeName(quarantine).constData(),&staged,AT_SYMLINK_NOFOLLOW)&&S_ISDIR(staged.st_mode)&&
                staged.st_dev==record->bin.directoryIdentity.st_dev&&staged.st_ino==record->bin.directoryIdentity.st_ino,"RECORD_CHANGED","Deletion staging directory identity changed; replacement directory is preserved");
            require(!::unlinkat(rootFd->value,QFile::encodeName(quarantine).constData(),AT_REMOVEDIR),"PARTIAL_DELETE","Selected files removed but staging directory was not empty or could not be removed");
            moved=false;result["directory_removed"]=true;
            require(!::fsync(rootFd->value),"DELETE_DURABILITY_UNCONFIRMED","Selected pair removed but parent-directory sync failed");
            result["deleted"]=true;result["durability_confirmed"]=true;++deleted;
        }catch(const std::exception& e) {
            QString code="DELETE_FAILED";if(const auto* failure=dynamic_cast<const Failure*>(&e))code=failure->code;
            if(moved&&removed==0&&stagedIdentityConfirmed) {
#ifdef SYS_renameat2
                // If no selected file was removed, try to restore its original
                // opaque name without overwriting any concurrently created entry.
                struct stat restore{};
                const bool originalStaging=!::fstatat(rootFd->value,QFile::encodeName(quarantine).constData(),&restore,AT_SYMLINK_NOFOLLOW)&&S_ISDIR(restore.st_mode)&&
                    restore.st_dev==record->bin.directoryIdentity.st_dev&&restore.st_ino==record->bin.directoryIdentity.st_ino;
                if(originalStaging&&!::syscall(SYS_renameat2,rootFd->value,QFile::encodeName(quarantine).constData(),rootFd->value,QFile::encodeName(id).constData(),1u)) {
                    moved=false;(void)::fsync(rootFd->value);
                }
#endif
            }
            if(moved)result["remaining_staging_id"]=quarantine;
            result["error"]=QJsonObject{{"code",code},{"message",QString::fromUtf8(e.what())}};
        }
        result["bytes"]=double(bytes);result["files_removed"]=removed;totalBytes+=bytes;results.append(result);
    }
    return {{"results",results},{"requested_records",ids.size()},{"deleted_records",deleted},{"deleted_bytes",double(totalBytes)},
        {"storage","sd"},{"scope","explicit_selected_completed_segments"}};
}
inline QJsonObject decimateSpectrum(QJsonObject object,int limit=128) {
    const auto amplitudes=object.value("peak_amplitude").toArray();std::set<int> keep;
    if(amplitudes.size()<=limit)for(int i=0;i<amplitudes.size();++i)keep.insert(i);
    else {
        keep.insert(0);keep.insert(amplitudes.size()-1);
        for(int b=0;b<(limit-2)/2;++b) {
            const int begin=1+(amplitudes.size()-2)*b/((limit-2)/2),end=1+(amplitudes.size()-2)*(b+1)/((limit-2)/2);int lo=begin,hi=begin;
            for(int i=begin+1;i<end;++i){if(amplitudes[i].toDouble()<amplitudes[lo].toDouble())lo=i;if(amplitudes[i].toDouble()>amplitudes[hi].toDouble())hi=i;}
            keep.insert(lo);keep.insert(hi);
        }
    }
    for(const auto& key:QStringList{"frequency_hz","peak_amplitude","psd"}) {
        const auto a=object.value(key).toArray();if(a.size()!=amplitudes.size())continue;QJsonArray reduced;for(int i:keep)reduced.append(a[i]);object[key]=reduced;
    }
    object["display_decimated"]=int(keep.size())<amplitudes.size();object["original_bin_count"]=amplitudes.size();return object;
}
struct Task {QString kind,id,command,code,error;std::shared_ptr<Record> record;QJsonObject data;};
inline Task analyzeTask(const QString& root,const QString& recordId,const QString& requestId) {
    Task task;task.kind="analysis";task.command="analyze_record";task.id=requestId;
    try {
        vibration::Capture capture;task.record=inspectRecord(root,recordId,true,&capture);
        vibration::AnalysisConfig config;config.waveform_points=240;
        config.signal_kind=task.record->metadata.value("synthetic").toBool()?vibration::SignalKind::TestSignal:vibration::SignalKind::Unknown;
        auto result=parse(QByteArray::fromStdString(vibration::to_json(vibration::analyze(capture,config))));
        QJsonArray spectra;for(const auto& channel:result.value("spectra").toArray())spectra.append(decimateSpectrum(channel.toObject()));result["spectra"]=spectra;
        result["display_decimated"]=true;result["display_scope"]="completed_segment";
        result["waveform_representation"]="legacy_extrema_summary_not_raw";
        result["spectra_representation"]="legacy_extrema_summary_not_uniform_bins";
        if(!requestId.isEmpty()) {
            // The original 65536-point diagnostic calculation above stays
            // unchanged. The separately labelled display FFT averages the
            // entire selected record with 8192-point windows, not 128 extrema.
            auto displayConfig=config;displayConfig.fft_size=8192;displayConfig.waveform_points=12;
            const auto display=vibration::analyze(capture,displayConfig);
            const auto factor=std::abs(display.config.analog_gain*display.config.sensor_sensitivity_v_per_g);
            QJsonArray channels;for(const auto& channel:display.spectra)channels.append(packedSpectrum(channel,"V",factor));
            const auto origin=capture.records.front().index;const int rate=int(capture.header.sample_rate_hz),range=capture.header.adc_range_uv;
            result["display_detail"]=waveformDetail(capture.records,rate,range,origin,false);
            result["display_envelope"]=waveformEnvelope(capture.records,rate,range,origin,128);
            result["display_spectrum"]=QJsonObject{{"available",true},{"channels",channels},{"unit","V"},
                {"start_s",0.},{"end_s",double(capture.records.size()-1)/rate},{"max_hz",displayConfig.spectrum_max_hz},
                {"scope","complete_record"},{"time_origin_index",number(origin)},{"fft_target",8192},
                {"display_decimated",false}};
        } else {
            // Automatic completed-segment diagnostics are features only. Bulk
            // display data must not occupy the UART ahead of control replies.
            result.remove("waveform");result.remove("spectra");auto speed=result.value("speed").toObject();speed.remove("trace");result["speed"]=speed;
            result["display_scope"]="features_only";
        }
        require(task.record->bin.unchanged()&&task.record->meta.unchanged(),"RECORD_CHANGED","Segment changed during complete analysis");
        task.data={{"job_id",task.record->job},{"record_id",recordId},{"result",result},
            {"scope",requestId.isEmpty()?"completed_segment":"complete_record"},{"segment_only",true},
            {"source_metadata",task.record->metadata},{"files",QJsonArray{task.record->bin.json(),task.record->meta.json()}}};
    }catch(const Failure& e){task.code=e.code;task.error=QString::fromUtf8(e.what());}
    catch(const std::exception& e){task.code="ANALYSIS_FAILED";task.error=QString::fromUtf8(e.what());}
    return task;
}
}
