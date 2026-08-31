#pragma once

#include <QObject>

#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTimer>

class QLocalSocket;

namespace RemoteUsb {

struct RemoteUsbAgentHostConfig {
    QString host;
    quint16 httpsPort = 0;
    QSslCertificate serverCertificate;
    QString clientIdentity;
    QString clientName;
    QSslConfiguration sslConfiguration;

    bool valid() const noexcept;
};

/*
 * Qt-side client for the standalone moonlight-usb-agent.  The client owns the
 * process and local socket, but never exposes the bearer token or a native USB
 * object to callers.  All commands carry a monotonically increasing
 * generation so late events from a previous lease can be ignored by policy.
 */
class RemoteUsbAgentClient final : public QObject
{
    Q_OBJECT

public:
    explicit RemoteUsbAgentClient(QObject *parent = nullptr);
    ~RemoteUsbAgentClient() override;

    Q_DISABLE_COPY(RemoteUsbAgentClient)

    bool launch(const QString &program,
                const QString &socketName,
                const QByteArray &token,
                QString *error = nullptr);
    void connectToAgent(const QString &socketName,
                        const QByteArray &token);
    bool configureHost(RemoteUsbAgentHostConfig config,
                       QString *error = nullptr);
    void enumerate();
    void start(const QByteArray &deviceId);
    void stop();
    void shutdown() noexcept;

    bool isReady() const noexcept { return m_ready; }
    quint64 generation() const noexcept { return m_generation; }

signals:
    void ready(quint32 version);
    void devicesChanged(QJsonArray devices);
    void opened(QByteArray deviceId);
    void stopped();
    void failed(QString message);

private slots:
    void retryConnect();
    void readSocket();
    void processFinished(int exitCode, QProcess::ExitStatus status);

private:
    void sendHello();
    void sendRequest(const QString &operation, const QJsonObject &extra = {});
    void handleMessage(const QJsonObject &message);
    void fail(const QString &message);
    void closeSocket() noexcept;

    QProcess *m_process = nullptr;
    QLocalSocket *m_socket = nullptr;
    QTimer m_connectTimer;
    QString m_socketName;
    QByteArray m_token;
    QByteArray m_readBuffer;
    quint64 m_generation = 0;
    int m_connectAttempts = 0;
    bool m_ready = false;
    bool m_shuttingDown = false;
    RemoteUsbAgentHostConfig m_hostConfig;
    quint64 m_streamGeneration = 0;
};

} // namespace RemoteUsb
