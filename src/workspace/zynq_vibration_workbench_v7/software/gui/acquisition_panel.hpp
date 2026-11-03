#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QHash>
#include <QStringList>
#include <functional>
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QLineEdit;
class QProgressBar;
class QProcess;
class QTimer;
namespace vibration::gui {
// Device plans deliberately do not share the file-analysis configuration.
class AcquisitionPanel final : public QWidget {
    Q_OBJECT
public:
    explicit AcquisitionPanel(std::function<QDoubleSpinBox*()> numberFactory, QWidget* parent=nullptr);
    ~AcquisitionPanel() override;
    // Explicit dependency injection for native QA; normal GUI has no fixture option.
    void setServiceCommand(const QString& program, const QStringList& arguments={});
    void setAnalysisBusy(bool busy);
signals:
    void captureReady(const QString& path);
protected:
    void showEvent(QShowEvent* event) override;
private:
    void connectService();
    void stopService();
    void request(const QString& command, QJsonObject fields={});
    void receive();
    void message(const QJsonObject& object);
    void capabilities(const QJsonObject& data);
    void jobStatus(const QJsonObject& data);
    void startCapture();
    void controls();
    void estimate();
    void fail(const QString& text);
    void detail(const QString& text);
    QJsonObject plan() const;
    QComboBox* rate_;
    QDoubleSpinBox *duration_, *ppr_;
    QLineEdit* directory_;
    QLabel *state_, *source_, *estimate_, *progressText_, *details_, *device_;
    QPushButton *start_, *cancel_, *retry_, *browse_, *view_;
    QProgressBar* progress_;
    QProcess* process_;
    QTimer* timer_;
    struct Pending {QString command; qint64 deadline;};
    QHash<QString,Pending> pending_;
    QByteArray buffer_;
    QString program_, jobId_, completedPath_;
    QStringList arguments_;
    QJsonObject planned_;
    quint64 sequence_=0;
    bool available_=false, active_=false, cancelling_=false, analysisBusy_=false, stopping_=false;
};
}
