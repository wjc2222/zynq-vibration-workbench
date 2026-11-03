#include "record_labels.hpp"

#include <vibration/core.hpp>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <stdexcept>

namespace vibration::workbench {
namespace {
constexpr qint64 maxAnnotationBytes = 4 * 1024 * 1024;
constexpr qint64 maxSourceBytes = 1024 * 1024;
const QString schema = QStringLiteral("vibration.operator-record-label.v1");
void require(bool condition, const QString& message) {
    if (!condition) throw std::runtime_error(message.toUtf8().constData());
}
quint32 word(const QByteArray& bytes, int offset) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}
bool validLabel(RecordLabel label) { return !recordLabelKey(label).isEmpty(); }
struct RecordGuard {
    QJsonObject identity;
    bool synthetic = false;
    QString sourceStatus = QStringLiteral("unconfirmed");
};
QByteArray readFile(const QString& path, qint64 limit) {
    QFileInfo info(path);
    require(info.isFile() && !info.isSymLink(), QStringLiteral("文件不存在或不是普通文件：%1").arg(path));
    require(info.size() >= 0 && info.size() <= limit, QStringLiteral("文件过大：%1").arg(path));
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取 %1：%2").arg(path, file.errorString()));
    auto bytes = file.readAll();
    require(file.error() == QFile::NoError && bytes.size() == info.size(), QStringLiteral("文件读取不完整：%1").arg(path));
    return bytes;
}
QJsonObject jsonObject(const QByteArray& bytes, const QString& path) {
    QJsonParseError parse;
    auto document = QJsonDocument::fromJson(bytes, &parse);
    require(parse.error == QJsonParseError::NoError && document.isObject(),
        QStringLiteral("JSON 内容无效：%1（%2）").arg(path, parse.errorString()));
    return document.object();
}
RecordGuard inspectRecord(const QString& path) {
    const QFileInfo info(path);
    require(info.isAbsolute() && info.isFile() && !info.isSymLink() && info.suffix().compare("BIN", Qt::CaseInsensitive) == 0,
        QStringLiteral("标签只支持已保存的普通 BIN 文件：%1").arg(path));
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取采集头：%1").arg(file.errorString()));
    auto header = file.read(4096);
    require(header.size() == 4096 && file.error() == QFile::NoError, QStringLiteral("BIN 采集头读取不完整。"));
    const bool stream = header.left(8) == QByteArray("CAPSTRM1", 8);
    require(stream || header.left(8) == QByteArray("CAPDDR2\0", 8), QStringLiteral("BIN 格式不是 CAPDDR2/CAPSTRM1。"));
    const auto crc = word(header, 100);
    std::fill(header.begin() + 100, header.begin() + 104, 0);
    require(vibration::crc32(reinterpret_cast<const std::uint8_t*>(header.constData()), stream ? 4096 : 128) == crc,
        QStringLiteral("BIN 采集头 CRC 校验失败。"));
    require(word(header, 8) == (stream ? 1u : 2u) && word(header, 12) == 4096 && word(header, 16) == 32 &&
        word(header, 20) == 2 && word(header, 44) == 0 && word(header, 56) == 0 && word(header, 36) > 0 &&
        word(header, 32) == word(header, 36) && quint64(word(header, 36)) * 32 == word(header, 40) &&
        info.size() == 4096 + qint64(word(header, 40)), QStringLiteral("BIN 记录尚未完整保存或头字段无效。"));
    if (stream) require(!(word(header, 148) & ~1u), QStringLiteral("不支持的流式来源标记。"));
    RecordGuard guard;
    guard.synthetic = stream ? bool(word(header, 148) & 1u) : bool(word(header, 48) & (1u << 6));
    guard.identity = QJsonObject{{"file_name", info.fileName()}, {"bytes", QString::number(info.size())},
        {"header_crc32", QString::number(crc)}, {"payload_crc32", QString::number(word(header, 96))}};
    const auto metaPath = QDir(info.absolutePath()).filePath(info.completeBaseName() + ".meta.json");
    const QFileInfo metaInfo(metaPath);
    if (metaInfo.exists() || metaInfo.isSymLink()) {
        const auto bytes = readFile(metaPath, maxSourceBytes);
        const auto meta = jsonObject(bytes, metaPath);
        if (meta.contains("synthetic")) require(meta.value("synthetic").isBool(), QStringLiteral("原始来源元数据的 synthetic 标记无效。"));
        if (meta.contains("source_kind")) require(meta.value("source_kind").isString(), QStringLiteral("原始来源元数据的 source_kind 标记无效。"));
        // Conservatively retain any synthetic declaration even if a malformed
        // duplicate key would otherwise hide it in QJsonObject's last value.
        const QRegularExpression syntheticTrue(QStringLiteral("\\\"synthetic\\\"\\s*:\\s*true\\b"));
        const QRegularExpression syntheticKind(QStringLiteral("\\\"source_kind\\\"\\s*:\\s*\\\"synthetic\\\""));
        guard.synthetic = guard.synthetic || meta.value("synthetic").toBool() || meta.value("source_kind").toString() == "synthetic" ||
            meta.value("backend").toString() == "test_fixture" || meta.value("actual_source").toString() == "host_fixture" ||
            meta.value("actual_source").toString() == "hardware_self_test" || meta.value("hardware_test_source").toBool() ||
            syntheticTrue.match(QString::fromUtf8(bytes)).hasMatch() || syntheticKind.match(QString::fromUtf8(bytes)).hasMatch();
    }
    if (guard.synthetic) guard.sourceStatus = QStringLiteral("synthetic");
    return guard;
}
bool utcTime(const QJsonValue& value) {
    if (!value.isString()) return false;
    auto text = value.toString();
    return text.endsWith('Z') && QDateTime::fromString(text, Qt::ISODateWithMs).isValid();
}
bool validEntry(const QJsonObject& entry) {
    RecordLabel ignored;
    return entry.value("source") == "operator" && recordLabelFromKey(entry.value("label").toString(), &ignored) &&
        utcTime(entry.value("updated_at")) && entry.value("clock_valid").isBool() &&
        entry.value("clock_valid").toBool() == (QDateTime::fromString(entry.value("updated_at").toString(), Qt::ISODateWithMs).date().year() >= 2020) &&
        entry.value("session_id").isString() && entry.value("session_id").toString().size() <= 128;
}
RecordLabelAnnotation load(const QString& path, const RecordGuard& guard) {
    RecordLabelAnnotation result;
    result.synthetic = guard.synthetic; result.sourceStatus = guard.sourceStatus;
    const auto sidecar = recordLabelSidecarPath(path);
    const QFileInfo info(sidecar);
    if (!info.exists() && !info.isSymLink()) return result;
    const auto value = jsonObject(readFile(sidecar, maxAnnotationBytes), sidecar);
    require(value.value("schema") == schema && value.value("record").isObject() && value.value("record").toObject() == guard.identity,
        QStringLiteral("人工标签的版本或关联 BIN 身份不匹配：%1").arg(sidecar));
    require(validEntry(value) && value.value("history").isArray() && value.value("synthetic_record").isBool() &&
        value.value("purpose") == "dataset_organization_only" && value.value("inference_input").isBool() && !value.value("inference_input").toBool(),
        QStringLiteral("人工标签内容无效：%1").arg(sidecar));
    result.history = value.value("history").toArray();
    require(!result.history.isEmpty(), QStringLiteral("人工标签缺少操作历史：%1").arg(sidecar));
    for (const auto& entry : result.history) require(entry.isObject() && validEntry(entry.toObject()), QStringLiteral("人工标签历史无效：%1").arg(sidecar));
    const auto latest = result.history.last().toObject();
    for (const auto& key : {"label", "source", "updated_at", "clock_valid", "session_id"})
        require(latest.value(key) == value.value(key), QStringLiteral("人工标签与最新操作历史不一致：%1").arg(sidecar));
    require(recordLabelFromKey(value.value("label").toString(), &result.label), QStringLiteral("未知人工工况标签。"));
    result.exists = true; result.updatedAt = value.value("updated_at").toString(); result.sessionId = value.value("session_id").toString();
    result.clockValid = value.value("clock_valid").toBool();
    // Source warnings are monotonic. Removing original metadata cannot erase a
    // synthetic declaration that was present when this annotation was saved.
    result.synthetic = result.synthetic || value.value("synthetic_record").toBool();
    if (result.synthetic) result.sourceStatus = QStringLiteral("synthetic");
    const auto trainingUse = value.value("training_use").toString();
    require(trainingUse == "prohibited_synthetic" || trainingUse == "requires_source_validation", QStringLiteral("人工标签缺少有效训练用途保护。"));
    require(!value.value("synthetic_record").toBool() || trainingUse == "prohibited_synthetic", QStringLiteral("合成记录的人工标签未保留训练用途保护。"));
    return result;
}
} // namespace

