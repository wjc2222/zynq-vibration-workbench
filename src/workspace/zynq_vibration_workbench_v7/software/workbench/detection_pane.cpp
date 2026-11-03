#include "detection_pane.hpp"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace vibration::workbench {
namespace {
QString utf8(const std::string& value) { return QString::fromUtf8(value.data(), int(value.size())); }
QString html(const std::string& value) { return utf8(value).toHtmlEscaped(); }
QString number(double value, int precision = 5) { return std::isfinite(value) ? QString::number(value, 'g', precision) : QStringLiteral("—"); }
QString number(const std::optional<double>& value, int precision = 5) { return value ? number(*value, precision) : QStringLiteral("—"); }
QString candidate(const std::string& value) {
    if (value == "normal") return QStringLiteral("正常状态候选");
    if (value == "imbalance") return QStringLiteral("不平衡候选");
    if (value == "misalignment") return QStringLiteral("不对中候选");
    if (value == "looseness") return QStringLiteral("松动候选");
    return utf8(value) + QStringLiteral("候选");
}
QString feature(const std::string& value) {
    if (value == "mean") return QStringLiteral("均值");
    if (value == "rms") return QStringLiteral("总 RMS");
    if (value == "ac_rms") return QStringLiteral("交流 RMS");
    if (value == "abs_peak") return QStringLiteral("绝对峰值");
    if (value == "peak_to_peak") return QStringLiteral("峰峰值");
    if (value == "crest_factor") return QStringLiteral("峰值因子");
    if (value == "pearson_kurtosis") return QStringLiteral("峭度");
    if (value == "order1_rms") return QStringLiteral("1X RMS");
    if (value == "order2_rms") return QStringLiteral("2X RMS");
    if (value == "order3_rms") return QStringLiteral("3X RMS");
    return utf8(value);
}
QString gateTitle(const std::string& value) {
    if (value == "setup") return QStringLiteral("台架与安装标识");
    if (value == "source") return QStringLiteral("数据来源声明");
    if (value == "calibration") return QStringLiteral("实际标定");
    if (value == "analysis_configuration") return QStringLiteral("分析参数一致性");
    if (value == "ppr") return QStringLiteral("每转脉冲数");
    if (value == "record_count") return QStringLiteral("记录长度");
    if (value == "adc_endpoints") return QStringLiteral("削顶与饱和");
    if (value == "finite_features") return QStringLiteral("数值特征完整性");
    if (value == "required_features") return QStringLiteral("规则所需特征");
    if (value == "comparison_domain") return QStringLiteral("对比值计算条件");
    if (value == "speed") return QStringLiteral("测速与工况范围");
    return utf8(value);
}
QString gateDetail(const DetectionGate& gate) {
    if (gate.id == "setup") return QStringLiteral("当前台架、安装标识须与配置一致。");
    if (gate.id == "source") return QStringLiteral("当前与基线须声明为机械振动，且来源说明有效。");
    if (gate.id == "calibration") return QStringLiteral("当前与基线须确认实际标定，并使用加速度单位。");
    if (gate.id == "analysis_configuration") return QStringLiteral("显示单位、各通道灵敏度、增益、零点、采样率、量程、通道映射与 FFT 配置须一致。");
    if (gate.id == "ppr") return QStringLiteral("当前与基线须具有相同且已知的 PPR。");
    if (gate.id == "record_count") return QStringLiteral("当前记录须达到配置规定的最低采样点数。");
    if (gate.id == "adc_endpoints") return QStringLiteral("三个通道须均不包含 ADC 满量程端点码。");
    if (gate.id == "finite_features") return QStringLiteral("记录时长与核心时域特征须为有效数值。");
    if (gate.id == "required_features") return QStringLiteral("所有基线与规则引用的特征须均可用。");
    if (gate.id == "comparison_domain") return QStringLiteral("规则所需差值、比值须可计算，比值基线须不为零。");
    if (gate.id == "speed") return QStringLiteral("有效测速、转速波动与工况范围须满足配置要求。");
    return utf8(gate.detail);
}
QString conditionState(const std::string& value) {
    if (value == "calibration_configuration_mismatch") return QStringLiteral("换算参数或单位不一致，未执行对比");
    if (value == "not_evaluated_quality_gate") return QStringLiteral("质量条件未通过，未执行区间判定");
    if (value == "feature_unavailable") return QStringLiteral("所需特征不可用");
    if (value == "comparison_unavailable") return QStringLiteral("对比值不可计算");
    if (value == "within_interval") return QStringLiteral("满足区间");
    if (value == "outside_interval") return QStringLiteral("不满足区间");
    return utf8(value);
}
QString evidence(const std::string& value) {
    if (value == "CAPDDR2 integrity, index continuity and sample timestamps passed validation.") return QStringLiteral("文件 CRC、记录序号与采样时间连续性校验通过。");
    if (value == "Time-domain features and window-averaged spectra are measured from this record.") return QStringLiteral("时域特征与分段平均频谱由本次记录计算。");
    if (value == "1X/2X/3X band energy is referenced to measured SPEED and the stated PPR.") return QStringLiteral("1X / 2X / 3X 频带以有效测速与指定 PPR 为参考。");
    if (value == "One or more channels contain ADC endpoint codes; inspect saturation before interpretation.") return QStringLiteral("发现 ADC 满量程端点码，需排查削顶或输入饱和。");
    if (value == "Synthetic source sidecar: development signal only; this record is not a physical machine measurement.") return QStringLiteral("来源说明标记为合成数据，仅用于软件验证。");
    if (value == "Source sidecar is invalid or unreadable; source confirmation is disabled until it is resolved.") return QStringLiteral("来源说明无效或无法读取，当前不能确认数据来源。");
    if (value == "Acceleration uses configured nominal sensitivity/gain; calibration is not confirmed.") return QStringLiteral("幅值按名义灵敏度与增益换算，尚未确认实际标定。");
    if (value == "V1/V3/V5 are storage channels; physical X/Y/Z direction requires a verified wiring record.") return QStringLiteral("CH1 / CH2 / CH3 对应 V1 / V3 / V5，物理轴向需接线记录确认。");
    if (value == "The file proves digital continuity, not sensor validity or analog anti-alias performance.") return QStringLiteral("文件数字校验不验证传感器状态与模拟抗混叠性能。");
    if (value == "PPR is unknown; only pulse Hz is available and RPM/order features are disabled.") return QStringLiteral("PPR 未知，当前仅显示脉冲频率，RPM 与转频特征暂停。");
    if (value == "Less than 90 percent of SPEED snapshots are valid; order features are disabled.") return QStringLiteral("有效测速不足 90%，转频特征暂停。");
    if (value == "Speed variation exceeds 5 percent; fixed-frequency order bands are disabled. Order tracking is not implemented.") return QStringLiteral("转速波动超过允许条件，固定转频带暂停；当前未实现阶次跟踪。");
    if (value == "Speed display merges multiple missing runs within a display bucket; intervening valid fragments are hidden. Full-record statistics are unchanged.") return QStringLiteral("测速概览合并了显示桶内多段缺测，全记录统计保留原值。");
    if (value == "No trained classifier or validated machine-specific thresholds are installed; no fault category or accuracy is claimed.") return QStringLiteral("尚无经验证的分类器或台架阈值，当前不提供最终故障分类或准确率。");
    if (value == "A 1X or 2X peak alone cannot uniquely identify imbalance, misalignment or looseness.") return QStringLiteral("单个 1X / 2X 谱峰不能唯一确定不平衡、不对中或松动。");
    if (value == "Envelope analysis and angle resampling are not implemented in this first analysis core.") return QStringLiteral("当前分析内核尚未实现包络分析与角度重采样。");
    if (value == "All profile rules and candidates are unvalidated; final classification remains not_classified.") return QStringLiteral("配置规则与候选均未经独立验证，最终分类保持未分类。");
    if (value == "A baseline draft captures observations only and is not proof of normal machine condition.") return QStringLiteral("单记录基线草稿仅保存观察值，不证明正常工况。");
    return utf8(value);
}
QString document(const QString& body) {
    return QStringLiteral("<html><head><style>body{color:#344255;font-family:sans-serif;font-size:14px;margin:0;}h3{font-size:15px;color:#263345;margin-top:12px;margin-bottom:8px;}p{margin:7px 0;line-height:1.5;}table{border-collapse:collapse;}th{font-weight:400;color:#687586;font-size:13px;}td,th{padding:9px 9px;border-bottom:1px solid #e4eaf1;}small{color:#687586;}ul{margin-top:5px;}li{margin-bottom:9px;}</style></head><body>") + body + QStringLiteral("</body></html>");
}
QString paragraph(const QString& value) { return QStringLiteral("<p>%1</p>").arg(value.toHtmlEscaped()); }
QTextBrowser* browser(const char* name) {
    auto* out = new QTextBrowser; out->setObjectName(QString::fromLatin1(name));
    out->setOpenExternalLinks(false); out->setOpenLinks(false); out->setFrameShape(QFrame::NoFrame);
    out->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);out->document()->setDocumentMargin(0);return out;
}
}

