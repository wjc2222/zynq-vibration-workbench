#pragma once
#include <QApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGlyphRun>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextLayout>

namespace vibration::workbench::qa {
inline QStringList configureFonts(QApplication& app) {
    QStringList registered;
    for (const auto path : {"/mnt/c/Windows/Fonts/segoeui.ttf", "/mnt/c/Windows/Fonts/seguisb.ttf", "/mnt/c/Windows/Fonts/msyh.ttc", "/mnt/c/Windows/Fonts/msyhbd.ttc",
        "/usr/share/fonts/truetype/workbench/segoeui.ttf", "/usr/share/fonts/truetype/workbench/seguisb.ttf", "/usr/share/fonts/truetype/workbench/msyh.ttc", "/usr/share/fonts/truetype/workbench/msyhbd.ttc"})
        if (QFileInfo::exists(path) && QFontDatabase::addApplicationFont(path) >= 0) registered.push_back(path);
    // Match the verified production constructor. Starting with default QFont
    // then setFamilies can merge the host's default family into the request.
    QFont font(QStringLiteral("Segoe UI,Microsoft YaHei UI,Microsoft YaHei,Noto Sans CJK SC,sans-serif"));
    font.setPixelSize(14); font.setWeight(QFont::Normal); font.setStyleStrategy(QFont::PreferAntialias);
    font.setHintingPreference(QFont::PreferDefaultHinting); app.setFont(font); return registered;
}
inline QJsonObject fontAudit(const QFont& font) {
    QTextLayout text(QStringLiteral("振动分析 CH1 123"), font); text.beginLayout(); auto line = text.createLine(); line.setLineWidth(1000); text.endLayout();
    QJsonArray families, runs;
    for (const auto& run : text.glyphRuns()) {
        if (!families.contains(run.rawFont().familyName())) families.append(run.rawFont().familyName());
        QJsonArray positions; for (const auto& position : run.positions()) positions.append(position.x());
        runs.append(QJsonObject{{"family", run.rawFont().familyName()},
            {"name_table_sha256", QString::fromLatin1(QCryptographicHash::hash(run.rawFont().fontTable("name"), QCryptographicHash::Sha256).toHex())},
            {"positions_x", positions}});
    }
    return {{"sample", text.text()}, {"families", families}, {"runs", runs}, {"pixel_size", font.pixelSize()}};
}
} // namespace vibration::workbench::qa
