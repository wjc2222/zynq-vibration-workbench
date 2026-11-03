#pragma once
#include <QMainWindow>
#include <memory>
#include <optional>
#include <array>
#include <QJsonObject>
#include <QVector>
#include "vibration/core.hpp"
#include "vibration/detection.hpp"
class QThread;class QLabel;class QPushButton;class QStackedWidget;class QListWidget;class QComboBox;class QCheckBox;
class QCloseEvent;
class QDialog;
namespace vibration::workbench {
class AnalysisPane;class CandidatePanel;class CapturePage;class FileTitle;class SensitivitySettings;
class Window final : public QMainWindow {
public:
    explicit Window(QWidget* parent=nullptr,const QString& dataRoot=QStringLiteral("/mnt/capture-data"));
    ~Window() override;
    void openCapture(const QString& path);
    void setDirectory(const QString& path);
    bool isAnalyzing() const;
    void setView(int view);
    void showAcquisition();
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    void chooseRecord();
    void chooseFolder();
    void refreshRecords();
    void updateRecordAnnotation();
    void saveSelectedRecordLabel();
    void addSelectedReference();
    void startLabeledCapture();
    void showRecords();
    void syncCaptureControls();
    void setDocumentState(const QString&);
    void setFootnote(const QString&);
    void analyze();
    void updateDetection();
    void settings();
    void exportResult();
    void setBusy(bool busy);
    void liveFrame(const QJsonObject& frame);
    void captureState(const QJsonObject& state);
    void loadCalibrationSettings();
    bool saveCalibrationSettings(const AnalysisConfig& config,QString* error);
    std::vector<std::string> protectedPaths() const;
    AnalysisConfig config_;
    std::shared_ptr<const Capture> capture_;
    std::unique_ptr<AnalysisResult> result_;
    std::optional<DetectionProfile> profile_;
    DetectionResult detection_;
    QString path_,directory_,recordRoot_,profilePath_,profileError_,setupId_;
    QString dataRoot_,calibrationSettingsPath_;
    QStringList protectedHistory_;
    QString liveJob_,pendingCompletedPath_;
    QVector<QJsonObject> pendingLiveFrames_;
    bool captureActive_=false,cancelRequested_=false;
    bool previewQuarantined_=false;
    bool closeAfterStop_=false;
    int lastPlotView_=0;
    QThread* worker_=nullptr;
    QWidget* tabs_;
    QWidget *curveControls_,*captureLabelControls_;
    QDialog *captureDialog_,*recordDialog_,*sensitivityDialog_,*dataMenuDialog_,*optionsDialog_;
    FileTitle *title_,*folderTitle_,*plan_;
    QLabel *state_,*footnote_,*device_;
    QListWidget* records_;
    QComboBox *recordFilter_,*recordLabelEditor_,*captureLabelChoice_;
    QLabel *recordAnnotationStatus_;
    QCheckBox* orderMarkers_;
    QPushButton *acquisitionTab_,*applyRecordLabel_,*addReferenceButton_,*sensitivityButton_;
    QStackedWidget* pages_;
    AnalysisPane* analysis_;
    SensitivitySettings* sensitivity_;
    CandidatePanel* detectionPage_;
    CapturePage* capturePage_;
    QPushButton *export_,*settings_,*open_,*folder_,*start_,*acquire_,*recordBrowser_,*reconnect_,*dataMenuButton_,*optionsButton_;
    std::array<QPushButton*,4> viewButtons_{};
};
}