struct DetectionPane::Impl {
    QLabel *state = nullptr, *subtitle = nullptr, *profile = nullptr;
    QStackedWidget* stack = nullptr;
    std::array<QTextBrowser*, 3> documents{};
};

DetectionPane::DetectionPane(QWidget* parent) : QWidget(parent), impl_(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("detectionPane")); setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral(
        "QWidget#detectionPane{background:#ffffff;color:#344255;}QLabel{background:transparent;color:#344255;}"
        "QLabel#detectionState{font-size:22px;font-weight:600;color:#263345;}QLabel#detectionSubtitle,QLabel#detectionProfileSummary{font-size:13px;color:#687586;}"
        "QPushButton{min-height:44px;background:#ffffff;border:1px solid #ccd4df;border-radius:4px;color:#687586;padding:0 14px;font-size:14px;}"
        "QPushButton:checked{color:#245eaa;border-color:#96b2d6;background:#edf3fb;}QPushButton:hover{background:#f0f5fb;}"
        "QTextBrowser{background:#ffffff;border:0;color:#344255;}QScrollBar:vertical{background:#f5f7fa;width:16px;}"
        "QScrollBar::handle:vertical{background:#b7c2d0;border-radius:5px;min-height:44px;margin:3px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:none;}"));
    auto& d = *impl_; auto* layout = new QVBoxLayout(this); layout->setContentsMargins(16, 16, 16, 0); layout->setSpacing(12);
    auto* heading = new QVBoxLayout; heading->setContentsMargins(0, 0, 0, 0); heading->setSpacing(6);
    d.state = new QLabel; d.state->setObjectName(QStringLiteral("detectionState")); d.state->setWordWrap(true); heading->addWidget(d.state);
    d.subtitle = new QLabel; d.subtitle->setObjectName(QStringLiteral("detectionSubtitle")); d.subtitle->setWordWrap(true); heading->addWidget(d.subtitle);
    layout->addLayout(heading);
    auto* tabs = new QHBoxLayout; tabs->setSpacing(8); auto* group = new QButtonGroup(this); group->setExclusive(true);
    const std::array<QString, 3> titles{QStringLiteral("判定依据"), QStringLiteral("数值特征"), QStringLiteral("质量检查")};
    d.stack = new QStackedWidget; d.stack->setObjectName(QStringLiteral("detectionDocuments"));
    const std::array<const char*, 3> ids{"detectionEvidence", "detectionFeatures", "detectionGates"};
    for (int i = 0; i < 3; ++i) {
        auto* tab = new QPushButton(titles[i]); tab->setObjectName(QStringLiteral("detectionTab%1").arg(i)); tab->setCheckable(true); tab->setChecked(i == 0); group->addButton(tab); tabs->addWidget(tab);
        connect(tab, &QPushButton::clicked, this, [this, i] { impl_->stack->setCurrentIndex(i); });
        d.documents[i] = browser(ids[i]); d.stack->addWidget(d.documents[i]);
    }
    tabs->addStretch(); layout->addLayout(tabs); layout->addWidget(d.stack, 1);
    d.profile = new QLabel; d.profile->setObjectName(QStringLiteral("detectionProfileSummary"));
    d.profile->setMinimumHeight(28); d.profile->setMinimumWidth(0); d.profile->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->addWidget(d.profile); clear();
}

