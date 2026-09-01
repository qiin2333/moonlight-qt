#pragma once

#include <QJsonArray>
#include <QJsonObject>

#include <memory>
#include <functional>

namespace RemoteUsbAgent {

struct BackendCallbacks {
    std::function<void(QByteArray deviceId, quint64 generation)> opened;
    std::function<void(quint64 generation)> stopped;
    std::function<void(quint64 generation, QString message)> failed;
};

class Backend
{
public:
    virtual ~Backend() = default;
    virtual QJsonArray enumerate(QString *error = nullptr) = 0;
    virtual void setCallbacks(BackendCallbacks callbacks) = 0;
    virtual bool start(const QJsonObject &request,
                       QString *error = nullptr) = 0;
    virtual void stop() noexcept = 0;
};

class LibusbBackend final : public Backend
{
public:
    LibusbBackend();
    ~LibusbBackend() override;

    QJsonArray enumerate(QString *error = nullptr) override;
    void setCallbacks(BackendCallbacks callbacks) override;
    bool start(const QJsonObject &request,
               QString *error = nullptr) override;
    void stop() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace RemoteUsbAgent
