#pragma once

#include <QJsonArray>
#include <QString>

namespace vibration::workbench {

// Operator annotations are dataset targets only. They are deliberately kept out
// of Capture, AnalysisConfig, feature extraction and prediction inputs.
enum class RecordLabel { Unlabeled, Normal, Looseness, Misalignment, Imbalance };
QString recordLabelKey(RecordLabel label);
QString recordLabelText(RecordLabel label);
bool recordLabelFromKey(const QString& key, RecordLabel* label);
QString recordLabelSidecarPath(const QString& recordPath);

struct RecordLabelAnnotation {
    RecordLabel label = RecordLabel::Unlabeled;
    bool exists = false;
    QString updatedAt;
    bool clockValid = false;
    QString sessionId;
    QJsonArray history;
    bool synthetic = false;
    // This is a source guard, not a declaration that a measurement is physical.
    QString sourceStatus = QStringLiteral("unconfirmed");
};

// Missing labels are a successful Unlabeled result. Invalid/unreadable labels
// and record/source errors return false with an actionable diagnostic.
bool loadRecordLabel(const QString& recordPath, RecordLabelAnnotation* annotation,
                     QString* error = nullptr);

// An atomic, independent .labels.json sidecar; BIN and .meta.json are read-only.
// Previous operator entries remain in history. sessionId is empty for later
// manual annotation and fixed to the capture job ID for acquired segments.
// This never grants mechanical-training eligibility: synthetic is prohibited;
// other records still require independent integrity/source validation.
bool saveRecordLabel(const QString& recordPath, RecordLabel label,
                     const QString& sessionId = {}, RecordLabelAnnotation* annotation = nullptr,
                     QString* error = nullptr);

} // namespace vibration::workbench
