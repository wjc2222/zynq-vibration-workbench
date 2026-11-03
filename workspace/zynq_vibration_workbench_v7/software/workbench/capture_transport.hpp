#pragma once
#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include "../acquisition/capture_profile.hpp"
class QProcess;
class QTimer;

namespace vibration::workbench {
// Private protocol engine. All methods/children belong to its QObject thread.
// Shared by the default same-thread path and opt-in bounded worker path.
class CaptureTransport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QJsonObject transportProfile READ transportProfile)
public:
    explicit CaptureTransport(QObject* parent=nullptr);
    ~CaptureTransport() override;
    void initialize();
    void setCooperative(bool value){cooperative_=value;}
    quint64 sessionToken()const{return session_;}
    void setSessionToken(quint64 value){session_=value;}
    QJsonObject processInfo() const;
    void abortTransport(const QString& detail){fail(detail);}
    void setCommand(const QString& program,const QStringList& arguments={});
    void setStreaming(bool value);
    void connectService();
    void stop();
    bool isRunning() const;
    bool hasPending(const QString& command) const;
    void request(const QString& command,QJsonObject fields={});
    QJsonObject transportProfile() const;
signals:
    void mailboxReady(quint64 session);
    void stateSnapshotChanged(const QJsonObject& state);
    void responseReceived(const QString& command,const QJsonObject& data);
    void requestRejected(const QString& command,const QString& code,const QString& message);
    void jobUpdated(const QJsonObject& data);
    void liveFrame(const QJsonObject& frame);
    void previewRejected(const QString& reason,bool quarantined);
    void connectionFailed(const QString& detail);
    void transportProfileUpdated(const QJsonObject& profile);
private:
    void receive();
    void frame(const QJsonObject& object);
    void previewFrame(QJsonObject object);
    void streamFrame(const QJsonObject& object);
    void streamPreview(QJsonObject object);
    void fail(const QString& detail);
    void receiveDiagnostics();
    struct Pending {QString command;qint64 deadline;};
    quint64 session_=1;
    bool cooperative_=false,receivePending_=false;
    bool startedEver_=false,exitSeen_=false;
    QProcess* process_=nullptr;
    QTimer* deadlineTimer_=nullptr;
    QString program_;
    QStringList arguments_;
    QByteArray buffer_;
    QHash<QString,Pending> pending_;
    quint64 sequence_=0;
    QString liveJob_,liveGeneration_;
    QByteArray liveIdentity_;
    QJsonObject requestedPlan_,livePlan_,backendCapabilities_;
    quint64 lastFrameSequence_=0,tickAnchor_=0;
    quint64 lastRecordEnd_=0;
    bool haveTickAnchor_=false,previewQuarantined_=false,previewTerminal_=true;
    bool stopping_=false;
    bool streaming_=false,streamSessionDone_=true;
    vibration::capture_diag::Profile profile_;
    QByteArray diagnosticBuffer_;
    QJsonObject serviceProfile_;
    bool diagnosticDiscard_=false;
};
}
