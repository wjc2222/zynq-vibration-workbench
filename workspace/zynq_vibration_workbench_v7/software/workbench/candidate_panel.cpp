#include "candidate_panel.hpp"
#include "stream_preview_contract.hpp"
#include "touch_keyboard.hpp"
#include "record_labels.hpp"
#include "reference_display.hpp"
#include "vibration/reference_library.hpp"
#include "vibration/reference_validation.hpp"
#include "vibration/supervised_model.hpp"
#include "vibration/sha256.hpp"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QPainter>
#include <QProgressBar>
#include <QHeaderView>
#include <QSaveFile>
#include <QStackedWidget>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <future>
#include <map>
#include <set>

namespace vibration::workbench {
namespace {
QString q(const std::string&s){return QString::fromUtf8(s.data(),int(s.size()));}
bool referenceKind(const std::string&kind){return kind=="reference_prototype"||kind=="reference_exemplars"||kind=="reference_ensemble_v2"||kind=="reference_ensemble_v3";}
std::string modelDigest(const CandidateModel&model){const auto text=candidate_model_text(model);return sha256_hex(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());}
std::string modelDigest(const SupervisedModel&model){const auto text=supervised_model_text(model);return sha256_hex(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());}
QString label(const std::string&s){if(s=="normal")return QStringLiteral("正常候选");if(s=="imbalance")return QStringLiteral("不平衡候选");if(s=="misalignment")return QStringLiteral("不对中候选");if(s=="looseness")return QStringLiteral("松动候选");return QStringLiteral("未知 / 证据不足");}
QString resultLabel(const std::string&s){if(s=="normal")return QStringLiteral("正常");if(s=="imbalance")return QStringLiteral("不平衡");if(s=="misalignment")return QStringLiteral("不对中");if(s=="looseness")return QStringLiteral("松动");return {};}
QString reason(const std::string&s){
    if(s=="mechanical_source_not_confirmed")return QStringLiteral("尚未确认机械输入，请核实后勾选。");
    if(s=="synthetic_not_machine_evidence")return QStringLiteral("合成信号仅用于软件验证，不能建立实测基线或诊断机器。");
    if(s=="fewer_than_8_fresh_speed_periods")return QStringLiteral("不足 8 个新有效转速周期。");
    if(s=="speed_valid_fraction_below_90pct")return QStringLiteral("有效测速比例不足 90%。");
    if(s=="speed_cv_above_5pct")return QStringLiteral("转速波动超过 5%，需稳定后检测。");
    if(s=="required_order_bands_overlap_or_outside_2_500Hz")return QStringLiteral("倍频带重叠或超出 2–500 Hz，拒绝判别。");
    if(s=="model_setup_filter_conversion_or_sample_rate_mismatch")return QStringLiteral("台架、安装、采样档、FIR 或换算配置与模型不一致。");
    if(s=="speed_outside_normal_training_domain")return QStringLiteral("当前转速与模型基线不匹配，保留未知；此提示不表示机械超速。");
    if(s=="speed_outside_reference_coverage")return QStringLiteral("当前窗转速范围超出四类参考库已有覆盖，保持未知；请补采对应转速的四种工况，这不表示机械超速。");
    if(s=="insufficient_score_margin_or_out_of_distribution")return QStringLiteral("候选分数、领先差值不足，或超出训练分布。");
    if(s=="reference_class_coverage_missing_at_current_speed")return QStringLiteral("当前转速缺少某一类参考覆盖，请补采相近转速的对应工况；本次保持未知。");
    if(s=="at_least_two_independent_reference_files_per_class_required_at_current_speed")return QStringLiteral("当前转速每类至少需要两次独立采集；同次采集的多个窗不重复计数。");
    if(s=="insufficient_two_reference_support")return QStringLiteral("缺少两个足够相似的独立参考，请补采同类数据或检查工况。");
    if(s=="reference_session_identity_missing_ignored")return QStringLiteral("无采集会话标识的旧参考未计入独立支持。");
    if(s=="cross_speed_reference_support_limited_requires_machine_validation")return QStringLiteral("当前使用跨转速参考，候选结果需独立机械实测验证。");
    if(s=="single_reference_mode_match_requires_independent_confirmation")return QStringLiteral("单一参考模式匹配，待独立实测确认。");
    if(s=="acceleration_calibration_unconfirmed")return QStringLiteral("加速度标定未确认；可使用同链路电压基线。");
    if(s=="input_integrity_unverified")return QStringLiteral("输入完整性尚未核验。");
    if(s=="ppr_missing")return QStringLiteral("每转脉冲数 PPR 未确认。");
    if(s=="raw_sequence_or_tick_gap")return QStringLiteral("原始序号或采样时间有缺口，当前检测窗作废。");
    if(s=="accelerator_invalid_or_saturated")return QStringLiteral("分析结果无效或计算饱和。");
    const std::string referencePrefix="reference_band_rms_below_policy_CH";
    if(s.find(referencePrefix)==0)return QStringLiteral("CH%1参考频段幅值不足（工程门限%2码RMS），请检查传感器、增益与安装。").arg(q(s.substr(referencePrefix.size()))).arg(candidate_minimum_reference_rms_adc,0,'f',0);
    if(s.find("raw_adc_clipping_CH")==0)return QStringLiteral("原始 ADC 削顶：")+q(s.substr(17));
    if(s.find("flatline_or_below_")==0)return QStringLiteral("通道平线或低于数字噪声底限：")+q(s);
    if(s.find("reference_power_below_")==0)return QStringLiteral("参考频段功率不足，不能计算功率占比：")+q(s);
    return q(s);
}
QString p(const QString&s){return "<p>"+s.toHtmlEscaped()+"</p>";}
QString html(const QString&s){return "<html><head><style>body{color:#344255;font-family:sans-serif;font-size:14px}p{margin:7px 0}td,th{padding:5px 10px;border-bottom:1px solid #e4eaf1}</style></head><body>"+s+"</body></html>";}
bool sameInputConfig(const AnalysisConfig&a,const AnalysisConfig&b){return calibration_config_equal(a,b)&&a.calibration_confirmed==b.calibration_confirmed&&a.ppr_override==b.ppr_override;}
void saveModelAtomic(const QString&path,const QByteArray&bytes,const std::vector<std::string>&protectedPaths){
    const QFileInfo target(path);if(!QDir().mkpath(target.absolutePath()))throw AnalysisError("无法创建模型保存目录");
    validate_detection_outputs({path.toUtf8().toStdString()},protectedPaths);
    if(target.isSymLink()||target.isDir())throw AnalysisError("模型保存位置不是普通文件");
    if(bytes.size()>qint64(candidate_model_max_bytes))throw AnalysisError("模型或参考库超过16MiB限制");const auto atomicWrite=[](const QString&name,const QByteArray&data){QSaveFile file(name);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()||!file.commit())throw AnalysisError((QStringLiteral("模型保存失败：")+file.errorString()).toUtf8().toStdString());};
    if(target.exists()){
        QFile prior(path);if(!prior.open(QIODevice::ReadOnly)||prior.size()>qint64(candidate_model_max_bytes))throw AnalysisError("无法备份已有模型或参考库");const auto previous=prior.readAll();if(prior.error()!=QFile::NoError)throw AnalysisError("读取已有模型或参考库失败");
        const QDir history(target.absolutePath()+"/models");if(!QDir().mkpath(history.absolutePath()))throw AnalysisError("无法创建模型版本目录");
        const auto stamp=QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");QString backup;for(unsigned n=0;n<1000;++n){backup=history.filePath(QStringLiteral("%1-%2-%3.%4").arg(target.completeBaseName(),stamp).arg(n).arg(target.suffix()));if(!QFileInfo::exists(backup))break;backup.clear();}if(backup.isEmpty())throw AnalysisError("无法创建唯一备份名称");
        validate_detection_outputs({backup.toUtf8().toStdString()},protectedPaths);atomicWrite(backup,previous);
    }
    atomicWrite(path,bytes);
}
const char*names[]{"AC RMS","峰值因子","Pearson 峭度","1X RMS","0.5X 功率比","1X 功率比","1.5X 功率比","2X 功率比","2.5X 功率比","3X 功率比","4–10X 功率比","非谐波功率比"};
// A read-only view of the already extracted RPM-free window. Displaying these
// values never changes the model input, the inference result or source gates.
class SignalSummary final:public QWidget {
    std::optional<SupervisedWindow> current_;
    std::int32_t adcRangeUv_=0;
public:
    explicit SignalSummary(QWidget*parent=nullptr):QWidget(parent){
        setObjectName("detectionSignalSummary");setMinimumHeight(164);
        setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        setAccessibleName(QStringLiteral("三通道信号特征与频带功率占比"));
    }
    void setWindow(const SupervisedWindow*window,std::int32_t adcRangeUv=0){
        current_=window&&window->valid&&!window->synthetic?std::optional<SupervisedWindow>(*window):std::nullopt;
        adcRangeUv_=adcRangeUv>0?adcRangeUv:0;
        setProperty("hasSignal",bool(current_));
        QVariantList rms,volts,crest,kurtosis,bands;
        if(current_)for(unsigned ch=0;ch<3;++ch){rms.push_back(current_->ac_rms_adc[ch]);if(adcRangeUv_)volts.push_back(current_->ac_rms_adc[ch]*adcRangeUv_/32768./1000000.);crest.push_back(current_->features[ch*21+1]);kurtosis.push_back(current_->features[ch*21+2]);for(unsigned b=0;b<7;++b)bands.push_back(current_->features[ch*21+3+b]);}
        setProperty("acRmsAdc",rms);setProperty("acRmsVolts",volts);setProperty("displayAdcRangeUv",adcRangeUv_);setProperty("crestFactors",crest);setProperty("pearsonKurtosis",kurtosis);setProperty("powerFractions",bands);setToolTip(current_&&!adcRangeUv_?QStringLiteral("未确认记录量程，交流有效值暂不显示。"):QString());update();
    }
protected:
    void paintEvent(QPaintEvent*)override{
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const QColor ink("#263b54"),muted("#78879b");
        const std::array<QColor,3>colors{{QColor("#3769c7"),QColor("#268878"),QColor("#b78032")}};
        QFont font=this->font();font.setPixelSize(14);painter.setFont(font);
        painter.setPen(ink);painter.drawText(QRect(0,0,width(),24),Qt::AlignVCenter,QStringLiteral("三通道信号特征"));
        painter.setPen(muted);painter.drawText(QRect(width()/2,0,width()/2,24),Qt::AlignRight|Qt::AlignVCenter,QStringLiteral("频带功率占比 · 2–2000 Hz"));
        const double gap=10,cardWidth=(width()-2*gap)/3.;
        const char*frequency[]{"2–16","16–32","32–64","64–125","125–250","250–500","500–2k"};
        for(unsigned ch=0;ch<3;++ch){
            const QRectF card(ch*(cardWidth+gap),30,cardWidth,height()-31);
            painter.setPen(QColor("#e5ebf3"));painter.setBrush(QColor("#fbfcfe"));painter.drawRoundedRect(card,9,9);
            const double left=card.left()+14,right=card.right()-14;
            painter.setPen(colors[ch]);font.setPixelSize(15);font.setWeight(QFont::DemiBold);painter.setFont(font);painter.drawText(QRectF(left,card.top()+9,55,24),Qt::AlignVCenter,QStringLiteral("CH%1").arg(ch+1));
            painter.setPen(muted);font.setPixelSize(12);font.setWeight(QFont::Normal);painter.setFont(font);painter.drawText(QRectF(left+55,card.top()+9,right-left-55,24),Qt::AlignRight|Qt::AlignVCenter,QStringLiteral("交流有效值 / V"));
            painter.setPen(ink);font.setPixelSize(26);font.setWeight(QFont::DemiBold);painter.setFont(font);painter.drawText(QRectF(left,card.top()+33,cardWidth-28,32),Qt::AlignVCenter,current_&&adcRangeUv_?QString::number(current_->ac_rms_adc[ch]*adcRangeUv_/32768./1000000.,'f',4):QStringLiteral("—"));
            painter.setPen(muted);font.setPixelSize(13);font.setWeight(QFont::Normal);painter.setFont(font);painter.drawText(QRectF(left,card.top()+66,cardWidth-28,22),Qt::AlignVCenter,current_?QStringLiteral("峰值因子 %1    峭度 %2").arg(current_->features[ch*21+1],0,'f',2).arg(current_->features[ch*21+2],0,'f',2):QStringLiteral("等待完整的机械采集窗口"));
            if(current_){
                const double barWidth=(right-left)/7.,bottom=card.bottom()-23,top=card.top()+97,barHeight=std::max(8.,bottom-top);
                for(unsigned b=0;b<7;++b){const QRectF track(left+b*barWidth+2,top,barWidth-5,barHeight);painter.setPen(Qt::NoPen);painter.setBrush(QColor("#eaf0f8"));painter.drawRoundedRect(track,3,3);const double fraction=std::clamp(current_->features[ch*21+3+b],0.,1.);painter.setBrush(colors[ch]);if(fraction>0)painter.drawRoundedRect(QRectF(track.left(),bottom-barHeight*fraction,track.width(),barHeight*fraction),2,2);painter.setPen(muted);font.setPixelSize(cardWidth<340?10:11);painter.setFont(font);painter.drawText(QRectF(left+b*barWidth-2,bottom+4,barWidth+4,16),Qt::AlignCenter,QString::fromUtf8(frequency[b]));}
            }else{painter.setPen(QColor("#a4b0c0"));font.setPixelSize(13);painter.setFont(font);painter.drawText(QRectF(left,card.top()+93,cardWidth-28,std::max(20.,card.height()-104)),Qt::AlignVCenter,QStringLiteral("采集或打开 BIN 后显示"));}
        }
    }
};
}
std::array<unsigned,4> candidate_reference_coverage(const CandidateWindow&w,const CandidateModel&m){
    std::array<unsigned,4> counts{};
    const bool speedTolerant=m.kind=="reference_ensemble_v3";
    if((m.kind!="reference_exemplars"&&m.kind!="reference_ensemble_v2"&&!speedTolerant)||!std::isfinite(w.rpm_min)||!std::isfinite(w.rpm_max)||w.rpm_min<=0||w.rpm_min>w.rpm_max)return counts;
    std::array<std::set<std::string>,4> files,sessions;
    for(const auto&e:m.references){
        if(e.class_index>=4||e.capture_sha256.empty())continue;
        if((m.kind=="reference_ensemble_v2"||speedTolerant)&&e.session_id.empty())continue;
        if(!speedTolerant&&(w.rpm_min<e.rpm_min*(1-reference_ensemble_speed_tolerance)||w.rpm_max>e.rpm_max*(1+reference_ensemble_speed_tolerance)))continue;
        auto&seenFiles=files[e.class_index];auto&seenSessions=sessions[e.class_index];
        if(seenFiles.count(e.capture_sha256)||((m.kind=="reference_ensemble_v2"||speedTolerant)&&seenSessions.count(e.session_id)))continue;
        seenFiles.insert(e.capture_sha256);seenSessions.insert(e.session_id);++counts[e.class_index];
    }
    return counts;
}
std::vector<std::string> candidate_protected_output_paths(const QStringList& captures,
    const QStringList& configurationSources,const QString& activeModel,
    const QString& activeLibrary,CandidateWritePurpose purpose){
    std::vector<std::string> paths;
    const auto add=[&](const QString& path){if(!path.isEmpty())paths.push_back(path.toUtf8().toStdString());};
    // Sources are never exempt, even if an application target aliases a BIN
    // or its reserved sidecars. The writer also checks filesystem aliases.
    for(const auto& path:captures){if(path.isEmpty())continue;add(path);const QFileInfo f(path);
        add(f.absolutePath()+"/"+f.completeBaseName()+".meta.json");add(recordLabelSidecarPath(path));}
    const QString validation=activeModel.isEmpty()?QString():QFileInfo(activeModel).absolutePath()+"/reference-validation.json";
    const QString permitted=purpose==CandidateWritePurpose::ActiveModel?activeModel:
        purpose==CandidateWritePurpose::ActiveLibrary?activeLibrary:
        purpose==CandidateWritePurpose::ActiveValidation?validation:QString();
    const auto reserved=[&](const QString& path){
        if(path.isEmpty())return;
        if(!permitted.isEmpty()&&QFileInfo(path).absoluteFilePath()==QFileInfo(permitted).absoluteFilePath())return;
        add(path);
    };
    reserved(activeModel);reserved(activeLibrary);reserved(validation);
    for(const auto& path:configurationSources){if(purpose==CandidateWritePurpose::ActiveValidation)add(path);else reserved(path);}
    if(!activeModel.isEmpty())add(QFileInfo(activeModel).absolutePath()+"/calibration.json");
    return paths;
}
struct CandidatePanel::Impl {
    CandidatePanel*owner;
    QLabel *state,*subtitle,*profile,*settingsStatus;
    QLabel *scoreNote,*signalSource;
    std::array<QProgressBar*,4>scoreBars{};
    std::array<QLabel*,4>scoreValues{};
    SignalSummary*signalSummary;
    QDialog*settingsDialog;
    QLineEdit*setup;
    QCheckBox *mechanical,*normal,*legacyEnabled;
    QComboBox*backend;
    QPushButton *baseline,*save,*load,*features;
    QDialog*detailsDialog;
    QTabWidget*detailsTabs;
    std::array<QTextBrowser*,3>documents{};
    std::shared_ptr<const Capture>capture;
    AnalysisConfig config;
    std::optional<CandidateModel>model;
    std::optional<SupervisedModel>trainedModel;
    std::optional<SupervisedWindow>trainedWindow;
    SupervisedInference trainedInference;
    std::optional<CandidateWindow>window;
    CandidateInference inference;
    std::deque<Record>ring;
    Header liveHeader;
    QString job,storage,modelPath,pendingSegment,lastSegment;
    QString supervisedPath,supervisedValidationPath,supervisedSourcePath,legacyLibraryPath,libraryLoadPath;
    QString scheduledLibraryPath;
    bool scheduledLibraryKeepsQueue=false;
    std::uint64_t epoch=1,lastSubmitted=0,skipped=0;
    std::uint64_t observedEnd=0;
    qint64 lastRawMs=0;
    bool live=false,liveSynthetic=false,pending=false;
    bool modelSaved=false;
    bool supervisedSaved=false;
    QString supervisedSaveError;
    QString modelSaveError;
    QByteArray referenceValidation;
    std::string referenceValidationModelDigest;
    QString referenceValidationSummary;
    struct Task{std::uint64_t epoch=0;std::optional<CandidateWindow>window;std::optional<SupervisedWindow>trainedWindow;std::optional<CandidateModel>model;std::shared_ptr<const Capture>committed;std::string jsonl,error;QString exportPath;bool saveOnly=false,saveSupervised=false;};
    std::future<Task>task;
    QDialog*referenceDialog=nullptr;
    QLabel *referenceCounts=nullptr,*referenceStatus=nullptr;
    QTreeWidget*referenceList=nullptr;
    QPushButton *referenceBuild=nullptr,*referenceRemove=nullptr;
    QString libraryPath;
    std::vector<SupervisedSample>references;
    QCheckBox*referenceMechanical=nullptr;
    QStringList referenceLibrarySources;
    bool libraryNeedsLoad=false;
    std::uint64_t referenceEpoch=1;
    struct ReferenceRequest {QString path,label,hash,configuration,setup,session,updated;};
    std::deque<ReferenceRequest>referenceQueue;
    std::optional<ReferenceRequest>awaitingReferenceConfirmation;
    struct ReferenceTask {std::uint64_t generation=0;std::optional<SupervisedSample>sample;std::optional<SupervisedModel>model;std::vector<SupervisedSample>loaded;QString error;QByteArray validation;QString validationSummary;bool loading=false;};
    std::future<ReferenceTask>referenceTask;
    bool referenceModelNeedsSave=false;
    explicit Impl(CandidatePanel*p):owner(p){}
    bool hasDetectionModel()const{return model&&legacyEnabled->isChecked();}
    bool hasFourClassModel()const{return model&&referenceKind(model->kind);}
    void acceptSourceConfirmation(const AnalysisConfig&cfg){if(cfg.signal_kind==SignalKind::VibrationConfirmed&&!mechanical->isChecked()){QSignalBlocker block(mechanical);mechanical->setChecked(true);}}
    void maybeLoadLocalReferences(){if(libraryNeedsLoad&&referenceMechanical->isChecked()&&!referenceTask.valid()&&referenceQueue.empty())loadReferences(libraryLoadPath);}
    void cancelAwaitingReference(){if(!awaitingReferenceConfirmation)return;awaitingReferenceConfirmation.reset();owner->setProperty("referenceAwaitingConfirmation",false);referenceMessage(QStringLiteral("已取消待确认的参考选择，未加入样本。"));}
    void confirmReferenceSource(){
        ++referenceEpoch;referenceModelNeedsSave=false;
        if(!referenceMechanical->isChecked()){scheduledLibraryPath.clear();cancelAwaitingReference();return;}
        maybeLoadLocalReferences();
        if(awaitingReferenceConfirmation){const auto selected=*awaitingReferenceConfirmation;awaitingReferenceConfirmation.reset();owner->setProperty("referenceAwaitingConfirmation",false);addReference(selected.path,selected.label);}
    }
    void referenceMessage(const QString&s){referenceStatus->setText(s);referenceStatus->setToolTip(s);owner->setProperty("referenceLibraryStatus",s);}
    void clearReferenceAssessment(){
        if(!referenceValidation.isEmpty())referenceMessage(QStringLiteral("当前模型已切换，请重新训练后查看对应复核报告。"));
        referenceValidation.clear();referenceValidationModelDigest.clear();referenceValidationSummary.clear();
    }
    void refreshReferences(){
        std::array<unsigned,4>counts{};referenceList->clear();
        QDir sourceRoot(QFileInfo(libraryPath).absolutePath());sourceRoot.cdUp();
        for(std::size_t i=0;i<references.size();++i){const auto&r=references[i];++counts[reference_label_index(r.label)];auto*item=new QTreeWidgetItem(referenceList,{recordLabelText([&]{RecordLabel v;recordLabelFromKey(q(r.label),&v);return v;}()),referenceSourceName(sourceRoot.absolutePath(),r.path),QString::number(r.windows.size()),r.session_id.empty()?QStringLiteral("需核实"):QStringLiteral("已记录")});item->setData(0,Qt::UserRole,int(i));item->setToolTip(1,q(r.path)+"\nSHA256 "+q(r.capture_sha256)+"\n"+QStringLiteral("会话：")+(r.session_id.empty()?QStringLiteral("旧格式未记录；不能推定独立试验"):q(r.session_id)));item->setToolTip(2,QStringLiteral("从完整BIN重新提取的监督训练窗口；不使用转速筛选。"));}
        referenceCounts->setText(QStringLiteral("正常 %1 · 松动 %2 · 不对中 %3 · 不平衡 %4 个BIN").arg(counts[0]).arg(counts[3]).arg(counts[2]).arg(counts[1]));
        if(libraryNeedsLoad)referenceCounts->setText(QStringLiteral("已有本地参考库 · 确认本批来源后自动核验，再显示各类样本数量"));
        bool complete=true;for(unsigned n:counts)complete=complete&&n>0;referenceBuild->setEnabled(complete&&!referenceTask.valid()&&!task.valid()&&referenceQueue.empty());referenceRemove->setEnabled(!referenceTask.valid());owner->setProperty("referenceFileCount",int(references.size()));owner->setProperty("referenceNormalCount",int(counts[0]));owner->setProperty("referenceLoosenessCount",int(counts[3]));owner->setProperty("referenceMisalignmentCount",int(counts[2]));owner->setProperty("referenceImbalanceCount",int(counts[1]));
    }
    QByteArray referenceManifest()const{
        QJsonArray entries;for(const auto&s:references){const auto&w=s.windows.front();entries.append(QJsonObject{{"path",q(s.path)},{"label",q(s.label)},{"capture_sha256",q(s.capture_sha256)},{"session_id",q(s.session_id)},{"operator_updated_at",q(s.operator_updated_at)},{"configuration",q(w.configuration)},{"setup_id",q(w.setup_id)},{"quality_valid_windows",int(s.windows.size())}});}
        const auto bytes=QJsonDocument(QJsonObject{{"schema","vibration.supervised-library.v1"},{"purpose","operator_labeled_full_capture_supervised_training"},{"entries",entries}}).toJson(QJsonDocument::Indented);if(bytes.size()>1024*1024)throw AnalysisError("参考库超过1MiB限制");return bytes;
    }
    bool persistReferences(const QString&path){
        try{if(libraryNeedsLoad)throw AnalysisError("请先加载并核验已有本地参考库，防止覆盖尚未加载的条目");if(path.isEmpty())throw AnalysisError("参考库保存目录不可用");const auto purpose=QFileInfo(path).absoluteFilePath()==QFileInfo(libraryPath).absoluteFilePath()?CandidateWritePurpose::ActiveLibrary:CandidateWritePurpose::Export;saveModelAtomic(path,referenceManifest(),protectedPaths(purpose));if(QFileInfo(path).absoluteFilePath()==QFileInfo(libraryPath).absoluteFilePath())libraryLoadPath=libraryPath;return true;}catch(const std::exception&e){referenceMessage(QStringLiteral("参考库保存失败：")+QString::fromUtf8(e.what()));return false;}
    }
    bool referenceReady(){
        if(!referenceMechanical->isChecked()){referenceMessage(QStringLiteral("请先确认本批参考来自同一机械台架。来源确认与工况标签分别核验，标签不能证明机械来源。"));return false;}
        if(backend->currentIndex()==0){referenceMessage(QStringLiteral("计算方式不可用，请检查高级设置。"));return false;}return true;
    }
    static SupervisedSample readReference(const ReferenceRequest&r,const AnalysisConfig&cfg,const std::string&setupId,accelerator::Backend mode){
        const QFileInfo file(r.path);if(!file.isFile()||file.isSymLink()||file.suffix().compare("BIN",Qt::CaseInsensitive))throw AnalysisError("参考来源必须是已保存的普通BIN文件");RecordLabelAnnotation annotation;QString error;if(!loadRecordLabel(file.absoluteFilePath(),&annotation,&error))throw AnalysisError(error.toUtf8().toStdString());if(!annotation.exists||annotation.label==RecordLabel::Unlabeled||recordLabelKey(annotation.label)!=r.label)throw AnalysisError("参考文件的人工标签缺失或已变化，请先在SD记录中确认并保存工况标签");if(annotation.synthetic)throw AnalysisError("合成记录不能进入机械参考库");
        auto capture=load_capture(file.absoluteFilePath().toUtf8().toStdString());auto sample=extract_supervised_sample(capture,r.label.toUtf8().toStdString(),cfg,setupId,mode);sample.operator_updated_at=annotation.updatedAt.toUtf8().toStdString();if(sample.session_id.empty())sample.session_id=annotation.sessionId.toUtf8().toStdString();
        if(!r.hash.isEmpty()&&q(sample.capture_sha256)!=r.hash)throw AnalysisError("参考BIN哈希已变化，拒绝加载原参考条目");if(!r.configuration.isEmpty()&&q(sample.windows.front().configuration)!=r.configuration)throw AnalysisError("训练库与当前采样或换算配置不一致");if(!r.setup.isEmpty()&&q(sample.windows.front().setup_id)!=r.setup)throw AnalysisError("训练库的台架安装标识不一致");if(!r.updated.isEmpty()&&annotation.updatedAt!=r.updated)throw AnalysisError("参考文件人工标签历史已变化，请重新确认并加入");if(!r.session.isEmpty()&&q(sample.session_id)!=r.session)throw AnalysisError("参考采集会话身份已变化");return sample;
    }
    void startReference(){
        if(referenceTask.valid()||task.valid()||referenceQueue.empty())return;
        if(!referenceReady()){referenceQueue.clear();return;}
        if(libraryNeedsLoad){loadReferences(libraryLoadPath,true);return;}
        const auto request=referenceQueue.front();referenceQueue.pop_front();const auto generation=referenceEpoch;auto cfg=config;cfg.signal_kind=SignalKind::VibrationConfirmed;const auto setupId=setup->text().trimmed().toUtf8().toStdString();const auto mode=backend->currentIndex()==1?accelerator::Backend::Hardware:accelerator::Backend::Reference;referenceMessage(QStringLiteral("后台核验并提取：%1").arg(QFileInfo(request.path).fileName()));
        referenceTask=std::async(std::launch::async,[request,cfg,setupId,mode,generation]{ReferenceTask t;t.generation=generation;try{t.sample=readReference(request,cfg,setupId,mode);}catch(const std::exception&e){t.error=QString::fromUtf8(e.what());}return t;});refreshReferences();
    }
    bool addReference(const QString&path,const QString&labelKey){
        RecordLabel ignored;if(!recordLabelFromKey(labelKey,&ignored)||ignored==RecordLabel::Unlabeled){referenceMessage(QStringLiteral("请先为该BIN保存正常、松动、不对中或不平衡人工标签。"));return false;}
        const QFileInfo info(path);
        if(!referenceMechanical->isChecked()){
            if(referenceDialog->isVisible()&&info.isFile()&&!info.isSymLink()&&!info.suffix().compare("BIN",Qt::CaseInsensitive)){
                awaitingReferenceConfirmation=ReferenceRequest{info.canonicalFilePath(),labelKey,{},{},{},{},{}};owner->setProperty("referenceAwaitingConfirmation",true);
                referenceMessage(QStringLiteral("已保留所选记录 %1；确认本批机械来源后自动核验并加入，关闭此窗口即取消。").arg(info.fileName()));return true;
            }
            return referenceReady();
        }
        if(!referenceReady())return false;if(references.size()+referenceQueue.size()>=reference_max_files){referenceMessage(QStringLiteral("参考库最多256个BIN，请移出不再使用的参考。"));return false;}
        if(!info.isFile()||info.isSymLink()||info.suffix().compare("BIN",Qt::CaseInsensitive)){referenceMessage(QStringLiteral("只能加入已保存的普通BIN文件。"));return false;}const auto absolute=info.canonicalFilePath();for(const auto&s:references)if(q(s.path)==absolute){referenceMessage(QStringLiteral("该BIN已在参考库中；修改标签时请先移除旧条目。"));return false;}if(libraryNeedsLoad&&!referenceTask.valid()){loadReferences(libraryLoadPath,true);referenceQueue.push_back({absolute,labelKey,{},{},{},{},{}});return true;}referenceQueue.push_back({absolute,labelKey,{},{},{},{},{}});startReference();return true;
    }
    void loadReferences(const QString&path,bool keepQueued=false){
        if(!referenceReady()||referenceTask.valid()||(!keepQueued&&!referenceQueue.empty()))return;if(task.valid()){scheduledLibraryPath=path;scheduledLibraryKeepsQueue=keepQueued;return;}scheduledLibraryPath.clear();const auto generation=referenceEpoch;auto cfg=config;cfg.signal_kind=SignalKind::VibrationConfirmed;const auto setupId=setup->text().trimmed().toUtf8().toStdString();const auto mode=backend->currentIndex()==1?accelerator::Backend::Hardware:accelerator::Backend::Reference;referenceMessage(QStringLiteral("后台重新核验参考库来源、标签和特征…"));
        if(!referenceLibrarySources.contains(path))referenceLibrarySources.push_back(path);
        referenceTask=std::async(std::launch::async,[path,cfg,setupId,mode,generation]{ReferenceTask t;t.generation=generation;t.loading=true;try{QFile f(path);if(!f.open(QIODevice::ReadOnly)||f.size()>1024*1024)throw AnalysisError("参考库无法读取或超过1MiB");QJsonParseError error;const auto doc=QJsonDocument::fromJson(f.readAll(),&error);const auto obj=doc.object();if(error.error!=QJsonParseError::NoError||(obj.value("schema")!="vibration.reference-library.v1"&&obj.value("schema")!="vibration.supervised-library.v1")||!obj.value("entries").isArray())throw AnalysisError("参考库格式无效");const auto entries=obj.value("entries").toArray();if(entries.size()>int(reference_max_files))throw AnalysisError("参考库条目过多");std::set<std::string>hashes,paths;std::map<std::string,std::string>sessionLabels;for(const auto&item:entries){if(!item.isObject())throw AnalysisError("参考库条目无效");const auto x=item.toObject();ReferenceRequest r{x.value("path").toString(),x.value("label").toString(),x.value("capture_sha256").toString(),x.value("configuration").toString(),x.value("setup_id").toString(),x.value("session_id").toString(),x.value("operator_updated_at").toString()};if(r.hash.size()!=64||r.configuration.isEmpty()||r.setup.isEmpty()||r.updated.isEmpty())throw AnalysisError("参考库来源或配置字段缺失");if(obj.value("schema")=="vibration.reference-library.v1")r.configuration.clear();auto s=readReference(r,cfg,setupId,mode);if(!hashes.insert(s.capture_sha256).second||!paths.insert(s.path).second)throw AnalysisError("训练库包含重复BIN或相同内容副本");if(!s.session_id.empty()){const auto prior=sessionLabels.emplace(s.session_id,s.label);if(!prior.second&&prior.first->second!=s.label)throw AnalysisError("同一次采集含冲突工况标签");}t.loaded.push_back(std::move(s));}}catch(const std::exception&e){t.error=QString::fromUtf8(e.what());}return t;});refreshReferences();
    }
    void buildReferences(){
        if(referenceTask.valid()||task.valid()||!referenceQueue.empty()||!referenceReady())return;
        const auto cached=references;const auto generation=referenceEpoch;auto cfg=config;cfg.signal_kind=SignalKind::VibrationConfirmed;const auto setupId=setup->text().trimmed().toUtf8().toStdString();const auto mode=backend->currentIndex()==1?accelerator::Backend::Hardware:accelerator::Backend::Reference;
        referenceMessage(QStringLiteral("后台重新核验完整记录，提取特征并训练四类模型…"));
        referenceTask=std::async(std::launch::async,[cached,generation,cfg,setupId,mode]{ReferenceTask t;t.generation=generation;try{
            std::vector<SupervisedSample>verified;for(const auto&s:cached){const auto&w=s.windows.front();ReferenceRequest r{q(s.path),q(s.label),q(s.capture_sha256),q(w.configuration),q(w.setup_id),q(s.session_id),q(s.operator_updated_at)};verified.push_back(readReference(r,cfg,setupId,mode));}
            const SupervisedTrainOptions options;auto fitted=fit_supervised_model(verified,"four_class_supervised",options);const auto validation=evaluate_supervised_models(verified,options);
            auto report=QJsonDocument::fromJson(QByteArray::fromStdString(supervised_validation_json(validation))).object();report.insert("deployment_model_sha256",q(modelDigest(fitted)));report.insert("deployment_model_kind",q(fitted.kind));report.insert("binding_policy","valid_only_when_deployment_model_sha256_matches_actual_model_bytes");
            t.validation=QJsonDocument(report).toJson(QJsonDocument::Indented);t.validationSummary=validation.metrics_available?QStringLiteral("训练完成，已生成按采集会话留出的复核报告，见详情。"):QStringLiteral("训练完成；独立样本不足，复核指标暂不可用，见详情。");t.model=std::move(fitted);
        }catch(const std::exception&e){t.error=QString::fromUtf8(e.what());}return t;});refreshReferences();
    }
    void pollReferences(){
        if(referenceTask.valid()&&referenceTask.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            auto t=referenceTask.get();
            if(t.generation!=referenceEpoch){referenceQueue.clear();referenceMessage(QStringLiteral("设置、来源确认或当前模型已改变，旧参考任务已取消，请重新操作。"));}
            else if(!t.error.isEmpty()){if(t.loading)referenceQueue.clear();referenceMessage(QStringLiteral("未加入 / 未建立：")+t.error);}
            else if((t.sample||t.loading||t.model)&&!referenceMechanical->isChecked()){referenceQueue.clear();referenceMessage(QStringLiteral("参考来源确认已取消，未加入或替换参考库/模型。"));}
            else if(t.sample){bool duplicate=false,conflictingSession=false;for(const auto&r:references){if(r.path==t.sample->path||r.capture_sha256==t.sample->capture_sha256)duplicate=true;if(!r.session_id.empty()&&r.session_id==t.sample->session_id&&r.label!=t.sample->label)conflictingSession=true;}if(conflictingSession)referenceMessage(QStringLiteral("未加入：同一次采集不能有冲突工况标签。"));else if(duplicate)referenceMessage(QStringLiteral("未加入：重复BIN或相同内容副本。"));else if(references.size()>=reference_max_files)referenceMessage(QStringLiteral("参考库已达到256个BIN上限。"));else{references.push_back(std::move(*t.sample));if(persistReferences(libraryPath))referenceMessage(QStringLiteral("已加入并保存。同次采集的分段按同一会话训练和验证，不重复计作独立试验。"));}}
            else if(t.loading){references=std::move(t.loaded);libraryNeedsLoad=false;if(persistReferences(libraryPath))referenceMessage(QStringLiteral("已从完整记录重新提取训练特征；旧参考库和旧模型保留。"));}
            else if(t.model){trainedModel=std::move(t.model);referenceValidation=std::move(t.validation);referenceValidationModelDigest=modelDigest(*trainedModel);referenceValidationSummary=t.validationSummary;supervisedSaved=false;supervisedSaveError.clear();referenceModelNeedsSave=true;referenceMessage(referenceValidationSummary);}
            refreshReferences();render();startReference();
        }
        referenceBuild->setEnabled(!libraryNeedsLoad&&!referenceTask.valid()&&!task.valid()&&referenceQueue.empty()&&owner->property("referenceNormalCount").toInt()>0&&owner->property("referenceLoosenessCount").toInt()>0&&owner->property("referenceMisalignmentCount").toInt()>0&&owner->property("referenceImbalanceCount").toInt()>0);
        if(referenceModelNeedsSave&&!task.valid()){referenceModelNeedsSave=false;saveSupervisedModel();}
        else if(!referenceTask.valid()&&!task.valid())nextTask();
    }
    QString speedDomainDetails()const{
        if(!window||!model)return {};
        if(model->kind=="reference_ensemble_v3")return QStringLiteral("当前实测 %1 rpm（本窗 %2–%3 rpm）；已按实测转速换算阶次特征并比较最近参考，参考转速范围不是检测硬限制。").arg(window->rpm,0,'f',1).arg(window->rpm_min,0,'f',1).arg(window->rpm_max,0,'f',1);
        if(model->kind=="reference_exemplars"||model->kind=="reference_ensemble_v2")return QStringLiteral("当前窗 %1–%2 rpm；参考库总包络 %3–%4 rpm（中间可能缺类或缺转速，逐窗另行检查覆盖）。").arg(window->rpm_min,0,'f',1).arg(window->rpm_max,0,'f',1).arg(model->rpm_min,0,'f',1).arg(model->rpm_max,0,'f',1);
        return QStringLiteral("当前窗有效测速均值 %1 rpm（窗内 %2–%3 rpm）；模型适用转速 %4–%5 rpm。")
            .arg(window->rpm,0,'f',1).arg(window->rpm_min,0,'f',1).arg(window->rpm_max,0,'f',1)
            .arg(model->rpm_min,0,'f',1).arg(model->rpm_max,0,'f',1);
    }
    bool inputSynthetic()const{return live?liveSynthetic:capture&&capture->source_metadata.synthetic;}
    void refreshDashboard(){
        const bool supervised=!legacyEnabled->isChecked();
        const bool available=supervised&&trainedWindow&&trainedWindow->valid&&!trainedWindow->synthetic&&!resultLabel(trainedInference.classification).isEmpty();
        const bool replay=available&&trainedInference.training_replay;
        const auto displayRange=trainedWindow&&trainedWindow->input_kind=="local_full_raw_crc_continuous"?(liveHeader.stream_segment?liveHeader.adc_range_uv:0):capture?capture->header.adc_range_uv:0;
        signalSummary->setWindow(supervised&&trainedWindow?&*trainedWindow:nullptr,displayRange);
        const auto source=supervised&&trainedWindow?q(trainedWindow->source_path):window?q(window->source_path):capture?q(capture->source_path):QString();
        QString sourceText;if(live)sourceText=liveSynthetic?QStringLiteral("测试输入 · 合成信号"):mechanical->isChecked()?QStringLiteral("实时输入 · 已确认机械来源"):QStringLiteral("实时输入 · 来源尚未确认");else if(!source.isEmpty()){const QFileInfo file(source);const auto directory=file.dir().dirName();sourceText=QStringLiteral("记录 · ")+(directory=="."?QString():directory+" / ")+file.fileName();}else sourceText=QStringLiteral("输入 · 等待采集或打开记录");
        signalSource->setText(signalSource->fontMetrics().elidedText(sourceText,Qt::ElideMiddle,std::max(150,signalSource->width())));signalSource->setToolTip(sourceText);
        scoreNote->setText(available?QStringLiteral("模型得分用于比较，不是准确率"):!supervised?QStringLiteral("旧模型兼容 · 详情中查看"):trainedModel?QStringLiteral("等待有效输入后显示得分"):QStringLiteral("训练四类模型后显示得分"));
        scoreNote->setToolTip(replay?QStringLiteral("当前为训练文件或同次采集回放，不是独立验证。"):QString());
        for(unsigned cls=0;cls<4;++cls){const double value=available?trainedInference.scores[cls]:0;scoreBars[cls]->setValue(int(std::lround(std::clamp(value,0.,100.)*10)));scoreBars[cls]->setProperty("scoreAvailable",available);scoreBars[cls]->setProperty("modelScore",available?QVariant(value):QVariant());scoreValues[cls]->setText(available?QString::number(value,'f',1):QStringLiteral("—"));}
    }
    void sourceHint(bool synthetic){mechanical->setEnabled(!synthetic);normal->setEnabled(!synthetic);if(!synthetic)return;{QSignalBlocker block(mechanical);mechanical->setChecked(false);}state->setStyleSheet("font-size:26px;font-weight:600;");state->setText(QStringLiteral("等待有效机械数据"));documents[0]->setHtml(html(p(QStringLiteral("当前为合成数据，仅用于软件演示；请采集机械数据后检测。"))));}
    void invalidatePublished(){window.reset();trainedWindow.reset();trainedInference={};inference={};inference.status="warming_up";owner->setProperty("candidateStatus","warming_up");owner->setProperty("candidateClassification","unknown");owner->setProperty("candidateSource",QString());owner->setProperty("candidateReferenceReplay",false);state->setStyleSheet("font-size:26px;font-weight:600;");for(auto*b:documents)b->setHtml(html(p(QStringLiteral("等待新的完整采集记录。"))));refreshDashboard();}
    double ageSeconds()const{if(!live)return 0;const auto last=!legacyEnabled->isChecked()&&trainedWindow?trainedWindow->analysis_last_index:window?window->analysis_last_index:0;const auto rate=!legacyEnabled->isChecked()&&trainedWindow?trainedWindow->sample_rate_hz:window?window->sample_rate_hz:0;if(!rate)return 0;const double behind=observedEnd>last?double(observedEnd-last)/rate:0;return behind+(lastRawMs?std::max<qint64>(0,QDateTime::currentMSecsSinceEpoch()-lastRawMs)/1000.:0);}
    void updateAge(){
        if(!legacyEnabled->isChecked()){updateSupervisedAge();return;}
        if(!window)return;const auto age=ageSeconds();const bool historical=live&&age>3;
        const QString compute=window->source.find("pl_hardware")==0?QStringLiteral("FPGA计算"):QStringLiteral("ARM计算");
        const QString input=window->input_kind=="local_full_raw_crc_continuous"?QStringLiteral("连续原码"):QStringLiteral("SD回放");
        const QString speed=window->fresh_periods&&window->rpm>0?QStringLiteral("%1 rpm").arg(window->rpm,0,'f',0):QStringLiteral("未获取转速");
        subtitle->setText(QStringLiteral("%1 · %2 · %3–%4 s · %5%6%7%8").arg(compute,input)
            .arg(double(window->analysis_first_index)/window->sample_rate_hz,0,'f',2)
            .arg(double(window->analysis_last_index)/window->sample_rate_hz,0,'f',2).arg(speed)
            .arg(live?QStringLiteral(" · 距当前 %1 s%2").arg(age,0,'f',1).arg(historical?QStringLiteral(" · 历史窗"):QString()):QString())
            .arg(task.valid()?QStringLiteral(" · 后台处理中"):QString())
            .arg(inference.reference_replay?QStringLiteral(" · 参考回放"):QString()));
        subtitle->setToolTip(q(window->source)+"\n"+q(window->source_path));
        if(inference.status=="provisional_candidate"&&!resultLabel(inference.classification).isEmpty())state->setText(resultLabel(inference.classification));
        owner->setProperty("candidateHistorical",historical);owner->setProperty("candidateAgeSeconds",age);
    }
    std::vector<std::string>protectedPaths(CandidateWritePurpose purpose=CandidateWritePurpose::Export)const{
        QStringList sources;if(capture&&!capture->source_path.empty())sources.push_back(q(capture->source_path));for(const auto&r:references)sources.push_back(q(r.path));
        auto configurations=referenceLibrarySources;if(!modelPath.isEmpty())configurations.push_back(modelPath);
        for(const auto&path:{supervisedPath,supervisedValidationPath,supervisedSourcePath,legacyLibraryPath})if(!path.isEmpty())configurations.push_back(path);
        return candidate_protected_output_paths(sources,configurations,storage,libraryPath,purpose);
    }
    std::vector<std::string>supervisedProtectedPaths(const QString&target)const{
        QStringList sources;if(capture&&!capture->source_path.empty())sources.push_back(q(capture->source_path));for(const auto&r:references)sources.push_back(q(r.path));
        auto configurations=referenceLibrarySources;for(const auto&path:{storage,modelPath,supervisedPath,supervisedValidationPath,supervisedSourcePath,legacyLibraryPath})if(!path.isEmpty()&&QFileInfo(path).absoluteFilePath()!=QFileInfo(target).absoluteFilePath())configurations.push_back(path);
        return candidate_protected_output_paths(sources,configurations,target,libraryPath,CandidateWritePurpose::ActiveModel);
    }
    void saveSupervisedModel(){
        if(!trainedModel||task.valid())return;supervisedSaved=false;supervisedSaveError.clear();owner->setProperty("candidateModelSaved",false);
        const auto path=supervisedPath;const auto bytes=QByteArray::fromStdString(supervised_model_text(*trainedModel));const auto protection=supervisedProtectedPaths(path);const auto generation=epoch;
        const auto validation=referenceValidationModelDigest==modelDigest(*trainedModel)?referenceValidation:QByteArray();const auto validationPath=supervisedValidationPath;const auto validationProtection=supervisedProtectedPaths(validationPath);
        task=std::async(std::launch::async,[path,bytes,protection,generation,validation,validationPath,validationProtection]{Task t;t.epoch=generation;t.saveSupervised=true;try{if(path.isEmpty())throw AnalysisError("模型保存目录不可用");if(!validation.isEmpty())saveModelAtomic(validationPath,validation,validationProtection);saveModelAtomic(path,bytes,protection);}catch(const std::exception&e){t.error=e.what();}return t;});render();
    }
    void saveModel(){
        if(!model||task.valid())return;modelSaved=false;modelSaveError.clear();owner->setProperty("candidateModelSaved",false);
        const auto path=storage;const auto bytes=QByteArray::fromStdString(candidate_model_text(*model));const auto protection=protectedPaths(CandidateWritePurpose::ActiveModel);const auto generation=epoch;
        const auto validation=referenceValidationModelDigest==modelDigest(*model)?referenceValidation:QByteArray();
        const auto validationPath=QFileInfo(path).absolutePath()+"/reference-validation.json";
        const auto validationProtection=protectedPaths(CandidateWritePurpose::ActiveValidation);
        task=std::async(std::launch::async,[path,bytes,protection,generation,validation,validationPath,validationProtection]{Task t;t.epoch=generation;t.saveOnly=true;try{if(path.isEmpty())throw AnalysisError("模型保存目录不可用");if(!validation.isEmpty())saveModelAtomic(validationPath,validation,validationProtection);saveModelAtomic(path,bytes,protection);}catch(const std::exception&e){t.error=e.what();}return t;});
        render();
    }
    void renderSupervised(){
        baseline->setEnabled(false);features->setEnabled(bool(capture)&&!task.valid());save->setEnabled(bool(trainedModel)&&!task.valid());load->setEnabled(!task.valid()&&!referenceTask.valid());
        owner->setProperty("candidateModelSaved",supervisedSaved);owner->setProperty("candidateModelKind",trainedModel?q(trainedModel->kind):QString());owner->setProperty("candidateFourClassModelReady",bool(trainedModel));owner->setProperty("candidateLegacyCompatibilityEnabled",false);owner->setProperty("normalBaselineCount",0);
        profile->setText(trainedModel?QStringLiteral("独立训练采集：正常 %1 · 松动 %2 · 不对中 %3 · 不平衡 %4%5").arg(trainedModel->independent_session_counts[0]).arg(trainedModel->independent_session_counts[3]).arg(trainedModel->independent_session_counts[2]).arg(trainedModel->independent_session_counts[1]).arg(supervisedSaved?QStringLiteral(" · 已保存"):QStringLiteral(" · 未保存")):model?QStringLiteral("历史参考模型已保留"):QString());if(!supervisedSaveError.isEmpty())profile->setText(profile->text()+QStringLiteral(" · 模型保存失败：")+supervisedSaveError);
        if(!trainedWindow){state->setStyleSheet("font-size:26px;font-weight:600;");state->setText(trainedModel?QStringLiteral("等待有效数据"):QStringLiteral("先训练四类模型"));if(!trainedModel)documents[0]->setHtml(html(p(QStringLiteral("采集并标注四种工况，在参考样本库训练模型。"))));if(inputSynthetic())sourceHint(true);refreshDashboard();return;}
        if(trainedModel)trainedInference=infer_supervised(*trainedWindow,*trainedModel);else{trainedInference={};trainedInference.status=trainedWindow->valid?"no_model":"waiting_for_data";trainedInference.reasons=trainedWindow->reasons;}
        const auto result=resultLabel(trainedInference.classification);const bool available=!result.isEmpty()&&trainedWindow->valid&&!trainedWindow->synthetic;
        state->setStyleSheet(available?"font-size:48px;font-weight:600;":"font-size:26px;font-weight:600;");state->setText(available?result:trainedWindow->synthetic?QStringLiteral("等待有效机械数据"):trainedModel?QStringLiteral("等待有效数据"):QStringLiteral("先训练四类模型"));
        documents[0]->setHtml(html(p(available?QStringLiteral("本次检测结果已更新。"):trainedWindow->synthetic?QStringLiteral("当前为合成数据，仅用于软件演示；请采集机械数据后检测。"):!trainedModel?QStringLiteral("采集并标注四种工况，在参考样本库训练模型。"):QStringLiteral("请提供完整的机械采集记录，具体原因可查看详情。"))));
        QString details;if(trainedModel){details+=p(QStringLiteral("已训练的四类模型：%1；模型得分不是诊断准确率。").arg(q(trainedModel->kind)));if(available){details+="<table width='98%'><tr><th>正常</th><th>不平衡</th><th>不对中</th><th>松动</th></tr><tr>";for(double score:trainedInference.scores)details+=QStringLiteral("<td align='center'>%1</td>").arg(score,0,'g',5);details+="</tr></table>";}details+=p(QStringLiteral("训练窗 %1 · 分类特征 %2 · 训练警告窗 %3。").arg(trainedModel->training_windows).arg(int(supervised_active_feature_indices(trainedModel->options).size())).arg(trainedModel->training_warning_windows));}
        if(trainedInference.training_replay)details+=p(QStringLiteral("当前为训练文件或同次采集回放；仅用于复核流程，不是独立验证。"));
        for(const auto&item:trainedInference.reasons)details+=p(reason(item));for(const auto&item:trainedWindow->warnings)details+=p(reason(item));for(const auto&item:trainedInference.evidence)details+=p(q(item));
        if(trainedWindow->valid)details+=p(QStringLiteral("当前窗口可分析；转速、PPR和测速状态不参与分类输入或筛选。"));
        if(!referenceValidation.isEmpty()&&trainedModel&&referenceValidationModelDigest==modelDigest(*trainedModel)){const auto report=QJsonDocument::fromJson(referenceValidation).object();details+=p(report.value("metrics_available").toBool()?QStringLiteral("按完整采集会话留出复核：%1个会话，宏F1 %2；范围限于同台架内部验证。").arg(report.value("heldout_sessions").toInt()).arg(report.value("metrics").toObject().value("macro_f1").toDouble(),0,'f',3):QStringLiteral("独立采集数量不足，本次未输出验证指标。"));details+=p(QStringLiteral("完整复核报告：")+supervisedValidationPath);}
        details+=p(q(trainedWindow->source))+p(q(trainedWindow->source_path));documents[2]->setHtml(html(details));
        const auto&featureNames=supervised_feature_names();const auto active=trainedModel?supervised_active_feature_indices(trainedModel->options):std::vector<unsigned>{};QString featureText=p(QStringLiteral("%1 · 原始信号的时域、频域、小波包与通道关系特征；不含转速或人工标签。").arg(q(trainedWindow->feature_schema)))+"<table width='98%'><tr><th>特征</th><th>用于分类</th><th>数值</th></tr>";for(std::size_t i=0;i<supervised_feature_count;++i)featureText+="<tr><td>"+q(featureNames[i]).toHtmlEscaped()+"</td><td>"+(std::find(active.begin(),active.end(),unsigned(i))!=active.end()?QStringLiteral("是"):QStringLiteral("—"))+QStringLiteral("</td><td>%1</td></tr>").arg(trainedWindow->features[i],0,'g',6);documents[1]->setHtml(html(featureText+"</table>"));
        owner->setProperty("candidateStatus",q(trainedInference.status));owner->setProperty("candidateClassification",q(trainedInference.classification));owner->setProperty("candidateSource",q(trainedWindow->source));owner->setProperty("candidateReferenceReplay",trainedInference.training_replay);refreshDashboard();updateSupervisedAge();
    }
    void updateSupervisedAge(){
        if(!trainedWindow)return;const auto age=ageSeconds();const bool historical=live&&age>3;const auto&current=*trainedWindow;
        subtitle->setText(QStringLiteral("%1 · %2 · %3–%4 s%5%6%7").arg(current.source.find("pl_hardware")==0?QStringLiteral("FPGA计算"):QStringLiteral("ARM计算"),current.input_kind=="local_full_raw_crc_continuous"?QStringLiteral("连续原码"):QStringLiteral("SD记录"))
            .arg(double(current.analysis_first_index)/current.sample_rate_hz,0,'f',2).arg(double(current.analysis_last_index)/current.sample_rate_hz,0,'f',2)
            .arg(live?QStringLiteral(" · 距当前 %1 s%2").arg(age,0,'f',1).arg(historical?QStringLiteral(" · 历史窗"):QString()):QString()).arg(task.valid()?QStringLiteral(" · 后台处理中"):QString()).arg(trainedInference.training_replay?QStringLiteral(" · 训练回放"):QString()));
        subtitle->setToolTip(q(current.source)+"\n"+q(current.source_path));owner->setProperty("candidateHistorical",historical);owner->setProperty("candidateAgeSeconds",age);
    }
    void render(){
        if(!legacyEnabled->isChecked()){renderSupervised();return;}
        refreshDashboard();
        baseline->setEnabled(legacyEnabled->isChecked()&&!task.valid()&&!referenceTask.valid());features->setEnabled(bool(capture)&&!task.valid());save->setEnabled(bool(model)&&!task.valid());
        load->setEnabled(!task.valid()&&!referenceTask.valid());
        profile->setText(model?QStringLiteral("%1：%2窗 · 模型适用 %3–%4 rpm").arg(model->kind=="engineering_prior"?QStringLiteral("旧正常基线（兼容）"):QStringLiteral("线性模型正常样本")).arg(model->baseline_count).arg(model->rpm_min,0,'f',1).arg(model->rpm_max,0,'f',1):QString());
        profile->setToolTip(model?q(model->id)+" · "+q(model->kind)+" · "+q(model->status)+"\n"+q(model->schema)+QStringLiteral("\n模型适用转速是基线工况的判别范围，不是台架机械最高转速。")+(model->kind=="engineering_prior"?QStringLiteral("\n本地正常基线范围由正常窗最低转速×0.9至最高转速×1.1确定；整个检测窗必须落在范围内。"):QString()):QString());
        if(model&&model->kind=="engineering_prior")profile->setText(profile->text()+QStringLiteral(" · 尚非四类参考库模型"));
        if(model&&!referenceKind(model->kind)&&!legacyEnabled->isChecked())profile->setText(QStringLiteral("旧模型已保留"));
        if(model)profile->setText(profile->text()+(modelSaved?QStringLiteral(" · 已保存"):QStringLiteral(" · 未保存")));
        if(model&&referenceKind(model->kind))profile->setText(QStringLiteral("四类参考 · 正常%1/松动%2/不对中%3/不平衡%4 · %5–%6 rpm（总范围） · 待盲测%7").arg(model->reference_file_counts[0],0,'f',0).arg(model->reference_file_counts[3],0,'f',0).arg(model->reference_file_counts[2],0,'f',0).arg(model->reference_file_counts[1],0,'f',0).arg(model->rpm_min,0,'f',1).arg(model->rpm_max,0,'f',1).arg(modelSaved?QStringLiteral(" · 已保存"):QStringLiteral(" · 未保存")));
        if(model&&model->kind=="reference_ensemble_v3")profile->setText(QStringLiteral("正常 %1 · 松动 %2 · 不对中 %3 · 不平衡 %4 次独立采集%5").arg(model->reference_file_counts[0],0,'f',0).arg(model->reference_file_counts[3],0,'f',0).arg(model->reference_file_counts[2],0,'f',0).arg(model->reference_file_counts[1],0,'f',0).arg(modelSaved?QStringLiteral(" · 已保存"):QStringLiteral(" · 未保存")));
        if(model&&!legacyEnabled->isChecked())profile->setText(QStringLiteral("历史模型已保留"));
        if(!modelSaveError.isEmpty())profile->setText(profile->text()+QStringLiteral(" · ")+modelSaveError);
        owner->setProperty("candidateModelSaved",modelSaved);
        owner->setProperty("candidateModelKind",model?q(model->kind):QString());
        owner->setProperty("candidateFourClassModelReady",hasFourClassModel());
        owner->setProperty("candidateLegacyCompatibilityEnabled",legacyEnabled->isChecked());
        if(!window){state->setStyleSheet("font-size:26px;font-weight:600;");state->setText(hasDetectionModel()?QStringLiteral("等待有效数据"):QStringLiteral("先训练四类模型"));if(inputSynthetic())sourceHint(true);return;}
        if(hasDetectionModel())inference=infer_candidate(*window,*model);else{inference={};inference.source=window->source;inference.reasons=window->reasons;inference.status=window->valid?"no_model":"quality_gated";}
        const bool invalidSpeed=std::any_of(window->reasons.begin(),window->reasons.end(),[](const std::string&r){return r=="fewer_than_8_fresh_speed_periods"||r=="speed_valid_fraction_below_90pct"||r=="speed_cv_above_5pct"||r=="ppr_missing";});
        const bool speedTolerant=model&&model->kind=="reference_ensemble_v3";
        const bool resultAvailable=inference.status=="provisional_candidate"&&!resultLabel(inference.classification).isEmpty();
        state->setStyleSheet(resultAvailable?"font-size:48px;font-weight:600;":"font-size:26px;font-weight:600;");
        state->setText(resultAvailable?resultLabel(inference.classification):!hasDetectionModel()?QStringLiteral("先训练四类模型"):QStringLiteral("等待有效数据"));
        updateAge();
        QString e;
        owner->setProperty("candidateReferenceReplay",inference.reference_replay);if(inference.reference_replay)e+=p(QStringLiteral("参考数据回放 / 同次采集 · 只检查参考匹配流程，不属于独立验证或实测准确率。"));
        const auto coverage=hasFourClassModel()?candidate_reference_coverage(*window,*model):std::array<unsigned,4>{};
        const bool scored=hasDetectionModel()&&(inference.status=="provisional_candidate"||inference.status=="rejected_unknown");
        if(window->synthetic){state->setText(QStringLiteral("等待有效机械数据"));e+=p(QStringLiteral("这份记录为合成数据，只用于软件验证，不能用于机械识别或参考建库。"));}
        else if(!hasDetectionModel()){
            e+=p(QStringLiteral("采集并标注四种工况，在参考样本库训练模型。"));
            for(std::size_t i=0;i<std::min<std::size_t>(1,inference.reasons.size());++i)e+=p(reason(inference.reasons[i]));
        }
        else if(scored){if(speedTolerant)e+=p(inference.speed_support_status=="extrapolated"?QStringLiteral("跨转速参考 · 待验证"):QStringLiteral("近转速参考"));if(speedTolerant&&inference.shape_support_status=="single_reference_mode")e+=p(QStringLiteral("单一参考模式匹配 · 待独立实测确认。"));e+="<table width='98%'><tr><th width='25%'>正常</th><th width='25%'>不平衡</th><th width='25%'>不对中</th><th width='25%'>松动</th></tr><tr>";for(double x:inference.scores)e+=QStringLiteral("<td align='center' style='font-size:24px'>%1</td>").arg(x,0,'f',1);e+="</tr></table>";
            if(referenceKind(model->kind))e+=p(inference.status=="provisional_candidate"?QStringLiteral("与%1参考最接近。").arg(label(inference.classification)):QStringLiteral("四类参考匹配的相似度、领先差值不足或超出参考分布，保留未知。"));
            else if(inference.status=="provisional_candidate"&&inference.classification=="normal")e+=p(QStringLiteral("与本机正常基线接近，暂未出现足够的故障谱型证据。"));
            else if(inference.classification=="imbalance")e+=p(QStringLiteral("1倍转频振动较正常基线增大，呈不平衡候选谱型。"));
            else if(inference.classification=="misalignment")e+=p(QStringLiteral("2倍转频功率较正常基线增大，呈不对中候选谱型。"));
            else if(inference.classification=="looseness")e+=p(QStringLiteral("分数阶、更多谐波或冲击特征增强，呈松动候选谱型。"));
            else e+=p(QStringLiteral("候选证据不足或相互重叠，保留未知。请查看质量检查。"));
            if(model->kind=="reference_exemplars"||model->kind=="reference_ensemble_v2"){const auto counts=inference.reference_eligible_files;const auto bounds=std::minmax_element(counts.begin(),counts.end());if(*bounds.first<2||*bounds.second>4*std::max(1u,*bounds.first))e+=p(QStringLiteral("当前转速参考覆盖有限或数量不均：正常%1、松动%2、不对中%3、不平衡%4个BIN；候选结果需复核。").arg(counts[0]).arg(counts[3]).arg(counts[2]).arg(counts[1]));}e+=p(QStringLiteral("匹配分0–100，不代表故障概率。"));if(model->kind=="engineering_prior")e+=p(QStringLiteral("当前为旧正常基线兼容结果，尚非四类参考库模型；四类识别请在“参考样本库”建立 / 更新模型。"));
        }else if(inference.status=="coverage_insufficient"||inference.status=="reference_support_insufficient"||(inference.status=="outside_operating_range"&&hasFourClassModel())){
            e+=p(QStringLiteral("实测转速 %1 rpm（本窗 %2–%3 rpm）。").arg(window->rpm,0,'f',1).arg(window->rpm_min,0,'f',1).arg(window->rpm_max,0,'f',1));
            e+=p((speedTolerant?QStringLiteral("可用独立参考：正常 %1次 · 松动 %2次 · 不对中 %3次 · 不平衡 %4次；每类至少2次。"):QStringLiteral("当前转速可用独立参考：正常 %1次 · 松动 %2次 · 不对中 %3次 · 不平衡 %4次；每类至少2次。")).arg(coverage[0]).arg(coverage[3]).arg(coverage[2]).arg(coverage[1]));
            e+=p(speedTolerant?QStringLiteral("下一步：补采不足的工况并保存标签，加入参考样本库后建立 / 更新模型。转速自动读取，建议覆盖常用工况。"):QStringLiteral("当前是旧参考匹配流程。正式检测请从四类已标注完整记录重新训练模型。"));
            e+=p(QStringLiteral("本次结果保持未知；此提示不表示实际转速错误或机械超速。"));
        }else if(inference.status=="outside_operating_range"){
            e+=p(speedDomainDetails());
            e+=p(QStringLiteral("当前窗转速超出模型适用范围，保留未知。模型范围不表示台架机械最高转速，此提示也不表示机械超速。"));
            e+=p(model&&referenceKind(model->kind)?QStringLiteral("请在当前转速、相同安装与换算配置下补齐四类独立参考；人工标签不会直接成为检测结果。"):QStringLiteral("请在当前相近转速、相同安装与采样配置下，确认机器正常后采集约10秒并建立正常基线；人工标签不会直接成为预测结果。"));
        }else if(invalidSpeed){
            e+=p(QStringLiteral("测速质量未通过，本次无法进行故障判别。"));
            for(const auto&s:inference.reasons)e+=p(reason(s));
            e+=p(QStringLiteral("下一步：检查测速连接、每转脉冲数，待转速稳定后重新采集。无需手工填写转速。"));
        }else{
            for(std::size_t i=0;i<std::min<std::size_t>(3,inference.reasons.size());++i)e+=p(reason(inference.reasons[i]));
            if(!model){e+=p(QStringLiteral("先采集并标注正常、松动、不对中、不平衡四种工况。"));e+=p(QStringLiteral("打开“参考样本库”，加入不同转速、不同损伤程度的独立BIN，再点“建立 / 更新模型”。"));e+=p(QStringLiteral("未知检测记录不需要选择类别，系统按实测转速匹配参考。"));}
        }
        const auto decisionDetails=e;
        if(resultAvailable)e=p(QStringLiteral("本次数据最匹配的工况。"));
        else if(window->synthetic)e=p(QStringLiteral("当前为合成数据，仅用于软件演示；请采集机械数据后检测。"));
        else if(hasDetectionModel())e=p(QStringLiteral("请采集完整的机械数据后检测，具体原因可查看详情。"));
        documents[0]->setHtml(html(e));
        QString f=p(QStringLiteral("AC RMS、1X RMS 单位 %1；其余特征无量纲。CH1/2/3 不自动等同物理轴向。").arg(q(amplitude_unit_symbol(config))))+p(QStringLiteral("1X 表示1倍转频（有效转速÷60 Hz），2X、3X分别为其2倍、3倍。特征来自对应频带的实际振动能量；转频参考不表示该位置必有谱峰。"))+"<table width='100%'><tr><th>特征</th><th>CH1</th><th>CH2</th><th>CH3</th></tr>";for(unsigned i=0;i<12;++i){f+="<tr><td>"+QString::fromUtf8(names[i])+"</td>";for(unsigned c=0;c<3;++c)f+=QStringLiteral("<td>%1</td>").arg(window->features[c*12+i],0,'g',6);f+="</tr>";}documents[1]->setHtml(html(f+"</table>"));
        QString g=decisionDetails+p(window->valid?QStringLiteral("原码连续、无削顶、测速有效、倍频带完整，参考幅值达到工程门限。") : QStringLiteral("当前窗口拒绝判别，原因如下："));for(const auto&s:inference.reasons)g+=p(reason(s));if(hasDetectionModel()&&window->fresh_periods)g+=p(speedDomainDetails());g+=p(QStringLiteral("2–500Hz参考RMS / ADC码：CH1 %1 · CH2 %2 · CH3 %3；门限%4。").arg(window->reference_rms_adc[0],0,'g',5).arg(window->reference_rms_adc[1],0,'g',5).arg(window->reference_rms_adc[2],0,'g',5).arg(candidate_minimum_reference_rms_adc,0,'f',0));g+=p(QStringLiteral("该门限仅提供ADC分辨率工程裕量；仪器噪声底尚未表征，不代表已知信噪比。"));g+=p(QStringLiteral("5kHz / 8192点 · 新测速周期 %1 · 转速CV %2% · 运算 %3ms").arg(window->fresh_periods).arg(window->speed_cv*100,0,'f',2).arg(window->elapsed_ms,0,'f',1));if(speedTolerant)g+=p(QStringLiteral("每类至少一份独立采集可建立四类模型；建议多次采集以覆盖不同工况。三次可做逐文件留出复核。结果为工程候选，尚非实测准确率。"));if(speedTolerant&&!inference.speed_support_status.empty())g+=p(QStringLiteral("转速参考：%1 · RPM距离 %2 · 支持因子 %3（工程权重，非概率、非实测准确率）。").arg(inference.speed_support_status=="extrapolated"?QStringLiteral("跨转速，待实测验证"):QStringLiteral("相近转速")).arg(inference.reference_speed_distance,0,'f',3).arg(inference.speed_support_factor,0,'f',3));g+=p(q(window->source))+p(q(window->input_kind))+p(q(window->source_path));for(const auto&s:inference.evidence)g+=p(q(s));documents[2]->setHtml(html(g));
        owner->setProperty("candidateStatus",q(inference.status));owner->setProperty("candidateClassification",q(inference.classification));owner->setProperty("candidateSource",q(inference.source));owner->setProperty("normalBaselineCount",model?int(model->baseline_count):0);
    }
    std::shared_ptr<const Capture>snapshot(bool full){if(!live||(full&&capture))return capture;auto c=std::make_shared<Capture>();c->header=liveHeader;const auto n=accelerator::required_raw_frames(liveHeader.sample_rate_hz);const auto begin=full||ring.size()<=n?ring.begin():ring.end()-n;c->records.assign(begin,ring.end());c->integrity_verified=true;c->source_metadata.synthetic=liveSynthetic;c->source_path="local_contiguous_raw_chunks";return c;}
    void submit(bool makeBaseline=false,const QString&exportPath={}){
        if(!exportPath.isEmpty())try{validate_detection_outputs({exportPath.toUtf8().toStdString()},protectedPaths());owner->setProperty("candidateOutputError",QString());}
        catch(const std::exception&e){const auto error=QString::fromUtf8(e.what());owner->setProperty("candidateOutputError",error);QMessageBox::warning(owner,QStringLiteral("导出未完成"),error);return;}
        if(backend->currentIndex()==0){state->setText(QStringLiteral("等待设置"));subtitle->setText(QStringLiteral("请在“高级设置”检查计算方式。"));return;}
        if(referenceTask.valid()){if(!makeBaseline&&exportPath.isEmpty())pending=true;return;}
        if(task.valid()){if(!makeBaseline&&exportPath.isEmpty())pending=true;return;}auto c=snapshot(makeBaseline||!exportPath.isEmpty());if(!c)return;
        if(makeBaseline&&(!normal->isChecked()||!mechanical->isChecked())){subtitle->setText(QStringLiteral("先核实并勾选机械输入和当前正常工况。"));return;}
        auto cfg=config;cfg.signal_kind=mechanical->isChecked()?SignalKind::VibrationConfirmed:SignalKind::Unknown;const auto setupId=setup->text().trimmed().toUtf8().toStdString();const auto mode=backend->currentIndex()==1?accelerator::Backend::Hardware:accelerator::Backend::Reference;const auto generation=epoch;const bool supervised=!legacyEnabled->isChecked()&&!makeBaseline;
        state->setText(makeBaseline?QStringLiteral("后台建立正常基线"):QStringLiteral("后台计算检测窗"));pending=false;
        task=std::async(std::launch::async,[c,cfg,setupId,mode,generation,makeBaseline,exportPath,supervised]{Task t;t.epoch=generation;t.exportPath=exportPath;try{const auto n=accelerator::required_raw_frames(c->header.sample_rate_hz);if(c->records.size()<n)throw AnalysisError("完整窗未就绪：需要 FIR 历史和 8192 个 5 kHz 点。");if(supervised){if(!exportPath.isEmpty()){for(std::size_t start=0;start+n<=c->records.size();start+=n){auto one=extract_supervised_window(*c,start,cfg,setupId,mode);t.jsonl+=supervised_window_json(one)+"\n";t.trainedWindow=std::move(one);}}else t.trainedWindow=extract_supervised_window(*c,c->records.size()-n,cfg,setupId,mode);}else if(makeBaseline||!exportPath.isEmpty()){std::vector<CandidateWindow>w;for(std::size_t start=0;start+n<=c->records.size();start+=n){auto one=extract_candidate_window(*c,start,cfg,setupId,mode);if(!exportPath.isEmpty())t.jsonl+=candidate_window_json(one)+"\n";w.push_back(std::move(one));}if(makeBaseline)t.model=fit_normal_baseline(w);t.window=w.back();}else t.window=extract_candidate_window(*c,c->records.size()-n,cfg,setupId,mode);}catch(const std::exception&e){t.error=e.what();}return t;});baseline->setEnabled(false);features->setEnabled(false);
    }
    void submitSegment(){
        if(task.valid()||referenceTask.valid()||pendingSegment.isEmpty()||backend->currentIndex()==0)return;
        const auto path=pendingSegment;pendingSegment.clear();lastSegment=path;const auto generation=epoch;auto cfg=config;cfg.signal_kind=mechanical->isChecked()?SignalKind::VibrationConfirmed:SignalKind::Unknown;const auto setupId=setup->text().trimmed().toUtf8().toStdString();const auto mode=backend->currentIndex()==1?accelerator::Backend::Hardware:accelerator::Backend::Reference;const bool supervised=!legacyEnabled->isChecked();
        task=std::async(std::launch::async,[path,generation,cfg,setupId,mode,supervised]{Task t;t.epoch=generation;try{t.committed=std::make_shared<Capture>(load_capture(path.toUtf8().toStdString()));const auto n=accelerator::required_raw_frames(t.committed->header.sample_rate_hz);if(t.committed->records.size()<n)throw AnalysisError("SD 已封段不足一个完整检测窗。");if(supervised)t.trainedWindow=extract_supervised_window(*t.committed,t.committed->records.size()-n,cfg,setupId,mode);else t.window=extract_candidate_window(*t.committed,t.committed->records.size()-n,cfg,setupId,mode);}catch(const std::exception&e){t.error=e.what();}return t;});baseline->setEnabled(false);features->setEnabled(false);
    }
    void nextTask(){if(!referenceTask.valid()&&!task.valid()&&!scheduledLibraryPath.isEmpty()){const auto path=scheduledLibraryPath;const bool keep=scheduledLibraryKeepsQueue;scheduledLibraryPath.clear();loadReferences(path,keep);}if(!referenceTask.valid()&&!referenceQueue.empty())startReference();if(referenceTask.valid())return;if(!pendingSegment.isEmpty())submitSegment();else if(pending)submit();}
    void poll(){
        if(!task.valid()||task.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        auto t=task.get();
        if(t.saveSupervised){supervisedSaved=t.error.empty();supervisedSaveError=q(t.error);if(supervisedSaved)supervisedSourcePath=supervisedPath;render();nextTask();return;}
        if(t.epoch!=epoch){render();nextTask();return;}
        if(t.saveOnly){modelSaved=t.error.empty();modelSaveError=q(t.error);if(modelSaved)modelPath=storage;render();nextTask();return;}
        if(!t.error.empty()){invalidatePublished();inference.status="awaiting_valid_data";trainedInference.status="waiting_for_data";owner->setProperty("candidateStatus","awaiting_valid_data");state->setText(QStringLiteral("等待有效数据"));subtitle->setText(QStringLiteral("当前记录尚无可分析窗口"));documents[0]->setHtml(html(p(QStringLiteral("请采集完整的机械数据后检测，具体原因可查看详情。"))));documents[2]->setHtml(html(p(q(t.error))));sourceHint(inputSynthetic());baseline->setEnabled(legacyEnabled->isChecked());features->setEnabled(bool(capture));nextTask();return;}
        // A preview gap can invalidate only its pending preview window. A
        // fully CRC-validated committed BIN remains usable during preview loss.
        if(t.window&&t.window->input_kind=="local_full_raw_crc_continuous"&&live&&(ring.empty()||t.window->last_index<ring.front().index)){nextTask();return;}
        if(t.trainedWindow&&t.trainedWindow->input_kind=="local_full_raw_crc_continuous"&&live&&(ring.empty()||t.trainedWindow->last_index<ring.front().index)){nextTask();return;}
        if(t.committed)capture=t.committed;
        if(t.trainedWindow)trainedWindow=std::move(t.trainedWindow);
        if(t.window)window=std::move(t.window);const bool newModel=bool(t.model);if(t.model){clearReferenceAssessment();model=std::move(t.model);modelSaved=false;modelSaveError.clear();}
        if(!t.exportPath.isEmpty())try{write_detection_text(t.exportPath.toUtf8().toStdString(),t.jsonl,protectedPaths());}catch(const std::exception&e){subtitle->setText(QString::fromUtf8(e.what()));}
        render();if(newModel)saveModel();else nextTask();
    }
};

CandidatePanel::CandidatePanel(QWidget*parent):QWidget(parent),impl_(std::make_unique<Impl>(this)){
    setObjectName("detectionPane");auto&d=*impl_;
    setStyleSheet("QWidget#detectionPane,QTextBrowser{background:white;color:#344255;}QFrame#detectionResultCard{background:#f5f8fe;border:1px solid #e1e9f6;border-radius:10px;}QFrame#detectionScoresCard{background:white;border:1px solid #e5ebf3;border-radius:10px;}QFrame#detectionResultCard QLabel,QFrame#detectionScoresCard QLabel{border:0;background:transparent;}QPushButton,QComboBox,QLineEdit{min-height:44px;font-size:16px;}QPushButton{border:1px solid #cbd6e6;border-radius:6px;padding:0 16px;background:white;color:#40536d;}QPushButton:hover{background:#f2f6ff;border-color:#7c9edd;}QPushButton:pressed{background:#e3edff;}QPushButton#openReferenceLibrary{background:#3768ce;color:white;border-color:#3768ce;}QCheckBox{min-height:44px;font-size:15px;}QCheckBox::indicator{width:20px;height:20px;}QLabel#detectionState{font-size:26px;font-weight:600;color:#263b54;}QLabel#detectionSubtitle,QLabel#detectionProfileSummary,QLabel#detectionSignalSource{font-size:13px;color:#748298;}QLabel#detectionScoreNote{font-size:12px;color:#748298;}QProgressBar{background:#edf1f8;border:0;border-radius:4px;min-height:8px;max-height:8px;}QProgressBar::chunk{background:#5b80d4;border-radius:4px;}");
    auto*l=new QVBoxLayout(this);l->setContentsMargins(16,12,16,10);l->setSpacing(10);
    auto*overview=new QHBoxLayout;overview->setSpacing(12);
    auto*resultCard=new QFrame;resultCard->setObjectName("detectionResultCard");auto*resultLayout=new QVBoxLayout(resultCard);resultLayout->setContentsMargins(18,10,18,10);resultLayout->setSpacing(2);
    d.state=new QLabel;d.state->setObjectName("detectionState");d.state->setWordWrap(true);d.state->setMinimumHeight(54);d.subtitle=new QLabel;d.subtitle->setObjectName("detectionSubtitle");d.subtitle->setWordWrap(true);d.subtitle->setMinimumHeight(20);resultLayout->addWidget(d.state);resultLayout->addWidget(d.subtitle);
    d.signalSource=new QLabel;d.signalSource->setObjectName("detectionSignalSource");d.signalSource->setMinimumWidth(0);d.signalSource->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);resultLayout->addWidget(d.signalSource);
    auto*scoresCard=new QFrame;scoresCard->setObjectName("detectionScoresCard");auto*scoresLayout=new QVBoxLayout(scoresCard);scoresLayout->setContentsMargins(18,10,18,10);scoresLayout->setSpacing(4);auto*scoreTitle=new QLabel(QStringLiteral("四类模型得分"));scoreTitle->setStyleSheet("font-size:15px;font-weight:600;color:#344a66;");scoresLayout->addWidget(scoreTitle);auto*scoreRows=new QGridLayout;scoreRows->setHorizontalSpacing(12);scoreRows->setVerticalSpacing(3);const unsigned scoreOrder[]{0,3,2,1};const char*scoreKeys[]{"normal","imbalance","misalignment","looseness"};for(unsigned row=0;row<4;++row){const auto cls=scoreOrder[row];auto*name=new QLabel(resultLabel(scoreKeys[cls]));name->setStyleSheet("font-size:14px;");scoreRows->addWidget(name,row,0);d.scoreBars[cls]=new QProgressBar;d.scoreBars[cls]->setObjectName(QStringLiteral("detectionScore_")+scoreKeys[cls]);d.scoreBars[cls]->setRange(0,1000);d.scoreBars[cls]->setTextVisible(false);d.scoreBars[cls]->setMinimumWidth(80);d.scoreBars[cls]->setAccessibleName(name->text()+QStringLiteral("模型得分"));scoreRows->addWidget(d.scoreBars[cls],row,1);d.scoreValues[cls]=new QLabel(QStringLiteral("—"));d.scoreValues[cls]->setObjectName(QStringLiteral("detectionScoreValue_")+scoreKeys[cls]);d.scoreValues[cls]->setMinimumWidth(47);d.scoreValues[cls]->setAlignment(Qt::AlignRight|Qt::AlignVCenter);d.scoreValues[cls]->setStyleSheet("font-size:14px;color:#526886;");scoreRows->addWidget(d.scoreValues[cls],row,2);}scoreRows->setColumnStretch(1,1);scoresLayout->addLayout(scoreRows,1);d.scoreNote=new QLabel;d.scoreNote->setObjectName("detectionScoreNote");d.scoreNote->setWordWrap(true);scoresLayout->addWidget(d.scoreNote);
    overview->addWidget(resultCard,3);overview->addWidget(scoresCard,2);l->addLayout(overview,1);
    d.settingsDialog=new QDialog(this);d.settingsDialog->setObjectName("candidateModelSettings");d.settingsDialog->setWindowTitle(QStringLiteral("高级设置"));d.settingsDialog->setModal(true);d.settingsDialog->resize(680,550);d.settingsDialog->setStyleSheet("QDialog{background:white;color:#344255;}QPushButton,QLineEdit,QComboBox{min-height:44px;font-size:16px;}QCheckBox{min-height:44px;font-size:16px;}");auto*settingsLayout=new QVBoxLayout(d.settingsDialog);settingsLayout->setContentsMargins(22,16,22,16);settingsLayout->setSpacing(10);
    auto*notice=new QLabel(QStringLiteral("普通检测使用已训练的四类模型，转速和人工工况标签不参与分类。以下为维护与旧流程兼容选项；台架、换算或采样配置变动后需重新训练。旧模型兼容仅对本次会话生效。"));notice->setWordWrap(true);settingsLayout->addWidget(notice);auto*form=new QFormLayout;d.setup=new TouchLineEdit;d.setup->setText("rig1_mount1");d.setup->setObjectName("candidateSetupId");d.setup->setMaxLength(128);form->addRow(QStringLiteral("台架与安装"),d.setup);d.backend=new QComboBox;d.backend->setObjectName("candidateBackend");d.backend->addItems({QStringLiteral("选择计算方式"),QStringLiteral("FPGA计算"),QStringLiteral("ARM计算")});const auto caps=accelerator::probe();d.backend->setCurrentIndex(caps.available?1:2);d.backend->setToolTip(caps.available?QStringLiteral("FPGA 身份和接口已核验"):QStringLiteral("FPGA 不可用：")+q(caps.reason));form->addRow(QStringLiteral("计算方式"),d.backend);settingsLayout->addLayout(form);
    auto*confirm=new QHBoxLayout;d.mechanical=new QCheckBox(QStringLiteral("确认本次输入来自机械传感器"));d.mechanical->setObjectName("candidateMechanicalConfirmed");d.mechanical->setMinimumHeight(44);d.mechanical->setToolTip(QStringLiteral("这是对本次接线来源的声明；工况标签不会自动确认来源，合成记录始终拒绝诊断。"));resultLayout->addWidget(d.mechanical);d.legacyEnabled=new QCheckBox(QStringLiteral("启用旧模型兼容检测（仅本次）"));d.legacyEnabled->setObjectName("candidateLegacyCompatibility");confirm->addWidget(d.legacyEnabled);d.normal=new QCheckBox(QStringLiteral("建立基线前确认当前正常"));d.normal->setObjectName("candidateNormalConfirmed");confirm->addWidget(d.normal);settingsLayout->addLayout(confirm);
    auto*buttons=new QGridLayout;d.baseline=new QPushButton(QStringLiteral("建立正常基线（兼容）"));d.baseline->setObjectName("buildNormalBaseline");d.save=new QPushButton(QStringLiteral("保存模型"));d.save->setObjectName("saveCandidateModel");d.load=new QPushButton(QStringLiteral("加载模型"));d.load->setObjectName("loadCandidateModel");d.features=new QPushButton(QStringLiteral("导出训练特征"));d.features->setObjectName("exportCandidateFeatures");buttons->addWidget(d.baseline,0,0);buttons->addWidget(d.save,0,1);buttons->addWidget(d.load,1,0);buttons->addWidget(d.features,1,1);settingsLayout->addLayout(buttons);d.settingsStatus=new QLabel;d.settingsStatus->setObjectName("candidateSettingsStatus");d.settingsStatus->setWordWrap(true);settingsLayout->addWidget(d.settingsStatus);settingsLayout->addStretch();auto*close=new QDialogButtonBox(QDialogButtonBox::Close);close->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));connect(close,&QDialogButtonBox::rejected,d.settingsDialog,&QDialog::reject);settingsLayout->addWidget(close);
    d.referenceDialog=new QDialog(this);d.referenceDialog->setObjectName("referenceLibraryDialog");d.referenceDialog->setWindowTitle(QStringLiteral("四类参考样本库"));d.referenceDialog->resize(900,560);d.referenceDialog->setStyleSheet("QDialog{background:white;color:#344255;}QPushButton,QCheckBox{min-height:44px;font-size:16px;}QPushButton{border:1px solid #cbd6e6;border-radius:6px;padding:0 14px;background:white;color:#40536d;}QPushButton#buildReferenceModel{background:#3768ce;border-color:#3768ce;color:white;}QPushButton:disabled,QPushButton#buildReferenceModel:disabled{background:#f1f4f8;border-color:#e1e7ef;color:#a4adba;}QTreeWidget{font-size:16px;border:1px solid #e1e7f0;border-radius:6px;alternate-background-color:#f7f9fc;selection-background-color:#e7effe;selection-color:#2f4d7a;}QTreeWidget::item{min-height:44px;}QHeaderView::section{background:#edf2f9;color:#61738e;border:0;border-bottom:1px solid #dce4f0;padding:8px;font-size:14px;}");auto*rl=new QVBoxLayout(d.referenceDialog);rl->setContentsMargins(18,12,18,12);rl->setSpacing(8);
    auto*rn=new QLabel(QStringLiteral("加入已标注的正常、松动、不对中、不平衡完整记录，然后训练 / 更新模型。"));rn->setWordWrap(true);rl->addWidget(rn);
    auto*rm=new QCheckBox(QStringLiteral("确认本批参考来自同一机械台架"));rm->setObjectName("referenceMechanicalConfirmed");d.referenceMechanical=rm;rm->setToolTip(QStringLiteral("本批次只需确认一次。程序仍逐份核验来源、人工标签、原码完整性与采集会话；当前检测输入的来源需单独确认。"));connect(rm,&QCheckBox::toggled,this,[this]{impl_->confirmReferenceSource();});auto*sourceRow=new QHBoxLayout;sourceRow->addWidget(rm);sourceRow->addStretch();auto*referenceBackend=new QComboBox;referenceBackend->setObjectName("referenceBackend");for(int i=0;i<d.backend->count();++i)referenceBackend->addItem(d.backend->itemText(i));referenceBackend->setCurrentIndex(d.backend->currentIndex());referenceBackend->setMinimumHeight(44);connect(referenceBackend,QOverload<int>::of(&QComboBox::currentIndexChanged),d.backend,&QComboBox::setCurrentIndex);connect(d.backend,QOverload<int>::of(&QComboBox::currentIndexChanged),referenceBackend,&QComboBox::setCurrentIndex);auto*libraryAdvanced=new QWidget;libraryAdvanced->setObjectName("referenceLibraryAdvanced");auto*libraryAdvancedLayout=new QVBoxLayout(libraryAdvanced);libraryAdvancedLayout->setContentsMargins(0,0,0,0);auto*advancedCompute=new QHBoxLayout;advancedCompute->addWidget(new QLabel(QStringLiteral("计算方式")));advancedCompute->addWidget(referenceBackend);libraryAdvancedLayout->addLayout(advancedCompute);auto*libraryMore=new QPushButton(QStringLiteral("高级"));libraryMore->setObjectName("referenceLibraryMore");libraryMore->setCheckable(true);sourceRow->addWidget(libraryMore);connect(libraryMore,&QPushButton::toggled,libraryAdvanced,&QWidget::setVisible);libraryAdvanced->hide();rl->addLayout(sourceRow);
    d.referenceCounts=new QLabel;d.referenceCounts->setObjectName("referenceClassCounts");d.referenceCounts->setStyleSheet("font-size:14px;color:#61738e;");rl->addWidget(d.referenceCounts);d.referenceList=new QTreeWidget;d.referenceList->setObjectName("referenceLibraryList");d.referenceList->setHeaderLabels({QStringLiteral("人工工况"),QStringLiteral("来源目录 / BIN"),QStringLiteral("训练窗"),QStringLiteral("采集会话")});d.referenceList->setRootIsDecorated(false);d.referenceList->setSelectionMode(QAbstractItemView::SingleSelection);d.referenceList->setAlternatingRowColors(true);d.referenceList->header()->setStretchLastSection(false);d.referenceList->header()->setSectionResizeMode(0,QHeaderView::Fixed);d.referenceList->header()->setSectionResizeMode(1,QHeaderView::Stretch);d.referenceList->header()->setSectionResizeMode(2,QHeaderView::Fixed);d.referenceList->header()->setSectionResizeMode(3,QHeaderView::Fixed);d.referenceList->setColumnWidth(0,100);d.referenceList->setColumnWidth(2,90);d.referenceList->setColumnWidth(3,110);d.referenceList->setTextElideMode(Qt::ElideMiddle);rl->addWidget(d.referenceList,1);
    auto*rb=new QGridLayout;auto*extraLibraryButtons=new QHBoxLayout;auto*addCurrent=new QPushButton(QStringLiteral("加入当前标注BIN"));addCurrent->setObjectName("addCurrentReference");d.referenceRemove=new QPushButton(QStringLiteral("移出参考库"));d.referenceRemove->setObjectName("removeReference");d.referenceBuild=new QPushButton(QStringLiteral("训练 / 更新模型"));d.referenceBuild->setObjectName("buildReferenceModel");auto*saveLibrary=new QPushButton(QStringLiteral("另存参考库"));saveLibrary->setObjectName("saveReferenceLibrary");auto*loadLibrary=new QPushButton(QStringLiteral("加载参考库"));loadLibrary->setObjectName("loadReferenceLibrary");auto*loadDefaultLibrary=new QPushButton(QStringLiteral("加载本地参考库"));loadDefaultLibrary->setObjectName("loadDefaultReferenceLibrary");rb->addWidget(addCurrent,0,0);rb->addWidget(d.referenceRemove,0,1);rb->addWidget(d.referenceBuild,0,2);extraLibraryButtons->addWidget(saveLibrary);extraLibraryButtons->addWidget(loadLibrary);extraLibraryButtons->addWidget(loadDefaultLibrary);libraryAdvancedLayout->addLayout(extraLibraryButtons);rl->addLayout(rb);rl->addWidget(libraryAdvanced);d.referenceStatus=new QLabel(QStringLiteral("确认本批来源后，可加入已标注记录。"));d.referenceStatus->setObjectName("referenceLibraryStatus");d.referenceStatus->setWordWrap(true);d.referenceStatus->setMinimumHeight(44);d.referenceStatus->setMaximumHeight(64);rl->addWidget(d.referenceStatus);auto*rclose=new QDialogButtonBox(QDialogButtonBox::Close);rclose->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));connect(rclose,&QDialogButtonBox::rejected,d.referenceDialog,&QDialog::reject);rl->addWidget(rclose);connect(d.referenceDialog,&QDialog::finished,this,[this]{impl_->cancelAwaitingReference();});
    connect(addCurrent,&QPushButton::clicked,this,[this]{auto&d=*impl_;if(!d.capture||d.capture->source_path.empty()){d.referenceMessage(QStringLiteral("请先打开已保存并标注的BIN；实时预览不能直接加入。"));return;}RecordLabelAnnotation annotation;QString error;if(!loadRecordLabel(q(d.capture->source_path),&annotation,&error)){d.referenceMessage(error);return;}d.addReference(q(d.capture->source_path),recordLabelKey(annotation.label));});
    connect(d.referenceRemove,&QPushButton::clicked,this,[this]{auto&d=*impl_;if(d.referenceTask.valid())return;const auto*item=d.referenceList->currentItem();if(!item)return;const auto i=item->data(0,Qt::UserRole).toInt();if(i<0||std::size_t(i)>=d.references.size())return;++d.referenceEpoch;d.references.erase(d.references.begin()+i);d.refreshReferences();if(d.persistReferences(d.libraryPath))d.referenceMessage(QStringLiteral("已移出并保存参考库。原BIN和人工标签保留；已有模型需重新建立后才会更新。"));});
    connect(d.referenceBuild,&QPushButton::clicked,this,[this]{impl_->buildReferences();});connect(saveLibrary,&QPushButton::clicked,this,[this]{auto&d=*impl_;if(d.referenceTask.valid())return;const auto path=QFileDialog::getSaveFileName(d.referenceDialog,QStringLiteral("另存参考库"),d.libraryPath,"Reference library (*.json)",nullptr,QFileDialog::DontUseNativeDialog);if(!path.isEmpty()&&d.persistReferences(path))d.referenceMessage(QStringLiteral("参考库已保存；加载时将重新核验原BIN哈希、标签和质量特征。"));});
    connect(loadLibrary,&QPushButton::clicked,this,[this]{auto&d=*impl_;const auto path=QFileDialog::getOpenFileName(d.referenceDialog,QStringLiteral("加载参考库"),d.libraryPath,"Reference library (*.json)",nullptr,QFileDialog::DontUseNativeDialog);if(!path.isEmpty())d.loadReferences(path);});connect(loadDefaultLibrary,&QPushButton::clicked,this,[this]{impl_->loadReferences(impl_->libraryLoadPath);});d.refreshReferences();
    d.detailsDialog=new QDialog(this);d.detailsDialog->setObjectName("candidateDetailsDialog");d.detailsDialog->setWindowTitle(QStringLiteral("检测详情"));d.detailsDialog->setModal(true);d.detailsDialog->resize(900,560);d.detailsDialog->setStyleSheet("QDialog{background:white;color:#344255;}QPushButton{min-height:44px;font-size:16px;}QTabBar::tab{min-height:44px;min-width:120px;font-size:16px;}");auto*detailsLayout=new QVBoxLayout(d.detailsDialog);detailsLayout->setContentsMargins(16,12,16,12);d.detailsTabs=new QTabWidget;d.detailsTabs->setObjectName("candidateDetailsTabs");detailsLayout->addWidget(d.detailsTabs,1);
    const char*ids[]{"detectionEvidence","detectionFeatures","detectionGates"};for(int i=0;i<3;++i){d.documents[i]=new QTextBrowser;d.documents[i]->setObjectName(ids[i]);d.documents[i]->setFrameShape(QFrame::NoFrame);d.documents[i]->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);d.documents[i]->document()->setDocumentMargin(0);}d.detailsTabs->addTab(d.documents[2],QStringLiteral("模型与验证"));d.detailsTabs->addTab(d.documents[1],QStringLiteral("特征"));auto*detailsClose=new QDialogButtonBox(QDialogButtonBox::Close);detailsClose->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));connect(detailsClose,&QDialogButtonBox::rejected,d.detailsDialog,&QDialog::reject);detailsLayout->addWidget(detailsClose);
    d.documents[0]->setFixedHeight(30);d.documents[0]->setStyleSheet("QTextBrowser{background:transparent;border:0;color:#61738e;}");d.documents[0]->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);resultLayout->addWidget(d.documents[0]);resultLayout->addStretch();
    d.signalSummary=new SignalSummary;l->addWidget(d.signalSummary,1);
    auto*actions=new QHBoxLayout;actions->setSpacing(8);d.profile=new QLabel;d.profile->setObjectName("detectionProfileSummary");d.profile->setWordWrap(true);d.profile->setMinimumWidth(0);d.profile->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);actions->addWidget(d.profile,1);auto*libraryButton=new QPushButton(QStringLiteral("参考样本库"));libraryButton->setObjectName("openReferenceLibrary");libraryButton->setMinimumWidth(136);actions->addWidget(libraryButton);connect(libraryButton,&QPushButton::clicked,d.referenceDialog,&QDialog::open);auto*detailsButton=new QPushButton(QStringLiteral("详情"));detailsButton->setObjectName("openCandidateDetails");detailsButton->setMinimumWidth(92);actions->addWidget(detailsButton);connect(detailsButton,&QPushButton::clicked,d.detailsDialog,&QDialog::open);auto*manage=new QPushButton(QStringLiteral("高级设置"));manage->setObjectName("openCandidateModelSettings");manage->setMinimumWidth(120);actions->addWidget(manage);connect(manage,&QPushButton::clicked,d.settingsDialog,&QDialog::open);l->addLayout(actions);
    connect(d.baseline,&QPushButton::clicked,this,[this]{++impl_->referenceEpoch;impl_->referenceModelNeedsSave=false;impl_->submit(true);});connect(d.save,&QPushButton::clicked,this,[this]{if(impl_->legacyEnabled->isChecked())impl_->saveModel();else impl_->saveSupervisedModel();});
    connect(d.load,&QPushButton::clicked,this,[this]{auto&d=*impl_;const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("加载模型"),QFileInfo(d.supervisedPath).absolutePath(),"Model (*.model);;All (*)",nullptr,QFileDialog::DontUseNativeDialog);if(path.isEmpty())return;try{QFile source(path);if(!source.open(QIODevice::ReadOnly)||source.size()>qint64(candidate_model_max_bytes))throw AnalysisError("模型无法读取或超过大小限制");const auto bytes=source.readAll();const bool supervised=bytes.startsWith("schema=zynq_vibration.supervised_model.");++d.epoch;++d.referenceEpoch;d.referenceModelNeedsSave=false;d.clearReferenceAssessment();if(supervised){d.trainedModel=parse_supervised_model(bytes.toStdString());d.setup->setText(q(d.trainedModel->setup_id));d.supervisedSourcePath=path;d.supervisedSaved=QFileInfo(path).canonicalFilePath()==QFileInfo(d.supervisedPath).canonicalFilePath();d.supervisedSaveError.clear();}else{d.model=parse_candidate_model(bytes.toStdString());d.setup->setText(q(d.model->setup_id));d.modelPath=path;d.modelSaved=QFileInfo(path).canonicalFilePath()==QFileInfo(d.storage).canonicalFilePath();d.modelSaveError.clear();}d.render();}catch(const std::exception&e){d.subtitle->setText(QString::fromUtf8(e.what()));}});
    connect(d.features,&QPushButton::clicked,this,[this]{auto&d=*impl_;const auto path=QFileDialog::getSaveFileName(this,QStringLiteral("导出无标签训练特征"),QFileInfo(d.storage).absolutePath()+"/features.jsonl","JSONL (*.jsonl)",nullptr,QFileDialog::DontUseNativeDialog);if(!path.isEmpty())d.submit(false,path);});
    const auto reconfigure=[this]{++impl_->epoch;++impl_->referenceEpoch;impl_->referenceModelNeedsSave=false;impl_->invalidatePublished();if(!impl_->pendingSegment.isEmpty())impl_->submitSegment();else impl_->submit();impl_->sourceHint(impl_->live?impl_->liveSynthetic:impl_->capture&&impl_->capture->source_metadata.synthetic);};connect(d.mechanical,&QCheckBox::toggled,this,reconfigure);connect(d.legacyEnabled,&QCheckBox::toggled,this,reconfigure);connect(d.backend,QOverload<int>::of(&QComboBox::currentIndexChanged),this,reconfigure);connect(d.setup,&QLineEdit::editingFinished,this,reconfigure);
    auto*timer=new QTimer(this);timer->setInterval(80);connect(timer,&QTimer::timeout,this,[this]{impl_->poll();impl_->pollReferences();impl_->updateAge();impl_->settingsStatus->setText(impl_->profile->text()+"\n"+impl_->state->text());});timer->start();clear();
}
CandidatePanel::~CandidatePanel()=default;
bool CandidatePanel::addReferenceFile(const QString&path,const QString&labelKey){return impl_->addReference(path,labelKey);}
void CandidatePanel::showReferenceLibrary(){impl_->referenceDialog->open();impl_->refreshReferences();}
void CandidatePanel::clear(){auto&d=*impl_;++d.epoch;d.capture.reset();d.ring.clear();d.invalidatePublished();d.live=false;d.job.clear();d.lastSubmitted=0;d.pending=false;d.pendingSegment.clear();d.lastSegment.clear();d.observedEnd=0;d.lastRawMs=0;d.state->setText(QStringLiteral("等待检测数据"));d.subtitle->setText(QStringLiteral("打开记录或开始采集"));for(auto*b:d.documents)b->setHtml(html(p(QStringLiteral("采集并标注四种工况，在参考样本库训练模型。"))));d.render();}
void CandidatePanel::setStorageRoot(const QString&root){auto&d=*impl_;d.clearReferenceAssessment();const QDir base(root);d.storage=base.absoluteFilePath(".vibration-workbench/detection.model");d.supervisedPath=base.absoluteFilePath(".vibration-workbench/supervised.model");d.supervisedValidationPath=base.absoluteFilePath(".vibration-workbench/supervised-validation.json");d.legacyLibraryPath=base.absoluteFilePath(".vibration-workbench/reference-library.json");d.libraryPath=base.absoluteFilePath(".vibration-workbench/supervised-library.json");d.libraryLoadPath=QFileInfo::exists(d.libraryPath)?d.libraryPath:d.legacyLibraryPath;d.libraryNeedsLoad=QFileInfo::exists(d.libraryLoadPath);
    if(QFileInfo::exists(d.storage))try{d.model=load_candidate_model(d.storage.toUtf8().toStdString());d.setup->setText(q(d.model->setup_id));d.modelPath=d.storage;d.modelSaved=true;d.modelSaveError.clear();}catch(const std::exception&e){d.modelSaved=false;d.modelSaveError=QString::fromUtf8(e.what());}
    if(QFileInfo::exists(d.supervisedPath))try{d.trainedModel=load_supervised_model(d.supervisedPath.toUtf8().toStdString());d.supervisedSourcePath=d.supervisedPath;d.setup->setText(q(d.trainedModel->setup_id));d.supervisedSaved=true;d.supervisedSaveError.clear();QFile report(d.supervisedValidationPath);if(report.open(QIODevice::ReadOnly)&&report.size()<=qint64(candidate_model_max_bytes)){const auto bytes=report.readAll();const auto object=QJsonDocument::fromJson(bytes).object();if(object.value("schema")=="zynq_vibration.supervised_validation.v1"&&object.value("deployment_model_sha256").toString()==q(modelDigest(*d.trainedModel))){d.referenceValidation=bytes;d.referenceValidationModelDigest=modelDigest(*d.trainedModel);}}}catch(const std::exception&e){d.supervisedSaved=false;d.supervisedSaveError=QString::fromUtf8(e.what());}
    d.refreshReferences();d.maybeLoadLocalReferences();d.render();}
