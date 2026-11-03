#pragma once
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <memory>
#include <array>
#include <optional>
#include <QVector>
#include "plot_widget.hpp"
#include "vibration/core.hpp"
#include "vibration/detection.hpp"
class QLabel;
class QPushButton;
class QTextBrowser;
class QThread;
class QStackedWidget;
class QDoubleSpinBox;
class QCheckBox;
class QListWidget;
class QTabWidget;
class QVBoxLayout;
namespace vibration::gui {
class PlotWidget;
class AcquisitionPanel;
class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(const AnalysisConfig& initialConfig = {}, QWidget* parent = nullptr);
    ~MainWindow() override;
    void openCapture(const QString& path);
    void setDataDirectory(const QString& path);
    bool isAnalyzing() const;
    bool setPage(const QString& name);
    bool saveResult(const QString& path, bool csv, QString* error = nullptr) const;
    bool loadDetectionProfile(const QString& path, QString* error = nullptr);
    void clearDetectionProfile();
    bool setDetectionSetupId(const QString& setupId, QString* error = nullptr);
    bool setDetectionMeasurementConditions(SignalKind source, bool calibrationConfirmed, QString* error = nullptr);
    bool saveDetectionBaselineDraft(const QString& path, const QString& profileId = QStringLiteral("single_record_draft"), QString* error = nullptr) const;
private:
    void chooseFile();
    void chooseDirectory();
    void refreshFiles();
    void openDemo();
    void editParameters();
    void beginAnalysis();
    void showResult();
    void updateWaveform();
    void selectWaveCursor(double time);
    void adjustWaveRange(double scale, double shift);
    void exportResult(bool csv = false);
    void showDetails();
    void clearResults();
    void setBusy(bool busy);
    void setBanner(const QString& text, const QString& style);
    void updateChannels();
    void updateDetection();
    void updateDetectionSummary();
    void chooseDetectionProfile();
    void createBaselineDraft();
    std::vector<std::string> protectedOutputPaths() const;
    AnalysisConfig config_;
    std::unique_ptr<AnalysisResult> result_;
    std::shared_ptr<const Capture> capture_;
    QString currentPath_, directory_;
    bool synthetic_ = false;
    QString metadataNotice_;
    std::optional<DetectionProfile> detectionProfile_;
    DetectionResult detectionResult_;
    QString detectionProfilePath_, detectionSetupId_, detectionProfileError_;
    QStringList detectionInputPaths_;
    QThread* worker_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QWidget* contextSidebar_ = nullptr;
    QLabel* sidebarRecord_ = nullptr;
    QStackedWidget* recordModes_ = nullptr;
    AcquisitionPanel* acquisition_ = nullptr;
    QTabWidget* detectionTabs_ = nullptr;
    std::array<QPushButton*, 5> navigation_{};
    std::array<QCheckBox*, 3> waveChannels_{}, spectrumChannels_{};
    std::array<QPushButton*, 4> waveNavigation_{};
    QDoubleSpinBox* rangeStart_ = nullptr;
    QDoubleSpinBox* rangeEnd_ = nullptr;
    QPushButton *openButton_ = nullptr, *directoryButton_ = nullptr, *refreshButton_ = nullptr,
        *demoButton_ = nullptr, *parametersButton_ = nullptr, *exportButton_ = nullptr,
        *csvButton_ = nullptr, *detailsButton_ = nullptr, *rangeButton_ = nullptr, *fullButton_ = nullptr;
    QPushButton *loadProfileButton_ = nullptr, *clearProfileButton_ = nullptr, *baselineButton_ = nullptr;
    QPushButton *waveRangePicker_ = nullptr, *exportMenuButton_ = nullptr, *detectionConfigButton_ = nullptr;
    QLabel *banner_ = nullptr, *calibration_ = nullptr, *fileLabel_ = nullptr, *sampleInfo_ = nullptr,
        *metrics_ = nullptr, *speedHeading_ = nullptr, *diagnosis_ = nullptr, *features_ = nullptr,
        *footer_ = nullptr, *fileMetadata_ = nullptr, *directoryLabel_ = nullptr, *recent_ = nullptr,
        *waveCursor_ = nullptr, *frequencyCursor_ = nullptr, *frequencyPeaks_ = nullptr,
        *speedStats_ = nullptr, *bandTable_ = nullptr;
    QLabel *detectionState_ = nullptr, *detectionProfileLabel_ = nullptr, *detectionQuality_ = nullptr;
    QLabel *frequencyReference_ = nullptr, *detectionSummaryHeading_ = nullptr,
        *detectionConfigSummary_ = nullptr, *detectionConfigMeta_ = nullptr;
    QVBoxLayout* detectionCards_ = nullptr;
    QListWidget* files_ = nullptr;
    QListWidget* sidebarFiles_ = nullptr;
    QTextBrowser* evidence_ = nullptr;
    QTextBrowser* detectionEvidence_ = nullptr;
    PlotWidget *waveform_ = nullptr, *spectrum_ = nullptr, *speed_ = nullptr;
    std::array<PlotWidget*, 3> waveSingle_{};
    QStringList recentPaths_;
    struct WavePreview {
        double start = 0, end = 0;
        QVector<PlotSeries> series;
    };
    QVector<WavePreview> wavePreviews_;
    std::optional<double> appliedWaveStart_, appliedWaveEnd_, waveCursorTime_;
    quint64 previewBuilds_ = 0, previewCacheHits_ = 0;
};
} // namespace vibration::gui