QString recordLabelKey(RecordLabel label) {
    switch (label) {
    case RecordLabel::Unlabeled: return QStringLiteral("unlabeled");
    case RecordLabel::Normal: return QStringLiteral("normal");
    case RecordLabel::Looseness: return QStringLiteral("looseness");
    case RecordLabel::Misalignment: return QStringLiteral("misalignment");
    case RecordLabel::Imbalance: return QStringLiteral("imbalance");
    }
    return {};
}
QString recordLabelText(RecordLabel label) {
    switch (label) {
    case RecordLabel::Unlabeled: return QStringLiteral("未标注");
    case RecordLabel::Normal: return QStringLiteral("正常");
    case RecordLabel::Looseness: return QStringLiteral("松动");
    case RecordLabel::Misalignment: return QStringLiteral("不对中");
    case RecordLabel::Imbalance: return QStringLiteral("不平衡");
    }
    return QStringLiteral("标签无效");
}
bool recordLabelFromKey(const QString& key, RecordLabel* label) {
    for (auto candidate : {RecordLabel::Unlabeled, RecordLabel::Normal, RecordLabel::Looseness, RecordLabel::Misalignment, RecordLabel::Imbalance}) {
        if (key == recordLabelKey(candidate)) { if (label) *label = candidate; return true; }
    }
    return false;
}
QString recordLabelSidecarPath(const QString& path) {
    QFileInfo info(path);
    return QDir(info.absolutePath()).filePath(info.completeBaseName() + ".labels.json");
}
bool loadRecordLabel(const QString& path, RecordLabelAnnotation* annotation, QString* error) {
    if (error) error->clear();
    if (annotation) *annotation = {};
    try {
        require(annotation, QStringLiteral("人工标签读取缺少输出对象。"));
        *annotation = load(path, inspectRecord(path)); return true;
    } catch (const std::exception& e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}
bool saveRecordLabel(const QString& path, RecordLabel label, const QString& sessionId, RecordLabelAnnotation* annotation, QString* error) {
    if (error) error->clear();
    if (annotation) *annotation = {};
    try {
        require(validLabel(label) && sessionId.size() <= 128, QStringLiteral("人工标签或采集会话标识无效。"));
        const auto guard = inspectRecord(path);
        const auto sidecar = recordLabelSidecarPath(path);
        QLockFile lock(sidecar + ".lock");
        require(lock.tryLock(0), QStringLiteral("此记录的人工标签正在被其他操作更新，或目录不可写：%1").arg(sidecar));
        auto current = load(path, guard);
        const auto time = QDateTime::currentDateTimeUtc();
        const auto timestamp = time.toString(Qt::ISODateWithMs);
        const bool clockValid = time.date().year() >= 2020;
        QJsonObject entry{{"label", recordLabelKey(label)}, {"source", "operator"}, {"updated_at", timestamp}, {"clock_valid", clockValid}, {"session_id", sessionId}};
        current.history.append(entry);
        auto value = entry;
        value["schema"] = schema; value["record"] = guard.identity; value["history"] = current.history;
        value["purpose"] = "dataset_organization_only"; value["inference_input"] = false;
        value["clock_notice"] = clockValid ? "system_clock_at_or_after_2020_not_independent_measurement_time" : "system_clock_unset_do_not_interpret_as_measurement_date";
        value["synthetic_record"] = current.synthetic;
        value["training_use"] = current.synthetic ? "prohibited_synthetic" : "requires_source_validation";
        const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
        require(bytes.size() <= maxAnnotationBytes, QStringLiteral("人工标签历史已达到大小限制，请先归档操作历史。"));
        QSaveFile output(sidecar); output.setDirectWriteFallback(false);
        require(output.open(QIODevice::WriteOnly), QStringLiteral("无法创建人工标签文件：%1").arg(output.errorString()));
        require(output.write(bytes) == bytes.size(), QStringLiteral("人工标签未完整写入：%1").arg(output.errorString()));
        require(output.commit(), QStringLiteral("人工标签未能原子保存：%1").arg(output.errorString()));
        current.exists = true; current.label = label; current.updatedAt = timestamp; current.clockValid = clockValid; current.sessionId = sessionId;
        if (annotation) *annotation = current;
        return true;
    } catch (const std::exception& e) { if (error) *error = QString::fromUtf8(e.what()); return false; }
}
} // namespace vibration::workbench