DetectionPane::~DetectionPane() = default;

void DetectionPane::clear() {
    auto& d = *impl_; d.state->setText(QStringLiteral("未分类"));
    d.subtitle->setText(QStringLiteral("打开记录后查看分析依据与质量条件")); d.profile->setText(QStringLiteral("尚未加载实验检测配置")); d.profile->setToolTip({});
    d.documents[0]->setHtml(document(paragraph(QStringLiteral("选择一条记录开始分析。"))));
    d.documents[1]->setHtml(document(paragraph(QStringLiteral("暂无数值特征。"))));
    d.documents[2]->setHtml(document(paragraph(QStringLiteral("暂无质量门控结果。"))));
}

void DetectionPane::setResult(const AnalysisResult* result, const DetectionResult* detection) {
    clear(); if (!result) return;
    auto& d = *impl_; const auto& r = *result;
    d.subtitle->setText(QStringLiteral("实验规则仅提供候选结果，最终分类尚未验证。"));
    if (!detection) d.subtitle->setText(QStringLiteral("数值分析已完成 · 尚未加载实验检测配置"));
    else {
        const auto& state = detection->status;
        if(state=="not_configured")d.subtitle->setText(QStringLiteral("尚未加载实验规则，可在“分析参数”中设置。"));
        if (state == "quality_gated") d.state->setText(QStringLiteral("条件未通过"));
        else if (state == "baseline_only") d.state->setText(QStringLiteral("基线观察"));
        else if (state == "candidate" && detection->candidate) d.state->setText(candidate(*detection->candidate) + QStringLiteral(" · 未验证"));
        else if (state == "conflict") d.state->setText(QStringLiteral("多个规则冲突"));
        else if (state == "inconclusive") d.state->setText(QStringLiteral("未命中规则"));
        else if (state == "invalid_profile") d.state->setText(QStringLiteral("配置无效"));
        else if (state == "no_record") d.state->setText(QStringLiteral("未分类"));
        if (!detection->profile_id.empty()) d.profile->setText(QStringLiteral("配置 %1    ·    台架 %2    ·    %3")
            .arg(utf8(detection->profile_id), utf8(detection->setup_id), detection->profile_status == "draft" ? QStringLiteral("草稿") : QStringLiteral("实验规则")));
        d.profile->setToolTip(d.profile->text());
    }
    QString evidenceBody = QStringLiteral("<h3>本次记录</h3>");
    const QString source = r.source_metadata.synthetic ? QStringLiteral("合成数据，仅用于软件验证") :
        r.config.signal_kind == SignalKind::VibrationConfirmed ? QStringLiteral("用户确认的机械振动记录") :
        r.config.signal_kind == SignalKind::TestSignal ? QStringLiteral("测试信号") : QStringLiteral("数据来源尚未确认");
    evidenceBody += paragraph(QStringLiteral("%1  ·  %2 s  ·  %3 kSPS / 通道").arg(source, number(r.duration_s), number(r.header.sample_rate_hz / 1000.0)));
    const auto unit=QString::fromStdString(amplitude_unit_symbol(r.config));
    evidenceBody += paragraph(r.config.amplitude_unit==AmplitudeUnit::Voltage ? QStringLiteral("幅值为原始 ADC 电压 V。"):r.config.calibration_confirmed ? QStringLiteral("用户已确认实际标定，幅值单位为 %1。").arg(unit) : QStringLiteral("幅值按用户输入参数换算为 %1，尚未确认实际标定。").arg(unit));
    evidenceBody += QStringLiteral("<h3>分析证据</h3><ul>");
    for (const auto& item : r.diagnosis.evidence) evidenceBody += QStringLiteral("<li>%1</li>").arg(evidence(item).toHtmlEscaped());
    evidenceBody += QStringLiteral("</ul>");
    if (detection && !detection->rules.empty()) {
        evidenceBody += QStringLiteral("<h3>实验规则</h3>");
        for (const auto& rule : detection->rules) {
            evidenceBody += QStringLiteral("<p><b>%1</b> · %2 · %3</p>").arg(html(rule.id), candidate(rule.label).toHtmlEscaped(), rule.matched ? QStringLiteral("命中，尚未验证") : QStringLiteral("未命中"));
            for (const auto& item : rule.conditions) {
                const auto& condition = item.condition;
                const QString comparison = condition.comparison == "delta" ? QStringLiteral("当前－基线") : condition.comparison == "ratio" ? QStringLiteral("当前 / 基线") : QStringLiteral("当前值");
                evidenceBody += QStringLiteral("<p><small>CH%1 %2 · %3 %4 · 区间 [%5, %6] · %7</small></p>")
                    .arg(condition.channel).arg(feature(condition.feature).toHtmlEscaped(), comparison, number(item.evaluated), number(condition.minimum), number(condition.maximum), conditionState(item.status).toHtmlEscaped());
            }
        }
    }
    evidenceBody += QStringLiteral("<h3>解释边界</h3><ul>");
    for (const auto& item : r.diagnosis.limitations) evidenceBody += QStringLiteral("<li>%1</li>").arg(evidence(item).toHtmlEscaped());
    if (detection) for (const auto& item : detection->limitations) evidenceBody += QStringLiteral("<li>%1</li>").arg(evidence(item).toHtmlEscaped());
    evidenceBody += QStringLiteral("</ul>"); d.documents[0]->setHtml(document(evidenceBody));

    QString values = QStringLiteral("<h3>时域统计</h3><table width='100%'><tr><th align='left'>通道</th><th align='right'>均值 / %1</th><th align='right'>交流 RMS / %1</th><th align='right'>峰值 / %1</th><th align='right'>峰值因子</th><th align='right'>峭度</th></tr>").arg(unit.toHtmlEscaped());
    for (int axis = 0; axis < 3; ++axis) {
        const auto& s = r.time_stats[axis];
        values += QStringLiteral("<tr><td>CH%1</td><td align='right'>%2</td><td align='right'>%3</td><td align='right'>%4</td><td align='right'>%5</td><td align='right'>%6</td></tr>")
            .arg(axis + 1).arg(number(s.mean), number(s.ac_rms), number(s.abs_peak), number(s.crest_factor), number(s.pearson_kurtosis));
    }
    values += QStringLiteral("</table><h3>转频频带 RMS / %1</h3><table width='100%'><tr><th align='left'>通道</th><th align='right'>1X</th><th align='right'>2X</th><th align='right'>3X</th></tr>").arg(unit.toHtmlEscaped());
    for (int axis = 0; axis < 3; ++axis) {
        values += QStringLiteral("<tr><td>CH%1</td>").arg(axis + 1);
        for (const auto& band : r.order_bands[axis]) values += QStringLiteral("<td align='right'>%1</td>").arg(number(band.rms));
        values += QStringLiteral("</tr>");
    }
    values += QStringLiteral("</table>");
    if (!r.config.calibration_confirmed&&r.config.amplitude_unit!=AmplitudeUnit::Voltage) values += paragraph(QStringLiteral("以上 %1 值按用户输入参数换算，尚未实际标定。").arg(unit));
    if (detection && !detection->baseline_comparisons.empty()) {
        values += QStringLiteral("<h3>基线对比</h3>");
        values += paragraph(QStringLiteral("幅值单位：基线 %1；当前 %2。无量纲特征保持无量纲。").arg(utf8(detection->baseline_amplitude_unit),utf8(detection->amplitude_unit)));
        values += QStringLiteral("<table width='100%'><tr><th align='left'>特征</th><th align='right'>基线</th><th align='right'>当前</th><th align='right'>差值</th><th align='right'>比值</th></tr>");
        for (const auto& comparison : detection->baseline_comparisons)
            values += QStringLiteral("<tr><td>CH%1 %2</td><td align='right'>%3</td><td align='right'>%4</td><td align='right'>%5</td><td align='right'>%6</td></tr>")
                .arg(comparison.channel).arg(feature(comparison.feature).toHtmlEscaped(), number(comparison.baseline), number(comparison.current), number(comparison.delta), number(comparison.ratio));
        values += QStringLiteral("</table>");
    }
    d.documents[1]->setHtml(document(values));

    QString gates;
    if (detection && !detection->gates.empty()) {
        const auto passed = std::count_if(detection->gates.begin(), detection->gates.end(), [](const auto& gate) { return gate.passed; });
        gates += QStringLiteral("<h3>%1 / %2 项质量条件通过</h3>").arg(passed).arg(detection->gates.size());
        gates += QStringLiteral("<table width='100%'>");
        for (const auto& gate : detection->gates) gates += QStringLiteral("<tr><td width='65' style='color:%1'>%2</td><td><b>%3</b><br><small>%4</small></td></tr>")
            .arg(gate.passed ? QStringLiteral("#4a756c") : QStringLiteral("#a77b39"), gate.passed ? QStringLiteral("通过") : QStringLiteral("未通过"), gateTitle(gate.id).toHtmlEscaped(), gateDetail(gate).toHtmlEscaped());
        gates += QStringLiteral("</table>");
    } else gates += paragraph(QStringLiteral("尚未装载实验检测配置，未执行配置对应的质量门控。"));
    if (!r.warnings.empty()) {
        gates += QStringLiteral("<h3>记录说明</h3><ul>");
        for (const auto& item : r.warnings) gates += QStringLiteral("<li>%1</li>").arg(evidence(item).toHtmlEscaped());
        gates += QStringLiteral("</ul>");
    }
    d.documents[2]->setHtml(document(gates));
}

} // namespace vibration::workbench
