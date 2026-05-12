#include "clipboardsync.h"
#include "backend/nvcomputer.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslError>
#include <QUrl>
#include <QtEndian>

#include <cstring>

#include <SDL.h>

#include <Limelight.h>

ClipboardSync::ClipboardSync(NvComputer* computer, QObject* parent)
    : QObject(parent),
      m_Computer(computer)
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
                "ClipboardSync: started (text + PNG, bidirectional)");
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

    if (kind == KIND_TEXT) {
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
        return;
    }

    if (kind == KIND_PNG) {
        applyInboundPng(payload);
        return;
    }

    if (kind == KIND_REF) {
        // Payload is a small UTF-8 JSON descriptor: {"id":"...","mime":"...","size":N}.
        QJsonParseError jerr{};
        QJsonDocument doc = QJsonDocument::fromJson(payload, &jerr);
        if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: dropping inbound REF with bad JSON (%s)",
                        jerr.errorString().toUtf8().constData());
            return;
        }
        QJsonObject obj = doc.object();
        QString id = obj.value(QStringLiteral("id")).toString();
        QString mime = obj.value(QStringLiteral("mime")).toString();
        qint64 size = static_cast<qint64>(obj.value(QStringLiteral("size")).toDouble(0));
        if (id.isEmpty() || id.size() > 128) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: dropping inbound REF with bad id length");
            return;
        }
        if (size > MAX_BLOB_BYTES) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: dropping inbound REF, declared size %lld exceeds %lld cap",
                        static_cast<long long>(size), static_cast<long long>(MAX_BLOB_BYTES));
            return;
        }
        fetchRefAndApply(id, mime, size);
        return;
    }

    // Unknown kind — ignore.
}

void ClipboardSync::applyInboundPng(const QByteArray& payload)
{
    QImage image;
    if (!image.loadFromData(payload, "PNG") || image.isNull()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ClipboardSync: dropping inbound PNG payload (decode failed, %d bytes)",
                    static_cast<int>(payload.size()));
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    // Hash the wire bytes (not the decoded pixels) so the echo we suppress
    // matches what we'd re-encode if QClipboard hands the image straight back.
    uint64_t hash = hashBytes(payload);
    recordHash(hash);
    m_SuppressNextChange = true;
    cb->setImage(image);
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

    const QMimeData* mime = cb->mimeData();

    // Image takes precedence — some applications attach a fallback text label
    // (file path, alt text) alongside the bitmap; we want the picture, not the
    // path. Matches moonlight-android's ClipboardSyncManager ordering.
    if (mime != nullptr && mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
            if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                            "ClipboardSync: image too large (%dx%d), dropping",
                            image.width(), image.height());
                return;
            }

            QByteArray png;
            png.reserve(64 * 1024);
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            if (!image.save(&buf, "PNG") || png.isEmpty()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "ClipboardSync: PNG encode failed (%dx%d)",
                            image.width(), image.height());
                return;
            }

            if (png.size() > MAX_PAYLOAD) {
                // Out-of-band path. Record the underlying PNG hash before
                // upload so the echo we'll see when the host loops the REF
                // back (and we fetch the same bytes) is suppressed.
                if (png.size() > MAX_BLOB_BYTES) {
                    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                                "ClipboardSync: PNG payload %lld B exceeds %lld B blob cap, dropping",
                                static_cast<long long>(png.size()),
                                static_cast<long long>(MAX_BLOB_BYTES));
                    return;
                }
                uint64_t hash = hashBytes(png);
                if (seenRecently(hash)) {
                    return;
                }
                recordHash(hash);
                uploadAndSendRef(png, QStringLiteral("image/png"));
                return;
            }

            uint64_t hash = hashBytes(png);
            if (seenRecently(hash)) {
                return;
            }
            recordHash(hash);

            QByteArray frame;
            if (encodeFrame(KIND_PNG, png, frame)) {
                int rc = LiSendClipboardData(frame.constData(), frame.size());
                if (rc != 0) {
                    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                                 "ClipboardSync: LiSendClipboardData(PNG, %d bytes) -> %d",
                                 static_cast<int>(frame.size()), rc);
                }
            }
            return;
        }
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
    if (!encodeFrame(KIND_TEXT, utf8, frame)) {
        return;
    }

    int rc = LiSendClipboardData(frame.constData(), frame.size());
    if (rc != 0) {
        // Negative return = no active session, host doesn't support clipboard,
        // or send failed; log once at debug level to avoid spam.
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                     "ClipboardSync: LiSendClipboardData(text, %d bytes) -> %d",
                     static_cast<int>(frame.size()), rc);
    }
}

