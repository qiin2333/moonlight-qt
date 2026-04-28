#pragma once

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QPair>
#include <QQueue>
#include <cstdint>

// ClipboardSync owns the bidirectional clipboard sync state for an active
// streaming session against an AlkaidLab Sunshine fork host. It implements
// the v1 wire format (u8 version=1, u8 kind, u32 token, u32 length, bytes
// payload, little-endian) over Limelight control packet 0x5508
// (LiSendClipboardData / ConnListenerClipboardData).
//
// Currently only kind=1 (UTF-8 text) is implemented in both directions.
// Image / file payloads are accepted on the wire but ignored.
//
// Echo suppression: when we either write a payload to the local clipboard
// (because the host pushed it) or send a payload to the host (because we
// detected a local change), we record a (hash, timestamp_ms) pair into a
// 16-entry deque with a 5 second TTL. Subsequent QClipboard::dataChanged
// notifications for the same content are dropped, mirroring the Sunshine
// GUI agent's logic so the two ends don't ping-pong.
//
// Threading:
//   * Construct on the GUI thread.
//   * start() / stop() must run on the GUI thread.
//   * handleIncomingFrame() may be called from any thread (specifically the
//     moonlight-common-c control receive thread). It marshals the payload
//     onto the GUI thread via a queued metacall.
class ClipboardSync : public QObject
{
    Q_OBJECT

public:
    enum Kind : uint8_t {
        KIND_TEXT = 1,
        KIND_PNG  = 2,
    };

    static constexpr uint8_t WIRE_VERSION = 1;
    static constexpr int     MAX_PAYLOAD  = 65535 - 10; // wire frame must fit u16 length
    static constexpr int     ECHO_TTL_MS  = 5000;
    static constexpr int     ECHO_MAX     = 16;

    explicit ClipboardSync(QObject* parent = nullptr);
    ~ClipboardSync() override;

    // Start watching the local clipboard and accepting inbound payloads.
    // Must be called on the GUI thread.
    void start();

    // Disconnect signals and stop processing both directions.
    void stop();

    // Called from the control receive thread when the host pushes a 0x5508
    // payload. Buffer is only valid for the duration of the call; we copy.
    void handleIncomingFrame(const char* data, int length);

private slots:
    // QClipboard::dataChanged -> emitted on GUI thread.
    void onLocalClipboardChanged();

    // Queued slot used by handleIncomingFrame() to deliver to GUI thread.
    void onIncomingFrame(QByteArray frame);

private:
    bool encodeTextFrame(const QByteArray& utf8, QByteArray& outFrame) const;
    bool decodeFrame(const QByteArray& frame,
                     uint8_t& outKind,
                     QByteArray& outPayload) const;

    // Hash + TTL bookkeeping; not thread-safe, only touched from GUI thread.
    bool seenRecently(uint64_t hash);
    void recordHash(uint64_t hash);

    static uint64_t hashBytes(const QByteArray& bytes);

    bool m_Active = false;
    QQueue<QPair<uint64_t, qint64>> m_EchoCache; // (hash, timestamp_ms)

    // Guard against re-entering onLocalClipboardChanged from our own SDL write.
    bool m_SuppressNextChange = false;
};
