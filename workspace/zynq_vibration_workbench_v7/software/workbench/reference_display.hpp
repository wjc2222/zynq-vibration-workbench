#pragma once
#include "vibration/reference_library.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <algorithm>
#include <limits>

namespace vibration::workbench {
// Keep relative source directories: SD segments commonly all use data.BIN.
// This formats provenance only; it does not ingest or train on the sample.
inline QString referenceSourceName(const QString& storageRoot,const std::string& path){
    const auto source=QFileInfo(QString::fromUtf8(path.data(),int(path.size()))).absoluteFilePath();
    return storageRoot.isEmpty()?QDir::cleanPath(source):QDir(storageRoot).relativeFilePath(source);
}
inline QString referenceMeasuredRpm(const ReferenceSample& sample){
    if(sample.windows.empty())return QStringLiteral("—");
    double minimum=std::numeric_limits<double>::max(),maximum=0;
    for(const auto&window:sample.windows){minimum=std::min(minimum,window.rpm_min);maximum=std::max(maximum,window.rpm_max);}
    if(maximum-minimum<.05)return QString::number(minimum,'f',1);
    return QStringLiteral("%1–%2").arg(minimum,0,'f',1).arg(maximum,0,'f',1);
}
} // namespace vibration::workbench
