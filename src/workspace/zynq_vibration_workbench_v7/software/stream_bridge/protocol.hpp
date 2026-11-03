#pragma once
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <cmath>
#include <set>
#include <stdexcept>
namespace streambridge {
struct Failure : std::runtime_error {
    QString code;
    Failure(QString c, QString m) : std::runtime_error(m.toUtf8().constData()), code(std::move(c)) {}
};
[[noreturn]] void fail(const QString& code, const QString& message) { throw Failure(code, message); }
void require(bool ok, const QString& code, const QString& message) { if (!ok) fail(code, message); }
bool integer(const QJsonValue& value, double lo, double hi) {
    const auto number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= lo && number <= hi && std::floor(number) == number;
}
void keys(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        require(allowed.contains(it.key()), "INVALID_REQUEST", "Unknown field: " + it.key());
}
// QJsonDocument validates syntax before this bounded duplicate/depth pass.
class UniqueKeys {
    const QByteArray& bytes_; int position_ = 0;
    void whitespace() { while (position_ < bytes_.size() && QByteArray(" \t\r\n").contains(bytes_[position_])) ++position_; }
    QString string(bool decode) {
        const int start = position_++;
        while (position_ < bytes_.size()) {
            const int end = bytes_.indexOf('"', position_); if (end < 0) break;
            int slash = 0; for (int i = end - 1; i >= position_ && bytes_[i] == '\\'; --i) ++slash;
            position_ = end + 1; if (!(slash & 1)) break;
        }
        return decode ? QJsonDocument::fromJson("[" + bytes_.mid(start, position_ - start) + "]").array()[0].toString() : QString();
    }
    void value(int depth) {
        require(depth <= 16, "INVALID_JSON", "JSON nesting limit exceeded"); whitespace(); if (position_ >= bytes_.size()) return;
        const char c = bytes_[position_];
        if (c == '{') {
            ++position_; whitespace(); std::set<QString> seen;
            while (position_ < bytes_.size() && bytes_[position_] != '}') {
                whitespace(); require(seen.insert(string(true)).second, "DUPLICATE_KEY", "Duplicate JSON key");
                whitespace(); ++position_; value(depth + 1); whitespace();
                if (position_ < bytes_.size() && bytes_[position_] == ',') { ++position_; continue; } break;
            }
            ++position_;
        } else if (c == '[') {
            ++position_; whitespace();
            while (position_ < bytes_.size() && bytes_[position_] != ']') {
                value(depth + 1); whitespace(); if (position_ < bytes_.size() && bytes_[position_] == ',') { ++position_; continue; } break;
            }
            ++position_;
        } else if (c == '"') string(false);
        else while (position_ < bytes_.size() && !QByteArray(",]} \r\n\t").contains(bytes_[position_])) ++position_;
    }
public:
    explicit UniqueKeys(const QByteArray& b) : bytes_(b) {}
    void check() { value(0); }
};
QJsonObject parse(const QByteArray& bytes) {
    QJsonParseError error; auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "INVALID_JSON", "Expected a UTF-8 JSON object");
    UniqueKeys(bytes).check(); return document.object();
}

inline QJsonValue numeric(double n) { return std::isfinite(n) ? QJsonValue(n) : QJsonValue(QJsonValue::Null); }
inline QString number(quint64 n) { return QString::number(n); }
inline quint64 decimal(const QJsonValue& value) {
    require(value.isString() && QRegularExpression("^(0|[1-9][0-9]{0,19})$").match(value.toString()).hasMatch(), "INVALID_COUNTER", "Expected canonical 64-bit decimal string");
    bool ok=false; auto result=value.toString().toULongLong(&ok); require(ok,"INVALID_COUNTER","Counter overflow"); return result;
}
inline quint16 u16(const unsigned char* p) { return quint16(p[0]) | (quint16(p[1])<<8); }
inline quint32 u32(const unsigned char* p) { return quint32(p[0]) | (quint32(p[1])<<8) | (quint32(p[2])<<16) | (quint32(p[3])<<24); }
inline quint64 u64(const unsigned char* p) { return quint64(u32(p)) | (quint64(u32(p+4))<<32); }
inline bool validSource(const QJsonObject& o) {
    if(!o.value("synthetic").isBool()||!o.value("hardware_confirmed").isBool())return false;
    const auto source=o.value("source").toString();const bool synthetic=o.value("synthetic").toBool(),hardware=o.value("hardware_confirmed").toBool();
    return (source=="test_fixture"&&synthetic&&!hardware)||(source=="hardware_test"&&synthetic&&hardware)||(source=="adc_unconfirmed"&&!synthetic&&hardware);
}
}

