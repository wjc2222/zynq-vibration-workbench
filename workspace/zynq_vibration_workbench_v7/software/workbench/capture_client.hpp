#pragma once
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <memory>
class QThread;
namespace vibration::workbench {
class CaptureTransport;
struct CaptureMailbox;
// Main-thread facade. AX7_CAPTURE_WORKER=1 selects the bounded transport actor.
class CaptureClient final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QJsonObject transportProfile READ transportProfile)
    Q_PROPERTY(QJsonObject serviceProcessInfo READ serviceProcessInfo)
public:
    explicit CaptureClient(QObject* parent=nullptr);
    ~CaptureClient() override;
    void setCommand(const QString& program,const QStringList& arguments={});
    // Explicit protocol/backend selection; existing callers remain legacy.
    void setStreaming(bool value);
    void connectService();
    void stop();
    bool isRunning() const;
    bool hasPending(const QString& command) const;
    void request(const QString& command,QJsonObject fields={});
    QJsonObject transportProfile() const;
    QJsonObject serviceProcessInfo() const;
signals:
    void responseReceived(const QString& command,const QJsonObject& data);
    void requestRejected(const QString& command,const QString& code,const QString& message);
    void jobUpdated(const QJsonObject& data);
    void liveFrame(const QJsonObject& frame);
    // Bracket one bounded stream-mailbox snapshot. Consumers ingest every
    // frame in order and defer presentation until the matching batch end.
    void previewBatchStarted();
    void previewBatchFinished();
    void previewRejected(const QString& reason,bool quarantined);
    void connectionFailed(const QString& detail);
    void transportProfileUpdated(const QJsonObject& profile);
private:
    void drain(quint64 session);
    bool stopWorker();
    CaptureTransport* transport_=nullptr;
    QThread* worker_=nullptr;
    std::shared_ptr<CaptureMailbox> mailbox_;
    QString program_;
    QStringList arguments_;
    QHash<QString,int> submitted_;
    quint64 session_=1,lastDeliveredEnd_=0;
    bool workerEnabled_=false,profileEnabled_=false,streaming_=false;
};
}
