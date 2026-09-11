#pragma once

#include <QString>
#include <QStringList>

class QProcess;

/*
 * Session-scoped local USB/IP server process (macOS).
 *
 * macOS has no system USB/IP daemon to attach to like usbipd-win on Windows.
 * Instead, Session spawns the bundled moonlight-usbd helper for the devices
 * being forwarded, waits for its READY line and points the reverse tunnel at
 * the helper's ephemeral loopback port. The helper exits by itself when its
 * stdin closes, so teardown only has to drop this object.
 *
 * Like ClipboardHelperClient, this must be used from the Session thread only
 * (Session::exec runs the SDL loop without a Qt event loop); all waits are
 * blocking instead of signal-based.
 */
class UsbForwardingLocalServer
{
public:
    static bool spawnSupported();
    static QString locateHelper();

    UsbForwardingLocalServer() = default;
    ~UsbForwardingLocalServer();

    UsbForwardingLocalServer(const UsbForwardingLocalServer&) = delete;
    UsbForwardingLocalServer& operator=(const UsbForwardingLocalServer&) = delete;

    // Spawns `moonlight-usbd serve --listen 127.0.0.1:0 --bind <id>...` and
    // waits up to 10 s for the helper's first stdout line. "READY <port>"
    // fills *actualPort; "ERROR {json...}" (or a spawn/timeout failure) fills
    // *error with the raw detail for the caller to map to a user message.
    bool start(const QStringList& busIds, quint16* actualPort, QString* error);

    // Graceful stop: closing the helper's stdin makes it exit by itself.
    void stop();

    bool isRunning() const;

private:
    QProcess* m_Process = nullptr;
};
