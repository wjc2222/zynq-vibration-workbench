#pragma once
#include <QJsonObject>
#include <memory>

// One bounded writer owns stdout. Acquisition producers only enqueue and never
// wait for the GUI pipe. Preview is a replaceable latest-only slot.
class CaptureOutput final {
public:
    CaptureOutput();
    ~CaptureOutput();
    CaptureOutput(const CaptureOutput&)=delete;
    CaptureOutput& operator=(const CaptureOutput&)=delete;
    bool enqueue(const QJsonObject& object);
    void discardPreview();
    QJsonObject transportProfile() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
