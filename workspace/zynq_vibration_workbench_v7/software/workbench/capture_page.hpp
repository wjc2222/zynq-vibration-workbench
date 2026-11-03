#pragma once
#include "record_labels.hpp"
#include <QWidget>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <functional>
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QTimer;
class QGridLayout;
namespace vibration::workbench {
class CaptureClient;
class CapturePage final : public QWidget {
    Q_OBJECT
public:
    explicit CapturePage(std::function<QDoubleSpinBox*()> numberFactory,QWidget* parent=nullptr,
                         const QString& dataRoot=QStringLiteral("/mnt/capture-data"));
    ~CapturePage() override;
    void setServiceCommand(const QString& program,const QStringList& arguments={});
    void setStreaming(bool enabled);
    void setAnalysisBusy(bool busy);
    // The main shell owns the visible start/stop actions. These methods use
    // the same validated plan and state machine as the standalone page.
    void ensureConnected();
    void reconnect();
    void startCapture();
    void cancelCurrent();
    void setParameterOnly(bool enabled);
    QString planSummary() const;
    QString outputDirectory() const;
    QString progressSummary() const;
    // A selection is retained across sessions. startCapture freezes it for all
    // segments; callers cannot alter either selection while capture is active.
    bool setNextRecordLabel(RecordLabel label);
    RecordLabel nextRecordLabel() const { return nextRecordLabel_; }
    RecordLabel activeRecordLabel() const { return activeRecordLabel_; }
signals:
    void captureReady(const QString& path);
    void statusChanged(const QString& status);
    void liveFrame(const QJsonObject& frame);
    void committedSegmentReady(const QString& path);
    void previewBatchStarted();
    void previewBatchFinished();
    void captureState(const QJsonObject& status);
    void controlsChanged();
    void recordLabelSaved(const QString& path);
    void recordLabelSaveFailed(const QString& path,const QString& message);
protected:
    void showEvent(QShowEvent* event) override;
private:
    void response(const QString& command,const QJsonObject& data);
    void capabilities(const QJsonObject& data);
    void jobStatus(const QJsonObject& data);
    void streamStatus(const QJsonObject& data);
    void updateMode();
    void unavailable(const QString& diagnostic);
    void setStatus(const QString& text);
    void controls();
    void updateParameterState();
    void samplesFromDuration();
    void durationFromSamples();
    QString parameterError() const;
    void showError(const QString& visible,const QString& diagnostic={});
    void annotateCommittedRecord(const QString& path);
    QJsonObject plan() const;
    CaptureClient* client_;
    QTimer* poll_;
    QComboBox *rate_,*mode_;
    QGridLayout* fieldGrid_;
    QDoubleSpinBox *duration_,*ppr_;
    QLineEdit *samples_,*directory_;
    QLabel *state_,*source_,*parameterError_,*error_,*progressText_;
    QLabel *modeLabel_,*samplesLabel_,*durationLabel_,*pprLabel_;
    QProgressBar* progress_;
    QPushButton *start_,*cancel_,*view_,*browse_,*retry_;
    QJsonObject planned_,capabilities_,lastJobStatus_;
    QString jobId_,completedPath_,dataRoot_;
    RecordLabel nextRecordLabel_=RecordLabel::Unlabeled,activeRecordLabel_=RecordLabel::Unlabeled;
    QSet<QString> annotatedSegments_;
    bool connectedOnce_=false,available_=false,active_=false,cancelling_=false,analysisBusy_=false;
    bool streaming_=true;
};
}