void CandidatePanel::setCapture(std::shared_ptr<const Capture>c,const AnalysisConfig&cfg){auto&d=*impl_;++d.epoch;d.live=false;d.ring.clear();d.capture=std::move(c);if(!sameInputConfig(d.config,cfg)){++d.referenceEpoch;d.referenceModelNeedsSave=false;}d.config=cfg;d.acceptSourceConfirmation(cfg);d.invalidatePublished();d.pendingSegment.clear();d.normal->setChecked(false);d.submit();d.sourceHint(d.capture&&d.capture->source_metadata.synthetic);}
void CandidatePanel::setResult(const AnalysisResult*r,const DetectionResult*){if(!r){clear();return;}auto&d=*impl_;if(!sameInputConfig(d.config,r->config)){++d.epoch;++d.referenceEpoch;d.referenceModelNeedsSave=false;d.ring.clear();d.invalidatePublished();d.lastSubmitted=0;}d.config=r->config;d.acceptSourceConfirmation(r->config);d.sourceHint(r->source_metadata.synthetic);}
void CandidatePanel::invalidateLive(const QString&s){auto&d=*impl_;d.ring.clear();d.invalidatePublished();d.lastSubmitted=0;d.pending=false;d.state->setText(QStringLiteral("检测窗已作废"));d.subtitle->setText(s);}
void CandidatePanel::acceptSegment(const QString&path,const AnalysisConfig&cfg){auto&d=*impl_;if(path.isEmpty()||path==d.lastSegment||path==d.pendingSegment)return;if(!sameInputConfig(d.config,cfg)){++d.epoch;++d.referenceEpoch;d.referenceModelNeedsSave=false;d.ring.clear();d.invalidatePublished();d.lastSubmitted=0;}d.config=cfg;d.acceptSourceConfirmation(cfg);d.pendingSegment=path;d.submitSegment();}
void CandidatePanel::acceptLive(const QJsonObject&f,const AnalysisConfig&cfg){auto&d=*impl_;try{
    QByteArray raw;quint64 first=0;quint32 count=0,fs=0,ppr=0;QString job;bool synthetic=false;
    if(f.value("schema")=="vibration.stream.v1"){auto s=stream_contract::decode(f);raw=s.raw;first=s.first;count=s.count;fs=s.rate;ppr=s.ppr;job=s.session;synthetic=s.synthetic;}
    else{count=quint32(f.value("record_count").toInt());fs=quint32(f.value("sample_rate_hz").toInt());ppr=quint32(f.value("ppr").toInt());job=f.value("job_id").toString()+"/"+f.value("generation").toString();synthetic=f.value("synthetic").toBool();first=quint64(f.value("first_record").toDouble());if(!transport::decodeCanonicalBase64(f.value("raw_base64").toString(),int(count*32),raw)||crc32(reinterpret_cast<const std::uint8_t*>(raw.constData()),std::size_t(raw.size()))!=quint32(f.value("crc32").toDouble()))throw AnalysisError("live raw CRC mismatch");}
    if(!sameInputConfig(d.config,cfg)){++d.referenceEpoch;d.referenceModelNeedsSave=false;}
    if(!d.live||d.job!=job||d.liveHeader.sample_rate_hz!=fs||d.liveHeader.pulses_per_rev!=ppr||!sameInputConfig(d.config,cfg)){++d.epoch;d.ring.clear();d.invalidatePublished();d.lastSubmitted=0;d.pending=false;d.live=true;d.job=job;d.capture.reset();d.pendingSegment.clear();d.lastSegment.clear();d.normal->setChecked(false);}d.config=cfg;d.acceptSourceConfirmation(cfg);d.liveSynthetic=synthetic;d.liveHeader.sample_rate_hz=fs;d.liveHeader.clock_hz=50000000;d.liveHeader.adc_range_uv=5000000;d.liveHeader.channel_map=0x50301;d.liveHeader.pulses_per_rev=ppr;
    d.liveHeader.stream_segment=f.value("schema")=="vibration.stream.v1";
    if(d.liveHeader.stream_segment){const auto session=QByteArray::fromHex(job.toLatin1());if(session.size()==16)std::copy(session.begin(),session.end(),d.liveHeader.session_id.begin());}
    d.sourceHint(synthetic);
    if(!d.ring.empty()&&first!=d.ring.back().index+1){d.ring.clear();d.invalidatePublished();d.lastSubmitted=0;d.pending=false;d.state->setText(QStringLiteral("输入缺口，检测已重新预热"));}
    for(unsigned i=0;i<count;++i){auto*b=reinterpret_cast<const unsigned char*>(raw.constData())+i*32;Record r;r.index=stream_contract::u64(b);r.tick=stream_contract::u64(b+8);for(unsigned ch=0;ch<3;++ch){const auto u=stream_contract::u16(b+16+ch*2);r.raw[ch]=std::int16_t(u<32768?int(u):int(u)-65536);}r.flags=stream_contract::u16(b+22);r.speed_period_ticks=stream_contract::u32(b+24);r.speed_edge_count=stream_contract::u32(b+28);d.ring.push_back(r);}
    const auto n=accelerator::required_raw_frames(fs);while(d.ring.size()>std::size_t(n)*6)d.ring.pop_front();const auto last=d.ring.empty()?0:d.ring.back().index;d.observedEnd=last;d.lastRawMs=QDateTime::currentMSecsSinceEpoch();if(d.ring.size()>=n&&(d.lastSubmitted==0||last-d.lastSubmitted>=std::uint64_t(fs)*4096/5000)){d.lastSubmitted=last;if(!d.task.valid())d.submit();else{++d.skipped;d.pending=true;}}
}catch(const std::exception&e){invalidateLive(QString::fromUtf8(e.what()));}}
std::string CandidatePanel::candidateJson()const{const auto&d=*impl_;const auto windowJson=!d.legacyEnabled->isChecked()?(d.trainedWindow?supervised_window_json(*d.trainedWindow):"null"):(d.window?candidate_window_json(*d.window):"null");const auto resultJson=!d.legacyEnabled->isChecked()?supervised_inference_json(d.trainedInference):candidate_inference_json(d.inference);return std::string("{\"window\":")+windowJson+",\"inference\":"+resultJson+",\"display_age_seconds\":"+std::to_string(d.ageSeconds())+",\"historical\":"+(d.live&&d.ageSeconds()>3?"true":"false")+"}";}
} // namespace vibration::workbench