bool ClipboardSync::encodeFrame(uint8_t kind, const QByteArray& payload, QByteArray& outFrame) const
{
    if (payload.size() > MAX_PAYLOAD) {
        return false;
    }

    outFrame.clear();
    outFrame.reserve(10 + payload.size());

    // u8 version
    outFrame.append(static_cast<char>(WIRE_VERSION));
    // u8 kind
    outFrame.append(static_cast<char>(kind));
    // u32 token (LE) - we don't generate tokens client-side; 0 is accepted.
    quint32 tokenLE = qToLittleEndian<quint32>(0);
    outFrame.append(reinterpret_cast<const char*>(&tokenLE), sizeof(tokenLE));
    // u32 length (LE)
    quint32 lenLE = qToLittleEndian<quint32>(static_cast<quint32>(payload.size()));
    outFrame.append(reinterpret_cast<const char*>(&lenLE), sizeof(lenLE));
    // bytes payload
    outFrame.append(payload);
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

QNetworkAccessManager* ClipboardSync::nam()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
        // Pin the host's self-signed cert exactly like NvHTTP does: ignore
        // SSL errors only when the offending cert matches the pinned one.
        connect(m_Nam, &QNetworkAccessManager::sslErrors, this,
                [this](QNetworkReply* reply, const QList<QSslError>& errors) {
                    if (m_Computer == nullptr || m_Computer->serverCert.isNull()) {
                        return;
                    }
                    for (const QSslError& e : errors) {
                        if (m_Computer->serverCert != e.certificate()) {
                            return;
                        }
                    }
                    reply->ignoreSslErrors(errors);
                });
    }
    return m_Nam;
}

bool ClipboardSync::buildBlobUrl(const QString& tail, QUrl& outUrl) const
{
    if (m_Computer == nullptr || m_Computer->serverCert.isNull()) {
        return false;
    }
    NvAddress addr = m_Computer->activeAddress;
    uint16_t port = m_Computer->activeHttpsPort;
    if (addr.isNull() || port == 0) {
        return false;
    }
    QString host = addr.address();
    if (host.contains(':')) {
        host = QString("[%1]").arg(host); // bracketed IPv6
    }
    outUrl = QUrl(QString("https://%1:%2/api/v1/clipboard%3").arg(host).arg(port).arg(tail));
    return outUrl.isValid();
}

void ClipboardSync::uploadAndSendRef(const QByteArray& payload, const QString& mime)
{
    QUrl url;
    if (!buildBlobUrl(QStringLiteral("/blob"), url)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ClipboardSync: cannot upload blob (no host context)");
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    req.setRawHeader("X-Clipboard-Mime", mime.toUtf8());
    req.setSslConfiguration(QSslConfiguration::defaultConfiguration());

    QNetworkReply* reply = nam()->post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, mime, payloadSize = payload.size()]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: blob upload failed: %s",
                        reply->errorString().toUtf8().constData());
            return;
        }
        QJsonParseError jerr{};
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &jerr);
        if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: blob upload response not JSON: %s",
                        jerr.errorString().toUtf8().constData());
            return;
        }
        QString id = doc.object().value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: blob upload response missing id");
            return;
        }

        QJsonObject meta;
        meta.insert(QStringLiteral("id"), id);
        meta.insert(QStringLiteral("mime"), mime);
        meta.insert(QStringLiteral("size"), static_cast<double>(payloadSize));
        QByteArray json = QJsonDocument(meta).toJson(QJsonDocument::Compact);

        QByteArray frame;
        if (!encodeFrame(KIND_REF, json, frame)) {
            return;
        }
        int rc = LiSendClipboardData(frame.constData(), frame.size());
        if (rc != 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                         "ClipboardSync: LiSendClipboardData(REF, %d bytes) -> %d",
                         static_cast<int>(frame.size()), rc);
        }
    });
}

void ClipboardSync::fetchRefAndApply(const QString& id, const QString& mime, qint64 advertisedSize)
{
    QUrl url;
    if (!buildBlobUrl(QStringLiteral("/blob/") + id, url)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "ClipboardSync: cannot fetch blob (no host context)");
        return;
    }

    QNetworkRequest req(url);
    req.setSslConfiguration(QSslConfiguration::defaultConfiguration());

    QNetworkReply* reply = nam()->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, mime, advertisedSize]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: blob fetch failed: %s",
                        reply->errorString().toUtf8().constData());
            return;
        }
        QByteArray bytes = reply->readAll();
        if (bytes.isEmpty()) {
            return;
        }
        // Defense: cap actual download size in case the server lied about
        // advertised size or QNAM streamed past the declared length.
        if (bytes.size() > MAX_BLOB_BYTES) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: dropping fetched blob, actual size %lld exceeds %lld cap",
                        static_cast<long long>(bytes.size()),
                        static_cast<long long>(MAX_BLOB_BYTES));
            return;
        }
        // Hard-fail on advertised/actual mismatch when REF declared a size.
        if (advertisedSize > 0 && bytes.size() != advertisedSize) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: dropping fetched blob, size mismatch (got %lld, advertised %lld)",
                        static_cast<long long>(bytes.size()),
                        static_cast<long long>(advertisedSize));
            return;
        }
        // Trust the REF descriptor's mime over Content-Type.
        if (mime == QStringLiteral("image/png")) {
            applyInboundPng(bytes);
        } else if (mime.startsWith(QStringLiteral("text/"))) {
            if (bytes.contains('\0')) {
                return;
            }
            uint64_t hash = hashBytes(bytes);
            recordHash(hash);
            m_SuppressNextChange = true;
            if (SDL_SetClipboardText(bytes.constData()) != 0) {
                m_SuppressNextChange = false;
            }
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "ClipboardSync: dropping fetched blob with unsupported mime '%s'",
                        mime.toUtf8().constData());
        }
    });
}
