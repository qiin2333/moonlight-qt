#include "clipboardsync.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QMetaObject>
#include <QtEndian>

#include <cstring>

#include <SDL.h>

#include <Limelight.h>

ClipboardSync::ClipboardSync(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<QByteArray>("QByteArray");
}

ClipboardSync::~ClipboardSync()
{
    stop();
}

void ClipboardSync::start()
{
    if (m_Active) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ClipboardSync: no QClipboard available; sync disabled");
        return;
    }

    connect(cb, &QClipboard::dataChanged,
            this, &ClipboardSync::onLocalClipboardChanged,
            Qt::UniqueConnection);

    m_Active = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "ClipboardSync: started (text-only, bidirectional)");
}

void ClipboardSync::stop()
{
    if (!m_Active) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb != nullptr) {
        disconnect(cb, &QClipboard::dataChanged,
                   this, &ClipboardSync::onLocalClipboardChanged);
    }

    m_EchoCache.clear();
    m_SuppressNextChange = false;
    m_Active = false;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ClipboardSync: stopped");
}

void ClipboardSync::handleIncomingFrame(const char* data, int length)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    // Copy out of the recv-thread buffer and marshal to the GUI thread.
    QByteArray frame(data, length);
    QMetaObject::invokeMethod(this, "onIncomingFrame", Qt::QueuedConnection,
                              Q_ARG(QByteArray, frame));
}

void ClipboardSync::onIncomingFrame(QByteArray frame)
{
    if (!m_Active) {
        return;
    }

    uint8_t kind = 0;
    QByteArray payload;
    if (!decodeFrame(frame, kind, payload)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ClipboardSync: discarding malformed inbound frame (%d bytes)",
                    static_cast<int>(frame.size()));
        return;
    }

    if (kind != KIND_TEXT) {
        // Image / file kinds are not yet wired up on the Qt client.
        return;
    }

    // Validate UTF-8 by round-tripping through QString. Reject anything that
    // contains an embedded NUL since SDL_SetClipboardText is C-string-based.
    if (payload.contains('\0')) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ClipboardSync: dropping inbound text payload with embedded NUL");
        return;
    }

    // Record hash *before* writing so the dataChanged echo we're about to
    // trigger is suppressed.
    uint64_t hash = hashBytes(payload);
    recordHash(hash);
    m_SuppressNextChange = true;

    // SDL_SetClipboardText copies internally; payload is already null-safe
    // because QByteArray's data() guarantees a trailing NUL.
    if (SDL_SetClipboardText(payload.constData()) != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ClipboardSync: SDL_SetClipboardText failed: %s",
                    SDL_GetError());
        m_SuppressNextChange = false;
    }
}

void ClipboardSync::onLocalClipboardChanged()
{
    if (!m_Active) {
        return;
    }

    if (m_SuppressNextChange) {
        m_SuppressNextChange = false;
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    QString text = cb->text();
    if (text.isEmpty()) {
        return;
    }

    QByteArray utf8 = text.toUtf8();
    if (utf8.isEmpty() || utf8.size() > MAX_PAYLOAD) {
        return;
    }

    uint64_t hash = hashBytes(utf8);
    if (seenRecently(hash)) {
        // This change was the echo of a payload the host just pushed to us.
        return;
    }
    recordHash(hash);

    QByteArray frame;
    if (!encodeTextFrame(utf8, frame)) {
        return;
    }

    int rc = LiSendClipboardData(frame.constData(), frame.size());
    if (rc != 0) {
        // Negative return = no active session, host doesn't support clipboard,
        // or send failed; log once at debug level to avoid spam.
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "ClipboardSync: LiSendClipboardData(%d bytes) -> %d",
                     static_cast<int>(frame.size()), rc);
    }
}

bool ClipboardSync::encodeTextFrame(const QByteArray& utf8, QByteArray& outFrame) const
{
    if (utf8.size() > MAX_PAYLOAD) {
        return false;
    }

    outFrame.clear();
    outFrame.reserve(10 + utf8.size());

    // u8 version
    outFrame.append(static_cast<char>(WIRE_VERSION));
    // u8 kind
    outFrame.append(static_cast<char>(KIND_TEXT));
    // u32 token (LE) - we don't generate tokens client-side; 0 is accepted.
    quint32 tokenLE = qToLittleEndian<quint32>(0);
    outFrame.append(reinterpret_cast<const char*>(&tokenLE), sizeof(tokenLE));
    // u32 length (LE)
    quint32 lenLE = qToLittleEndian<quint32>(static_cast<quint32>(utf8.size()));
    outFrame.append(reinterpret_cast<const char*>(&lenLE), sizeof(lenLE));
    // bytes payload
    outFrame.append(utf8);
    return true;
}

bool ClipboardSync::decodeFrame(const QByteArray& frame,
                                uint8_t& outKind,
                                QByteArray& outPayload) const
{
    // 1 + 1 + 4 + 4 = 10 byte header minimum.
    if (frame.size() < 10) {
        return false;
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(frame.constData());
    uint8_t version = p[0];
    if (version != WIRE_VERSION) {
        return false;
    }

    outKind = p[1];

    quint32 lenLE = 0;
    memcpy(&lenLE, p + 6, sizeof(lenLE));
    quint32 length = qFromLittleEndian<quint32>(lenLE);

    if (length > static_cast<quint32>(frame.size() - 10)) {
        return false;
    }

    outPayload = QByteArray(reinterpret_cast<const char*>(p + 10),
                            static_cast<int>(length));
    return true;
}

bool ClipboardSync::seenRecently(uint64_t hash)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Drop stale entries off the front first.
    while (!m_EchoCache.isEmpty() && (now - m_EchoCache.front().second) > ECHO_TTL_MS) {
        m_EchoCache.removeFirst();
    }

    for (const auto& e : m_EchoCache) {
        if (e.first == hash) {
            return true;
        }
    }
    return false;
}

void ClipboardSync::recordHash(uint64_t hash)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_EchoCache.enqueue(qMakePair(hash, now));
    while (m_EchoCache.size() > ECHO_MAX) {
        m_EchoCache.removeFirst();
    }
}

uint64_t ClipboardSync::hashBytes(const QByteArray& bytes)
{
    // FNV-1a 64-bit. Cheap and good enough for echo suppression.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < bytes.size(); ++i) {
        h ^= static_cast<uint8_t>(bytes[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}
