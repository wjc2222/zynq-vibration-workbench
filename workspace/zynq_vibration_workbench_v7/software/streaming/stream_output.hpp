#pragma once
#include <QJsonObject>
#include <memory>

// Preview occupies one replaceable slot. Control events are FIFO, bounded,
// and never replaced. A full control queue fails the transport after a bounded
// wait; it must never block raw tail publication or service shutdown.
class StreamOutput {
public:
    StreamOutput();
    ~StreamOutput();
    bool send(const QJsonObject& message);
    void clearPreview();
    bool failed() const;
    QString failureCode() const;
    QJsonObject diagnostics() const;
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
