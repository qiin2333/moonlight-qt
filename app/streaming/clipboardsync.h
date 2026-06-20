#pragma once

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QPair>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <cstdint>

class NvComputer;
class QImage;
class QMimeData;
class QNetworkAccessManager;
class QNetworkReply;
class QSslError;

// ClipboardSync owns the bidirectional clipboard sync state for an active
// streaming session against an AlkaidLab Sunshine fork host. It implements
// the v1 wire format (u8 version=1, u8 kind, u32 token, u32 length, bytes
// payload, little-endian) over Limelight control packet 0x5508
// (LiSendClipboardData / ConnListenerClipboardData).
//
// Supports kind=1 (UTF-8 text), kind=2 (PNG image), kind=3 (REF JSON
// pointing to an out-of-band blob transferred over HTTPS via the Sunshine
// /api/v1/clipboard/blob endpoints) in both directions, and kind=4
// (FILE_OFFER JSON) host-to-client file downloads. Payloads above the 65 KB
// single-packet cap are switched to KIND_REF transparently. Unknown payloads
// are accepted on the wire but ignored.
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
        KIND_REF  = 3,
        KIND_FILE_OFFER = 4,
    };

    static constexpr uint8_t WIRE_VERSION = 1;
    static constexpr int     MAX_PAYLOAD  = 65535 - 10; // wire frame must fit u16 length
    // Payload size at/above which we switch from inline KIND_TEXT/KIND_PNG to
    // out-of-band blob transfer (KIND_REF). Leaves headroom under the wire cap.
    static constexpr int     INLINE_THRESHOLD = 60000;
    // Mirrors the cross-client cap (64 MiB) shared with the Android and
    // HarmonyOS clients. The service-side blob store accepts up to this much.
    static constexpr qint64  MAX_BLOB_BYTES   = 64LL * 1024 * 1024;
    static constexpr qint64  MAX_FILE_TRANSFER_BYTES = 4LL * 1024 * 1024 * 1024;
    static constexpr int     ECHO_TTL_MS  = 5000;
    static constexpr int     ECHO_MAX     = 16;
    // Mirror the Android client's cap (32 Mpx) so a stray full-screen capture
    // doesn't try to PNG-encode a 100 MB bitmap and stall the GUI thread.
    static constexpr qint64  MAX_IMAGE_PIXELS = 32LL * 1024 * 1024;

    explicit ClipboardSync(NvComputer* computer = nullptr, QObject* parent = nullptr);
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
    bool encodeFrame(uint8_t kind, const QByteArray& payload, QByteArray& outFrame) const;
    bool decodeFrame(const QByteArray& frame,
                     uint8_t& outKind,
                     QByteArray& outPayload) const;

    // Hash + TTL bookkeeping; not thread-safe, only touched from GUI thread.
    bool seenRecently(uint64_t hash);
    void recordHash(uint64_t hash);

    static uint64_t hashBytes(const QByteArray& bytes);

    bool encodeImageAsPng(const QImage& image,
                          QByteArray& outPng,
                          const char* sourceDescription) const;
    void sendClipboardPng(const QByteArray& png,
                          const QString& sourceDescription);
    bool extractClipboardPng(const QMimeData* mime,
                             QByteArray& outPng,
                             QString* outSourceDescription = nullptr) const;
    bool tryExtractImageBytes(const QMimeData* mime,
                              const QStringList& preferredFormats,
                              QByteArray& outPng,
                              QString* outSourceDescription) const;
    bool tryExtractImageFromUrls(const QMimeData* mime,
                                 QByteArray& outPng,
                                 QString* outSourceDescription) const;
    bool tryExtractImageFromHtml(const QMimeData* mime,
                                 QByteArray& outPng,
                                 QString* outSourceDescription) const;

    // Out-of-band blob helpers. Both run on the GUI thread; the
    // QNetworkAccessManager event-loop callbacks land back on the GUI thread.
    bool buildApiUrl(const QString& path, QUrl& outUrl) const;
    bool buildBlobUrl(const QString& tail, QUrl& outUrl) const;
    QNetworkAccessManager* nam();
    void uploadAndSendRef(const QByteArray& payload, const QString& mime);
    void fetchRefAndApply(const QString& id, const QString& mime, qint64 advertisedSize);
    void fetchFileOffer(const QByteArray& payload);
    void downloadFileOffer(const QString& id,
                           const QString& name,
                           qint64 advertisedSize,
                           const QString& downloadPath);
    void applyInboundText(const QByteArray& payload);
    void applyInboundPng(const QByteArray& payload);

    static QString sanitizeFileName(const QString& name);
    static QString uniqueDownloadPath(const QString& fileName);

    NvComputer* m_Computer = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;

    bool m_Active = false;
    QQueue<QPair<uint64_t, qint64>> m_EchoCache; // (hash, timestamp_ms)

    // Counter for self-writes pending QClipboard::dataChanged callbacks.
    // Some Windows clipboard hooks (e.g. IME composition managers) cause a
    // single setMimeData() to fire dataChanged twice; a bool latch would
    // absorb the first echo and leak the second back to the host. Counting
    // lets multiple pending writes coexist without ping-pong.
    int m_PendingSelfWrites = 0;
};
