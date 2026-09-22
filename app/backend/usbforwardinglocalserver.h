#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QObject>
#include <functional>
#include <memory>

class QProcess;
class QTemporaryDir;

/*
 * Session-scoped local USB/IP server process (Linux/macOS).
 *
 * macOS has no system USB/IP daemon to attach to like usbipd-win on Windows.
 * Instead, Session spawns the bundled moonlight-usbd helper for the devices
 * being forwarded, waits for its READY line and points the reverse tunnel at
 * the helper's ephemeral loopback port. The helper exits by itself when its
 * stdin closes, so teardown only has to drop this object.
 *
 * macOS uses blocking startup. Linux elevates moonlight-usb-host through
 * polkit and starts asynchronously, including the authorization prompt. Its
 * supervisor restores the local driver on stdin EOF or exporter failure.
 * Both implementations need a running event loop to drain helper stderr.
 */
class UsbForwardingLocalServer : public QObject
{
public:
    static bool spawnSupported();
    static QString locateHelper();
    static bool hasPendingNativeCleanup();

    UsbForwardingLocalServer() = default;
    ~UsbForwardingLocalServer();

    UsbForwardingLocalServer(const UsbForwardingLocalServer&) = delete;
    UsbForwardingLocalServer& operator=(const UsbForwardingLocalServer&) = delete;

    // Spawns `moonlight-usbd serve --listen 127.0.0.1:0 --bind <id>...` and
    // waits up to 10 s for the helper's first stdout line. "READY <port>"
    // fills *actualPort; "ERROR {json...}" (or a spawn/timeout failure) fills
    // *error with the raw detail for the caller to map to a user message.
    bool start(const QStringList& busIds, quint16* actualPort, QString* error);

    // Linux authorization and startup run asynchronously. Closing stdin also
    // cancels a pending authorization: a late approval must not take a device.
    using StartCallback = std::function<void(quint16, const QString&)>;
    void startNative(const QString& busId, const QString& identity,
                     StartCallback ready, std::function<void(const QString&)> exited);

    // Graceful stop: closing the helper's stdin makes it exit by itself.
    void stop();

    bool isRunning() const;

private:
    void startNativeProcess(const QString& program, const QStringList& arguments,
                            std::shared_ptr<QTemporaryDir> staging, StartCallback ready,
                            std::function<void(const QString&)> exited);
    QProcess* m_Process = nullptr;
    bool m_Native = false;

    // Bounded tail of the helper's stderr after READY, kept for diagnostics.
    QByteArray m_StderrTail;
};
