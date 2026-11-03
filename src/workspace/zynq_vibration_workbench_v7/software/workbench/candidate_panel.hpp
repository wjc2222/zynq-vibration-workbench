#pragma once
#include "vibration/candidate_model.hpp"
#include "vibration/detection.hpp"
#include <QWidget>
#include <QJsonObject>
#include <QStringList>
#include <memory>
namespace vibration::workbench {
enum class CandidateWritePurpose { Export, ActiveModel, ActiveLibrary, ActiveValidation };
// Path-only policy, also exercised with isolated sentinel files. It does not
// load data, admit references or make a mechanical-source declaration.
std::vector<std::string> candidate_protected_output_paths(const QStringList& captures,
    const QStringList& configurationSources, const QString& activeModel,
    const QString& activeLibrary, CandidateWritePurpose = CandidateWritePurpose::Export);
// Read-only UI explanation for reference models. Does not alter the inference
// gates, fit a model, or treat a file's label as a query feature.
std::array<unsigned,4> candidate_reference_coverage(const CandidateWindow&,const CandidateModel&);
class CandidatePanel final:public QWidget {
public:
    explicit CandidatePanel(QWidget* parent=nullptr);
    ~CandidatePanel() override;
    void clear();
    void setStorageRoot(const QString& root);
    void setCapture(std::shared_ptr<const Capture>,const AnalysisConfig&);
    void setResult(const AnalysisResult*,const DetectionResult*);
    void acceptLive(const QJsonObject&,const AnalysisConfig&);
    void acceptSegment(const QString&,const AnalysisConfig&);
    void invalidateLive(const QString&);
    // Accepts an explicit selection for worker CRC/label validation and feature
    // extraction. In the open library, source confirmation may be pending;
    // closing that dialog cancels the pending selection. Results are visible there.
    bool addReferenceFile(const QString& path,const QString& labelKey);
    void showReferenceLibrary();
    std::string candidateJson()const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
