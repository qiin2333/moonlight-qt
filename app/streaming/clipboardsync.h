#pragma once

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QPair>
#include <QQueue>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>

class QImage;
class QMimeData;
class QNetworkAccessManager;
class QNetworkReply;
class QSslError;
class QTimer;

struct ClipboardSyncHostContext
{
    QString address;
    quint16 httpsPort = 0;
    QSslCertificate serverCertificate;
    QSslCertificate clientCertificate;
    QSslKey clientPrivateKey;
};

// ClipboardSync owns the local clipboard state inside the clipboard helper
// process. It implements the v1 wire format (u8 version=1, u8 kind, u32
// token, u32 length, bytes payload, little-endian) used by the Limelight
// control packet 0x5508, but it does not talk to Limelight directly. The
// helper process reports outbound frames to the main process via IPC, and the
// main process forwards them through LiSendClipboardData().
//
// Supports kind=1 (UTF-8 text), kind=2 (PNG image), and kind=3 (REF JSON
// pointing to an out-of-band blob transferred over HTTPS via the Sunshine
// /api/v1/clipboard/blob endpoints) in both directions. Payloads above the
// 65 KB single-packet cap are switched to KIND_REF transparently.
// File / other payloads are accepted on the wire but ignored.
//
// Compound clipboard (v2, no wire-format change): a clipboard change that
// carries BOTH text and an image is emitted as two consecutive frames
// sharing a non-zero token, text frame first, image frame second. Peers
// that don't aggregate simply apply the frames in order and end up with
// the image — the exact v1 outcome — so no version negotiation is needed.
// Aggregating peers apply each frame as it arrives (zero added latency vs
// v1) and retain the text payload: when the image lands — inline or after
// its REF blob fetch — the clipboard is upgraded with one compound write
// carrying both flavors. Single-flavor changes keep the legacy token=0
// single frame. token=0 is never aggregated (the Qt client historically
// emitted it for every frame). Inside a burst the text frame is always
// inline (a burst text payload larger than the wire cap is dropped rather
// than deferred via REF, which would race the image frame onto the wire
// and flip legacy peers' final state to text); the image frame may still
// use KIND_REF.
//
// Echo suppression: when we either write a payload to the local clipboard
// (because the host pushed it) or send a payload to the host (because we
// detected a local change), we record a (hash, timestamp_ms) pair into a
// 16-entry deque with a 5 second TTL. Subsequent QClipboard::dataChanged
// notifications for the same content are dropped, mirroring the Sunshine
// GUI agent's logic so the two ends don't ping-pong.
//
// Text keys the cache on the payload bytes. Images key a second cache on
// the decoded pixels: platforms re-encode clipboard image flavors after we
// write them (macOS synthesizes new PNGf/TIFF data lazily and bumps the
// pasteboard changeCount again), so the delayed echo hands back a
// re-encoded copy whose bytes never match the wire payload. Pixels survive
// those lossless re-encodes, making them the only stable identity.
//
// Threading:
//   * Construct on the GUI thread.
//   * start() / stop() must run on the GUI thread.
//   * handleIncomingFrame() may be called from any thread. It marshals the
//     payload onto the helper GUI thread via a queued metacall.
class ClipboardSync : public QObject
{
    Q_OBJECT

public:
    enum Kind : uint8_t {
        KIND_TEXT = 1,
        KIND_PNG  = 2,
        KIND_REF  = 3,
    };

    static constexpr uint8_t WIRE_VERSION = 1;
    static constexpr int     MAX_PAYLOAD  = 65535 - 10; // wire frame must fit u16 length
    // Payload size at/above which we switch from inline KIND_TEXT/KIND_PNG to
    // out-of-band blob transfer (KIND_REF). Leaves headroom under the wire cap.
    static constexpr int     INLINE_THRESHOLD = 60000;
    // Mirrors the cross-client cap (64 MiB) shared with the Android and
    // HarmonyOS clients. The service-side blob store accepts up to this much.
    static constexpr qint64  MAX_BLOB_BYTES   = 64LL * 1024 * 1024;
    static constexpr int     ECHO_TTL_MS  = 5000;
    static constexpr int     ECHO_MAX     = 16;
    // Mirror the Android client's cap (32 Mpx) so a stray full-screen capture
    // doesn't try to PNG-encode a 100 MB bitmap and stall the GUI thread.
    static constexpr qint64  MAX_IMAGE_PIXELS = 32LL * 1024 * 1024;

    static constexpr bool shouldTransferOutOfBand(qint64 payloadBytes)
    {
        return payloadBytes >= INLINE_THRESHOLD;
    }

    // How long a burst's payloads are retained after their frames applied —
    // purely a memory bound; application is immediate on arrival (see class
    // comment).
    static constexpr int BURST_RETENTION_MS = 5000;
    static constexpr int MAX_PENDING_BURSTS = 32;

    // File copy/paste is not part of the clipboard sync protocol. Native file
    // clipboards often also expose a thumbnail or application icon as an image,
    // so callers must reject file references before considering image formats.
    static bool hasFileReferences(const QMimeData* mime);

    explicit ClipboardSync(const ClipboardSyncHostContext& hostContext = ClipboardSyncHostContext(),
                           QObject* parent = nullptr);
    ~ClipboardSync() override;

    void setHostContext(const ClipboardSyncHostContext& hostContext);

    // Start watching the local clipboard and accepting inbound payloads.
    // Must be called on the GUI thread.
    void start();

    // Disconnect signals and stop processing both directions.
    void stop();

    // Called from the control receive thread when the host pushes a 0x5508
    // payload. Buffer is only valid for the duration of the call; we copy.
    void handleIncomingFrame(const char* data, int length);

signals:
    // Emitted when local clipboard changes need to be sent to the host. The
    // owner decides how to deliver the frame, which lets the clipboard engine
    // run in a helper process without direct access to the Limelight session.
    void outboundFrame(QByteArray frame);

private slots:
#ifdef Q_OS_MACOS
    // QClipboard::dataChanged wrapper that keeps the native pasteboard
    // changeCount baseline in sync before processing the clipboard.
    void onMacClipboardDataChanged();
#endif

    // QClipboard::dataChanged -> emitted on GUI thread.
    void onLocalClipboardChanged();

#ifdef Q_OS_MACOS
    // macOS does not reliably emit QClipboard::dataChanged for external
    // pasteboard changes while the helper is a background accessory process.
    void pollPasteboardChangeCount();
#endif

    // Queued slot used by handleIncomingFrame() to deliver to GUI thread.
    void onIncomingFrame(QByteArray frame);

private:
    // Aggregation state for one inbound compound burst (frames sharing a
    // non-zero token). Frames are applied as they arrive; the retained text
    // lets the arriving image upgrade the clipboard to one compound write.
    struct BurstState
    {
        QByteArray textPayload;
        QByteArray pngPayload;
        bool haveText = false;
        bool havePng = false;
        QTimer* timer = nullptr;
    };

    bool encodeFrame(uint8_t kind, quint32 token, const QByteArray& payload, QByteArray& outFrame) const;
    bool decodeFrame(const QByteArray& frame,
                     uint8_t& outKind,
                     quint32& outToken,
                     QByteArray& outPayload) const;

    // Hash + TTL bookkeeping; not thread-safe, only touched from GUI thread.
    bool seenRecently(uint64_t hash);
    void recordHash(uint64_t hash);

    static uint64_t hashBytes(const QByteArray& bytes);

    // Pixel-level echo bookkeeping for images (see class comment).
    bool seenImageRecently(uint64_t pixelHash);
    void recordImageHash(uint64_t pixelHash);
    static uint64_t hashImagePixels(const QImage& image);

    bool encodeImageAsPng(const QImage& image,
                          QByteArray& outPng,
                          const char* sourceDescription) const;
    void sendClipboardPng(const QByteArray& png,
                          const QString& sourceDescription,
                          quint32 token = 0);
    // Shared text outbound path for single-flavor changes (token 0) and the
    // text frame of a compound burst. Applies the size cap, echo checks and
    // inline-vs-REF choice, then emits.
    void sendTextOutbound(const QByteArray& utf8, quint32 token);
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
    bool buildBlobUrl(const QString& tail, QUrl& outUrl) const;
    QNetworkAccessManager* nam();
    void uploadAndSendRef(const QByteArray& payload, const QString& mime, quint32 token = 0);
    void fetchRefAndApply(const QString& id, const QString& mime, qint64 advertisedSize);
    void fetchBlob(const QString& id,
                   qint64 advertisedSize,
                   const std::function<void(const QByteArray&, const QString&)>& onFetched);
    void applyInboundText(const QByteArray& payload);
    void applyInboundPng(const QByteArray& payload);

    // Compound burst aggregation (see class comment).
    quint32 nextToken();
    void handleBurstFrame(uint8_t kind, quint32 token, const QByteArray& payload);
    void burstFlavorResolved(quint32 token, uint8_t kind, const QByteArray& payload);
    void removeBurst(quint32 token);
    void applyCompound(const QByteArray& text, const QByteArray& png);

    ClipboardSyncHostContext m_HostContext;
    QNetworkAccessManager* m_Nam = nullptr;

    bool m_Active = false;
    QQueue<QPair<uint64_t, qint64>> m_EchoCache;       // (byte hash, timestamp_ms)
    QQueue<QPair<uint64_t, qint64>> m_ImageEchoCache;  // (pixel hash, timestamp_ms)
    QHash<quint32, BurstState> m_PendingBursts;
    quint32 m_NextToken = 1;

#ifdef Q_OS_MACOS
    QTimer* m_PasteboardPollTimer = nullptr;
    int m_LastPasteboardChangeCount = -1;
#endif

    // Counter for self-writes pending QClipboard::dataChanged callbacks.
    // Some Windows clipboard hooks (e.g. IME composition managers) cause a
    // single setMimeData() to fire dataChanged twice; a bool latch would
    // absorb the first echo and leak the second back to the host. Counting
    // lets multiple pending writes coexist without ping-pong.
    int m_PendingSelfWrites = 0;
};
