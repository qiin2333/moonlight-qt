#include "clipboardsync.h"
#include "clipboardlogging.h"

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
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <cstring>

#ifdef Q_OS_MACOS
extern "C" int ClipboardHelperPasteboardChangeCount();
extern "C" bool ClipboardHelperWritePasteboardCompound(const char* utf8Text,
                                                       const unsigned char* pngBytes,
                                                       int pngLength);
#endif

namespace {
bool isImageLikeMimeFormat(const QString& format)
{
    if (format.startsWith(QStringLiteral("image/"), Qt::CaseInsensitive)
            || format.compare(QStringLiteral("application/x-qt-image"), Qt::CaseInsensitive) == 0) {
        return true;
    }

    // Chromium/Edge/Firefox register Windows clipboard formats like
    // "PNG" / "image/png" / "DeviceIndependentBitmap" directly; Qt
    // surfaces those mangled as application/x-qt-windows-mime;value="...".
    // Recognize the common image-bearing variants so we still treat them
    // as candidates for extractClipboardPng().
    if (format.startsWith(QStringLiteral("application/x-qt-windows-mime;value=\""),
                          Qt::CaseInsensitive)) {
        // Extract inner value name.
        const int prefixLen = QStringLiteral("application/x-qt-windows-mime;value=\"").size();
        const int endQuote = format.indexOf(QLatin1Char('"'), prefixLen);
        if (endQuote > prefixLen) {
            const QString inner = format.mid(prefixLen, endQuote - prefixLen);
            if (inner.compare(QStringLiteral("PNG"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/jpeg"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/bmp"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("image/webp"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("DeviceIndependentBitmap"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("DeviceIndependentBitmapV5"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("JFIF"), Qt::CaseInsensitive) == 0
                    || inner.compare(QStringLiteral("GIF"), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    return false;
}

// PNG ::= 89 50 4E 47 0D 0A 1A 0A (RFC 2083 §3.1). Some Windows apps
// (older Office, IM clients) advertise "image/png" on the clipboard but
// stuff a DIB/BMP payload inside; loadFromData still decodes those by
// sniffing the actual magic, so we cannot trust the mime name alone.
// Only treat the bytes as wire-ready PNG when the magic matches.
bool looksLikePngBytes(const QByteArray& bytes)
{
    static const unsigned char kMagic[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    return bytes.size() >= 8
            && memcmp(bytes.constData(), kMagic, sizeof(kMagic)) == 0;
}
}

ClipboardSync::ClipboardSync(const ClipboardSyncHostContext& hostContext, QObject* parent)
    : QObject(parent),
      m_HostContext(hostContext)
{
    qRegisterMetaType<QByteArray>("QByteArray");
}

ClipboardSync::~ClipboardSync()
{
    stop();
}

bool ClipboardSync::hasFileReferences(const QMimeData* mime)
{
    if (mime == nullptr) {
        return false;
    }

    // Qt normalizes ordinary Finder and Explorer file copies to local URLs.
    // Do not treat remote web URLs as file clipboard data: browsers commonly
    // attach those to otherwise valid copied images.
    if (mime->hasUrls()) {
        const QList<QUrl> urls = mime->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) {
                return true;
            }
        }
    }

    // Some native and promised-file providers expose only a platform format,
    // without a URL that QMimeData can normalize. Match the identifying part
    // so this also covers Qt's application/x-qt-*-mime wrappers.
    static const QStringList fileFormatMarkers {
        QStringLiteral("public.file-url"),
        QStringLiteral("NSFilenamesPboardType"),
        QStringLiteral("promised-file"),
        QStringLiteral("FileNameW"),
        QStringLiteral("FileGroupDescriptor"),
        QStringLiteral("FileContents"),
        QStringLiteral("Shell IDList Array")
    };

    const QStringList formats = mime->formats();
    for (const QString& format : formats) {
        for (const QString& marker : fileFormatMarkers) {
            if (format.contains(marker, Qt::CaseInsensitive)) {
                return true;
            }
        }
    }

    return false;
}

void ClipboardSync::setHostContext(const ClipboardSyncHostContext& hostContext)
{
    m_HostContext = hostContext;
}

void ClipboardSync::start()
{
    if (m_Active) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        ClipboardLog::warn("ClipboardSync: no QClipboard available; sync disabled");
        return;
    }

#ifdef Q_OS_MACOS
    m_LastPasteboardChangeCount = ClipboardHelperPasteboardChangeCount();
    connect(cb, &QClipboard::dataChanged,
            this, &ClipboardSync::onMacClipboardDataChanged,
            Qt::UniqueConnection);
    if (m_PasteboardPollTimer == nullptr) {
        m_PasteboardPollTimer = new QTimer(this);
        connect(m_PasteboardPollTimer, &QTimer::timeout,
                this, &ClipboardSync::pollPasteboardChangeCount);
    }
    m_PasteboardPollTimer->start(500);
#else
    connect(cb, &QClipboard::dataChanged,
            this, &ClipboardSync::onLocalClipboardChanged,
            Qt::UniqueConnection);
#endif

    m_Active = true;
    ClipboardLog::info("ClipboardSync: started (text + PNG, bidirectional)");
}

void ClipboardSync::stop()
{
    if (!m_Active) {
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb != nullptr) {
#ifdef Q_OS_MACOS
        disconnect(cb, &QClipboard::dataChanged,
                   this, &ClipboardSync::onMacClipboardDataChanged);
#else
        disconnect(cb, &QClipboard::dataChanged,
                   this, &ClipboardSync::onLocalClipboardChanged);
#endif
    }

    m_EchoCache.clear();
    m_ImageEchoCache.clear();
    m_PendingSelfWrites = 0;
    const QList<quint32> burstTokens = m_PendingBursts.keys();
    for (quint32 token : burstTokens) {
        removeBurst(token);
    }
#ifdef Q_OS_MACOS
    if (m_PasteboardPollTimer != nullptr) {
        m_PasteboardPollTimer->stop();
    }
    m_LastPasteboardChangeCount = -1;
#endif
    m_Active = false;

    ClipboardLog::info("ClipboardSync: stopped");
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

#ifdef Q_OS_MACOS
void ClipboardSync::onMacClipboardDataChanged()
{
    int changeCount = ClipboardHelperPasteboardChangeCount();
    if (changeCount >= 0) {
        m_LastPasteboardChangeCount = changeCount;
    }

    onLocalClipboardChanged();
}

void ClipboardSync::pollPasteboardChangeCount()
{
    if (!m_Active) {
        return;
    }

    int changeCount = ClipboardHelperPasteboardChangeCount();
    if (changeCount < 0) {
        return;
    }

    if (m_LastPasteboardChangeCount < 0) {
        m_LastPasteboardChangeCount = changeCount;
        return;
    }

    if (changeCount == m_LastPasteboardChangeCount) {
        return;
    }

    m_LastPasteboardChangeCount = changeCount;
    onLocalClipboardChanged();
}
#endif

void ClipboardSync::onIncomingFrame(QByteArray frame)
{
    if (!m_Active) {
        return;
    }

    uint8_t kind = 0;
    quint32 token = 0;
    QByteArray payload;
    if (!decodeFrame(frame, kind, token, payload)) {
        ClipboardLog::warn("ClipboardSync: discarding malformed inbound frame (%d bytes)",
                    static_cast<int>(frame.size()));
        return;
    }

    if (kind == KIND_TEXT) {
        if (token == 0) {
            applyInboundText(payload);
        } else {
            handleBurstFrame(kind, token, payload);
        }
        return;
    }

    if (kind == KIND_PNG) {
        if (token == 0) {
            applyInboundPng(payload);
        } else {
            handleBurstFrame(kind, token, payload);
        }
        return;
    }

    if (kind == KIND_REF) {
        // Payload is a small UTF-8 JSON descriptor: {"id":"...","mime":"...","size":N}.
        QJsonParseError jerr{};
        QJsonDocument doc = QJsonDocument::fromJson(payload, &jerr);
        if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF with bad JSON (%s)",
                        jerr.errorString().toUtf8().constData());
            return;
        }
        QJsonObject obj = doc.object();
        QString id = obj.value(QStringLiteral("id")).toString();
        QString mime = obj.value(QStringLiteral("mime")).toString();
        qint64 size = static_cast<qint64>(obj.value(QStringLiteral("size")).toDouble(0));
        if (id.isEmpty() || id.size() > 128) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF with bad id length");
            return;
        }
        if (size > MAX_BLOB_BYTES) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF, declared size %lld exceeds %lld cap",
                        static_cast<long long>(size), static_cast<long long>(MAX_BLOB_BYTES));
            return;
        }
        if (token == 0) {
            fetchRefAndApply(id, mime, size);
            return;
        }

        // Burst REF: start the fetch now; the bytes land in the aggregator
        // (or apply standalone if the burst window already closed).
        const uint8_t flavorKind = (mime == QStringLiteral("image/png")) ? KIND_PNG
                : mime.startsWith(QStringLiteral("text/")) ? KIND_TEXT : uint8_t(0);
        fetchBlob(id, size, [this, token, flavorKind](const QByteArray& bytes, const QString& fetchedMime) {
            Q_UNUSED(fetchedMime);
            if (flavorKind == 0) {
                ClipboardLog::info("ClipboardSync: dropping fetched blob with unsupported mime");
                burstFlavorResolved(token, 0, QByteArray());
                return;
            }
            burstFlavorResolved(token, flavorKind, bytes);
        });
        return;
    }

    // Unknown kind — ignore.
}

void ClipboardSync::applyInboundText(const QByteArray& payload)
{
    if (payload.contains('\0')) {
        ClipboardLog::warn("ClipboardSync: dropping inbound text payload with embedded NUL");
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    if (hasFileReferences(cb->mimeData())) {
        ClipboardLog::debug("ClipboardSync: preserving local file clipboard; inbound text ignored");
        return;
    }

    // Record hash *before* writing so the dataChanged echo we're about to
    // trigger is suppressed.
    uint64_t hash = hashBytes(payload);
    recordHash(hash);
    ++m_PendingSelfWrites;
    cb->setText(QString::fromUtf8(payload));
}

void ClipboardSync::applyInboundPng(const QByteArray& payload)
{
    QImage image;
    if (!image.loadFromData(payload, "PNG") || image.isNull()) {
        ClipboardLog::warn("ClipboardSync: dropping inbound PNG payload (decode failed, %d bytes)",
                    static_cast<int>(payload.size()));
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    if (hasFileReferences(cb->mimeData())) {
        ClipboardLog::debug("ClipboardSync: preserving local file clipboard; inbound PNG ignored");
        return;
    }

    // Record both identities *before* writing. The byte hash suppresses the
    // immediate dataChanged echo when the platform returns our payload
    // verbatim (Windows). macOS instead re-encodes the image flavors after
    // the write and bumps the pasteboard changeCount a second time, handing
    // us back bytes that never match the wire payload — the pixel hash
    // covers that delayed echo because the re-encodes are lossless.
    uint64_t hash = hashBytes(payload);
    recordHash(hash);
    recordImageHash(hashImagePixels(image));
    ++m_PendingSelfWrites;

    QMimeData* mime = new QMimeData();
    mime->setImageData(image);
    // Provide raw PNG bytes too so apps that prefer image/png over CF_DIB
    // (browsers, modern image editors) get the lossless copy verbatim.
    // Intentionally NOT setting HTML: pasting a giant base64 data: URI into
    // CF_HTML adds nothing useful (Office takes the bitmap path anyway) and
    // some Windows clipboard hooks reject oversize CF_HTML, dropping the
    // entire mime data set on the floor.
    mime->setData(QStringLiteral("image/png"), payload);
    cb->setMimeData(mime);
}

bool ClipboardSync::encodeImageAsPng(const QImage& image,
                                     QByteArray& outPng,
                                     const char* sourceDescription) const
{
    const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
    if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
        ClipboardLog::info("ClipboardSync: image too large from %s (%dx%d), dropping",
                    sourceDescription,
                    image.width(),
                    image.height());
        return false;
    }

    outPng.clear();
    outPng.reserve(64 * 1024);
    QBuffer buf(&outPng);
    buf.open(QIODevice::WriteOnly);
    if (!image.save(&buf, "PNG") || outPng.isEmpty()) {
        ClipboardLog::warn("ClipboardSync: PNG encode failed for %s (%dx%d)",
                    sourceDescription,
                    image.width(),
                    image.height());
        outPng.clear();
        return false;
    }

    return true;
}

void ClipboardSync::sendClipboardPng(const QByteArray& png,
                                     const QString& sourceDescription,
                                     quint32 token)
{
    const QByteArray sourceUtf8 = sourceDescription.isEmpty()
            ? QByteArrayLiteral("unknown source")
            : sourceDescription.toUtf8();

    if (png.size() > MAX_BLOB_BYTES) {
        ClipboardLog::info("ClipboardSync: PNG payload %lld B from %s exceeds %lld B blob cap, dropping",
                    static_cast<long long>(png.size()),
                    sourceUtf8.constData(),
                    static_cast<long long>(MAX_BLOB_BYTES));
        return;
    }

    uint64_t hash = hashBytes(png);
    if (seenRecently(hash)) {
        return;
    }

    // The outbound image may be the delayed echo of an image the host just
    // pushed: the platform clipboard re-encoded it, so its bytes no longer
    // match any recorded hash, but its pixels do. Check the pixel identity
    // before dispatching to either the inline or the out-of-band path.
    QImage decoded;
    if (decoded.loadFromData(png, "PNG") && !decoded.isNull()) {
        uint64_t pixelHash = hashImagePixels(decoded);
        if (pixelHash != 0 && seenImageRecently(pixelHash)) {
            return;
        }
        recordImageHash(pixelHash);
    }

    recordHash(hash);

    if (shouldTransferOutOfBand(png.size())) {
        // Out-of-band path. The hashes recorded above suppress the echo
        // we'll see when the host loops the REF back to us.
        uploadAndSendRef(png, QStringLiteral("image/png"), token);
        return;
    }

    QByteArray frame;
    if (encodeFrame(KIND_PNG, token, png, frame)) {
        ClipboardLog::debug("ClipboardSync: outbound PNG frame queued (%d bytes from %s, token %u)",
                     static_cast<int>(frame.size()),
                     sourceUtf8.constData(),
                     token);
        emit outboundFrame(frame);
    }
}

void ClipboardSync::sendTextOutbound(const QByteArray& utf8, quint32 token)
{
    if (utf8.isEmpty()) {
        return;
    }

    if (utf8.size() > MAX_BLOB_BYTES) {
        ClipboardLog::info("ClipboardSync: text payload %lld B exceeds %lld B blob cap, dropping",
                    static_cast<long long>(utf8.size()),
                    static_cast<long long>(MAX_BLOB_BYTES));
        return;
    }

    uint64_t hash = hashBytes(utf8);
    if (seenRecently(hash)) {
        // This change was the echo of a payload the host just pushed to us.
        return;
    }
    recordHash(hash);

    if (shouldTransferOutOfBand(utf8.size())) {
        ClipboardLog::debug("ClipboardSync: uploading outbound text blob (%lld bytes)",
                     static_cast<long long>(utf8.size()));
        // Sunshine's blob endpoint intentionally accepts only a bare
        // RFC 6838 type/subtype without parameters. KIND_TEXT already
        // defines the blob bytes as UTF-8.
        uploadAndSendRef(utf8, QStringLiteral("text/plain"), token);
        return;
    }

    QByteArray frame;
    if (!encodeFrame(KIND_TEXT, token, utf8, frame)) {
        return;
    }

    ClipboardLog::debug("ClipboardSync: outbound text frame queued (%d bytes, token %u)",
                 static_cast<int>(frame.size()),
                 token);
    emit outboundFrame(frame);
}

bool ClipboardSync::tryExtractImageBytes(const QMimeData* mime,
                                         const QStringList& preferredFormats,
                                         QByteArray& outPng,
                                         QString* outSourceDescription) const
{
    if (mime == nullptr) {
        return false;
    }

    QStringList candidateFormats = preferredFormats;
    for (const QString& format : mime->formats()) {
        if (!isImageLikeMimeFormat(format) || candidateFormats.contains(format, Qt::CaseInsensitive)) {
            continue;
        }
        candidateFormats.append(format);
    }

    for (const QString& format : candidateFormats) {
        QByteArray bytes = mime->data(format);
        if (bytes.isEmpty()) {
            continue;
        }

        QImage image;
        if (!image.loadFromData(bytes)) {
            continue;
        }

        if ((format.compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0
                || format.compare(QStringLiteral("application/x-qt-windows-mime;value=\"PNG\""), Qt::CaseInsensitive) == 0
                || format.compare(QStringLiteral("application/x-qt-windows-mime;value=\"image/png\""), Qt::CaseInsensitive) == 0)
                && looksLikePngBytes(bytes)) {
            const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
            if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                ClipboardLog::info("ClipboardSync: image too large from mime %s (%dx%d), dropping",
                            format.toUtf8().constData(),
                            image.width(),
                            image.height());
                return false;
            }
            outPng = bytes;
        }
        else if (!encodeImageAsPng(image, outPng, format.toUtf8().constData())) {
            continue;
        }

        if (outSourceDescription != nullptr) {
            *outSourceDescription = format;
        }
        return true;
    }

    return false;
}

bool ClipboardSync::tryExtractImageFromUrls(const QMimeData* mime,
                                            QByteArray& outPng,
                                            QString* outSourceDescription) const
{
    if (mime == nullptr || !mime->hasUrls()) {
        return false;
    }

    const QList<QUrl> urls = mime->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }

        QImage image(url.toLocalFile());
        if (image.isNull()) {
            continue;
        }

        if (!encodeImageAsPng(image, outPng, "local file url")) {
            continue;
        }

        if (outSourceDescription != nullptr) {
            *outSourceDescription = QStringLiteral("url:%1").arg(url.toLocalFile());
        }
        return true;
    }

    return false;
}

bool ClipboardSync::tryExtractImageFromHtml(const QMimeData* mime,
                                            QByteArray& outPng,
                                            QString* outSourceDescription) const
{
    if (mime == nullptr || !mime->hasHtml()) {
        return false;
    }

    const QString html = mime->html();
    if (html.isEmpty()) {
        return false;
    }

    static const QRegularExpression imgSrcRegex(
        QStringLiteral("<img[^>]+src\\s*=\\s*['\"]([^'\"]+)['\"]"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dataUrlRegex(
        QStringLiteral("^data:(image/[^;,]+)?;base64,(.+)$"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    QRegularExpressionMatchIterator it = imgSrcRegex.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString src = match.captured(1).trimmed();
        if (src.isEmpty()) {
            continue;
        }

        const QRegularExpressionMatch dataUrlMatch = dataUrlRegex.match(src);
        if (dataUrlMatch.hasMatch()) {
            QByteArray bytes = QByteArray::fromBase64(dataUrlMatch.captured(2).toUtf8());
            if (bytes.isEmpty()) {
                continue;
            }

            QImage image;
            if (!image.loadFromData(bytes)) {
                continue;
            }

            if (dataUrlMatch.captured(1).compare(QStringLiteral("image/png"), Qt::CaseInsensitive) == 0) {
                const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
                if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                    ClipboardLog::info("ClipboardSync: image too large from html data URL (%dx%d), dropping",
                                image.width(),
                                image.height());
                    return false;
                }
                outPng = bytes;
            }
            else if (!encodeImageAsPng(image, outPng, "html data url")) {
                continue;
            }

            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("html:data-url");
            }
            return true;
        }

        const QUrl url(src);
        if (!url.isLocalFile()) {
            continue;
        }

        QImage image(url.toLocalFile());
        if (image.isNull()) {
            continue;
        }

        if (!encodeImageAsPng(image, outPng, "html local file")) {
            continue;
        }

        if (outSourceDescription != nullptr) {
            *outSourceDescription = QStringLiteral("html:%1").arg(url.toLocalFile());
        }
        return true;
    }

    return false;
}

bool ClipboardSync::extractClipboardPng(const QMimeData* mime,
                                        QByteArray& outPng,
                                        QString* outSourceDescription) const
{
    outPng.clear();
    if (outSourceDescription != nullptr) {
        outSourceDescription->clear();
    }

    if (mime == nullptr) {
        return false;
    }

    // Prefer raw image bytes attached by the producer (including our own
    // applyInboundPng which sets "image/png" verbatim) BEFORE falling back
    // to re-encoding mime->imageData(). This keeps echoed inbound PNGs
    // byte-identical so the FNV-1a hash matches the echo cache and we
    // don't ping-pong the same image back to the host.
    const QStringList preferredFormats {
        QStringLiteral("image/png"),
        QStringLiteral("application/x-qt-windows-mime;value=\"PNG\""),
        QStringLiteral("application/x-qt-windows-mime;value=\"image/png\""),
        QStringLiteral("image/tiff"),
        QStringLiteral("image/jpeg"),
        QStringLiteral("image/jpg"),
        QStringLiteral("image/bmp"),
        QStringLiteral("image/webp"),
        QStringLiteral("application/x-qt-image")
    };

    if (tryExtractImageBytes(mime, preferredFormats, outPng, outSourceDescription)) {
        return true;
    }

    if (mime->hasImage()) {
        QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull() && encodeImageAsPng(image, outPng, "mime imageData")) {
            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("imageData");
            }
            return true;
        }
    }

    if (tryExtractImageFromUrls(mime, outPng, outSourceDescription)) {
        return true;
    }

    if (tryExtractImageFromHtml(mime, outPng, outSourceDescription)) {
        return true;
    }

    return false;
}

void ClipboardSync::onLocalClipboardChanged()
{
    if (!m_Active) {
        return;
    }

    if (m_PendingSelfWrites > 0) {
        --m_PendingSelfWrites;
        return;
    }

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    const QMimeData* mime = cb->mimeData();

    // Finder and Explorer file copies may include icon/thumbnail image data.
    // The protocol cannot carry file references, and sending that fallback
    // image allows the host echo to replace the original local file clipboard.
    if (hasFileReferences(mime)) {
        ClipboardLog::debug("ClipboardSync: local file clipboard detected; sync skipped");
        return;
    }

    // Image takes precedence — some applications attach a fallback text label
    // (file path, alt text) alongside the bitmap; we want the picture, not the
    // path. To match HarmonyOS' record iteration behavior more closely, try
    // multiple extraction paths rather than only mime->imageData().
    QByteArray png;
    QString imageSourceDescription;
    const bool havePng = extractClipboardPng(mime, png, &imageSourceDescription);

    // A clipboard change can genuinely carry both flavors (browser image
    // copies attach the source URL, IM clients alt text). Emit both as a
    // compound burst so aggregating peers restore the full clipboard; peers
    // without aggregation apply the frames in order and keep the v1 outcome.
    // Note the burst text frame is inline-only: a REF text frame would race
    // the image frame onto the wire (blob upload latency) and flip legacy
    // peers' final state to text, so oversize text is dropped from bursts.
    QString text = cb->text();
    QByteArray utf8;
    if (!text.isEmpty()) {
        utf8 = text.toUtf8();
    }

    if (havePng) {
        if (!utf8.isEmpty() && utf8.size() <= MAX_PAYLOAD) {
            const quint32 token = nextToken();
            sendTextOutbound(utf8, token);
            sendClipboardPng(png, imageSourceDescription, token);
        } else {
            if (!utf8.isEmpty()) {
                ClipboardLog::info("ClipboardSync: burst text %lld B exceeds inline cap; sending image only",
                            static_cast<long long>(utf8.size()));
            }
            sendClipboardPng(png, imageSourceDescription);
        }
        return;
    }

    if (mime != nullptr) {
        bool hasImageLikeHints = mime->hasImage() || mime->hasUrls() || mime->hasHtml();
        if (!hasImageLikeHints) {
            for (const QString& format : mime->formats()) {
                if (isImageLikeMimeFormat(format)) {
                    hasImageLikeHints = true;
                    break;
                }
            }
        }

        if (hasImageLikeHints) {
            const QString formatsSummary = mime->formats().join(QStringLiteral(", "));
            ClipboardLog::debug("ClipboardSync: no transferable image found in clipboard formats [%s]",
                         formatsSummary.toUtf8().constData());
        }
    }

    sendTextOutbound(utf8, 0);
}

bool ClipboardSync::encodeFrame(uint8_t kind, quint32 token, const QByteArray& payload, QByteArray& outFrame) const
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
    // u32 token (LE) - 0 for standalone frames; frames of one compound burst
    // share a non-zero token (see class comment).
    quint32 tokenLE = qToLittleEndian<quint32>(token);
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
                                quint32& outToken,
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

    quint32 tokenLE = 0;
    memcpy(&tokenLE, p + 2, sizeof(tokenLE));
    outToken = qFromLittleEndian<quint32>(tokenLE);

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

uint64_t ClipboardSync::hashImagePixels(const QImage& image)
{
    if (image.isNull()) {
        return 0;
    }

    // Normalize to one straight-alpha format so identical pixel content
    // hashes identically no matter which flavor (PNG/TIFF/DIB) or Qt format
    // it came back as. Opaque images (the overwhelmingly common clipboard
    // case) survive every lossless conversion bit-exact; semi-transparent
    // pixels may drift by rounding through a premultiplied intermediate,
    // which at worst degrades back to byte-hash-only suppression.
    const QImage canonical = image.convertToFormat(QImage::Format_RGBA8888);
    if (canonical.isNull() || canonical.width() <= 0 || canonical.height() <= 0) {
        return 0;
    }

    uint64_t h = 0xcbf29ce484222325ULL;
    const auto mixBytes = [&h](const uchar* bytes, int count) {
        for (int i = 0; i < count; ++i) {
            h ^= bytes[i];
            h *= 0x100000001b3ULL;
        }
    };

    // Fold dimensions in so equal bytes under different geometry don't collide.
    const quint32 dimensions[2] = {
        static_cast<quint32>(canonical.width()),
        static_cast<quint32>(canonical.height()),
    };
    mixBytes(reinterpret_cast<const uchar*>(dimensions), sizeof(dimensions));

    // Scan row data only, skipping any per-row stride padding.
    const int rowBytes = canonical.width() * 4;
    for (int y = 0; y < canonical.height(); ++y) {
        mixBytes(canonical.constScanLine(y), rowBytes);
    }

    return h;
}

bool ClipboardSync::seenImageRecently(uint64_t pixelHash)
{
    if (pixelHash == 0) {
        return false;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();

    while (!m_ImageEchoCache.isEmpty()
               && (now - m_ImageEchoCache.front().second) > ECHO_TTL_MS) {
        m_ImageEchoCache.removeFirst();
    }

    for (const auto& e : m_ImageEchoCache) {
        if (e.first == pixelHash) {
            return true;
        }
    }
    return false;
}

void ClipboardSync::recordImageHash(uint64_t pixelHash)
{
    if (pixelHash == 0) {
        return;
    }

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_ImageEchoCache.enqueue(qMakePair(pixelHash, now));
    while (m_ImageEchoCache.size() > ECHO_MAX) {
        m_ImageEchoCache.removeFirst();
    }
}

QNetworkAccessManager* ClipboardSync::nam()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
        // Pin the host's self-signed cert exactly like NvHTTP does: ignore
        // SSL errors only when the offending cert matches the pinned one.
        connect(m_Nam, &QNetworkAccessManager::sslErrors, this,
                [this](QNetworkReply* reply, const QList<QSslError>& errors) {
                    if (m_HostContext.serverCertificate.isNull()) {
                        return;
                    }
                    for (const QSslError& e : errors) {
                        if (m_HostContext.serverCertificate != e.certificate()) {
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
    if (m_HostContext.address.isEmpty() ||
            m_HostContext.httpsPort == 0 ||
            m_HostContext.serverCertificate.isNull()) {
        return false;
    }
    QString host = m_HostContext.address;
    if (host.contains(':')) {
        host = QString("[%1]").arg(host); // bracketed IPv6
    }
    outUrl = QUrl(QString("https://%1:%2/api/v1/clipboard%3").arg(host).arg(m_HostContext.httpsPort).arg(tail));
    return outUrl.isValid();
}

void ClipboardSync::uploadAndSendRef(const QByteArray& payload, const QString& mime, quint32 token)
{
    QUrl url;
    if (!buildBlobUrl(QStringLiteral("/blob"), url)) {
        ClipboardLog::warn("ClipboardSync: cannot upload blob (no host context)");
        return;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/octet-stream"));
    req.setRawHeader("X-Clipboard-Mime", mime.toUtf8());
    // Sunshine pins clipboard blob endpoint to its mTLS server (cert
    // required); reuse the same paired client identity nvhttp does, or
    // every fetch/upload fails with a TLS 'certificate required' alert.
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);

    QNetworkReply* reply = nam()->post(req, payload);
    connect(reply, &QNetworkReply::finished, this, [this, reply, mime, token, payloadSize = payload.size()]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            ClipboardLog::warn("ClipboardSync: blob upload failed: %s",
                        reply->errorString().toUtf8().constData());
            return;
        }
        QJsonParseError jerr{};
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &jerr);
        if (jerr.error != QJsonParseError::NoError || !doc.isObject()) {
            ClipboardLog::warn("ClipboardSync: blob upload response not JSON: %s",
                        jerr.errorString().toUtf8().constData());
            return;
        }
        QString id = doc.object().value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            ClipboardLog::warn("ClipboardSync: blob upload response missing id");
            return;
        }

        QJsonObject meta;
        meta.insert(QStringLiteral("id"), id);
        meta.insert(QStringLiteral("mime"), mime);
        meta.insert(QStringLiteral("size"), static_cast<double>(payloadSize));
        QByteArray json = QJsonDocument(meta).toJson(QJsonDocument::Compact);

        QByteArray frame;
        if (!encodeFrame(KIND_REF, token, json, frame)) {
            return;
        }
        ClipboardLog::debug("ClipboardSync: outbound REF frame queued (%d bytes, token %u)",
                     static_cast<int>(frame.size()),
                     token);
        emit outboundFrame(frame);
    });
}

void ClipboardSync::fetchRefAndApply(const QString& id, const QString& mime, qint64 advertisedSize)
{
    fetchBlob(id, advertisedSize, [this, mime](const QByteArray& bytes, const QString& fetchedMime) {
        Q_UNUSED(fetchedMime);
        // Trust the REF descriptor's mime over Content-Type.
        if (mime == QStringLiteral("image/png")) {
            applyInboundPng(bytes);
        } else if (mime.startsWith(QStringLiteral("text/"))) {
            applyInboundText(bytes);
        } else {
            ClipboardLog::info("ClipboardSync: dropping fetched blob with unsupported mime '%s'",
                        mime.toUtf8().constData());
        }
    });
}

void ClipboardSync::fetchBlob(const QString& id,
                              qint64 advertisedSize,
                              const std::function<void(const QByteArray&, const QString&)>& onFetched)
{
    QUrl url;
    if (!buildBlobUrl(QStringLiteral("/blob/") + id, url)) {
        ClipboardLog::warn("ClipboardSync: cannot fetch blob (no host context)");
        return;
    }

    QNetworkRequest req(url);
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);

    QNetworkReply* reply = nam()->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, onFetched, advertisedSize]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            ClipboardLog::warn("ClipboardSync: blob fetch failed: %s",
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
            ClipboardLog::warn("ClipboardSync: dropping fetched blob, actual size %lld exceeds %lld cap",
                        static_cast<long long>(bytes.size()),
                        static_cast<long long>(MAX_BLOB_BYTES));
            return;
        }
        // Hard-fail on advertised/actual mismatch when REF declared a size.
        if (advertisedSize > 0 && bytes.size() != advertisedSize) {
            ClipboardLog::warn("ClipboardSync: dropping fetched blob, size mismatch (got %lld, advertised %lld)",
                        static_cast<long long>(bytes.size()),
                        static_cast<long long>(advertisedSize));
            return;
        }
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        onFetched(bytes, contentType);
    });
}

quint32 ClipboardSync::nextToken()
{
    quint32 token = m_NextToken++;
    if (m_NextToken == 0) {
        m_NextToken = 1;
    }
    return token;
}

void ClipboardSync::handleBurstFrame(uint8_t kind, quint32 token, const QByteArray& payload)
{
    if (!m_PendingBursts.contains(token)) {
        if (m_PendingBursts.size() >= MAX_PENDING_BURSTS) {
            // Defensive bound: a pathological sender must not grow the map
            // without limit; drop the oldest (already-applied) burst.
            ClipboardLog::warn("ClipboardSync: burst backlog exceeded; dropping oldest burst");
            removeBurst(m_PendingBursts.constBegin().key());
        }

        BurstState state;
        state.timer = new QTimer(this);
        state.timer->setSingleShot(true);
        connect(state.timer, &QTimer::timeout, this, [this, token]() {
            removeBurst(token);
        });
        state.timer->start(BURST_RETENTION_MS);
        m_PendingBursts.insert(token, state);
    }

    BurstState& state = m_PendingBursts[token];
    if (kind == KIND_TEXT && !state.haveText) {
        state.haveText = true;
        state.textPayload = payload;
    } else if (kind == KIND_PNG && !state.havePng) {
        state.havePng = true;
        state.pngPayload = payload;
    } else {
        // Duplicate kind for this token; keep the first copy.
        return;
    }

    // Apply-as-arrive: the text applies instantly (no latency vs v1) and is
    // retained so the image can upgrade the clipboard to one compound write
    // carrying both flavors. A png arriving with no retained text applies
    // standalone; compliant senders put text first, so this only happens
    // when a text frame was lost (e.g. queue overflow) — the v1-style flip
    // to image-only is then the correct degradation.
    if (kind == KIND_PNG) {
        if (state.haveText) {
            applyCompound(state.textPayload, state.pngPayload);
        } else {
            applyInboundPng(state.pngPayload);
        }
    }
}

void ClipboardSync::burstFlavorResolved(quint32 token, uint8_t kind, const QByteArray& payload)
{
    if (kind == 0) {
        // Unsupported REF mime: nothing to contribute.
        return;
    }

    if (!m_PendingBursts.contains(token)) {
        // Retention for this token already expired; the late blob still
        // deserves to land on its own.
        if (kind == KIND_TEXT) {
            applyInboundText(payload);
        } else {
            applyInboundPng(payload);
        }
        return;
    }

    handleBurstFrame(kind, token, payload);
}

void ClipboardSync::removeBurst(quint32 token)
{
    const auto it = m_PendingBursts.find(token);
    if (it == m_PendingBursts.end()) {
        return;
    }

    QTimer* timer = it.value().timer;
    m_PendingBursts.erase(it);
    if (timer != nullptr) {
        timer->stop();
        timer->deleteLater();
    }
}

void ClipboardSync::applyCompound(const QByteArray& text, const QByteArray& png)
{
    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    if (hasFileReferences(cb->mimeData())) {
        ClipboardLog::debug("ClipboardSync: preserving local file clipboard; inbound compound ignored");
        return;
    }

    const bool textOk = !text.isEmpty() && !text.contains('\0');
    QImage image;
    if (!image.loadFromData(png, "PNG") || image.isNull()) {
        image = QImage();
    }

    if (textOk && !image.isNull()) {
#ifdef Q_OS_MACOS
        // Qt's macOS clipboard backend drops the text flavor from a
        // QMimeData that also carries an image, so compound writes go
        // through the native pasteboard instead.
        if (ClipboardHelperWritePasteboardCompound(text.constData(),
                                                   reinterpret_cast<const unsigned char*>(png.constData()),
                                                   png.size())) {
            recordHash(hashBytes(text));
            recordHash(hashBytes(png));
            recordImageHash(hashImagePixels(image));
            ++m_PendingSelfWrites;
            ClipboardLog::debug("ClipboardSync: applied compound clipboard (text %d B + PNG %d B)",
                         text.size(),
                         png.size());
            return;
        }
        ClipboardLog::warn("ClipboardSync: native compound pasteboard write failed; falling back to image only");
#endif

        // Record every identity before writing: the byte hashes suppress an
        // immediate verbatim echo, the pixel hash suppresses the delayed
        // platform re-encode echo, and the compound write fires exactly one
        // dataChanged, so one pending-self-write slot suffices.
        recordHash(hashBytes(text));
        recordHash(hashBytes(png));
        recordImageHash(hashImagePixels(image));
        ++m_PendingSelfWrites;

        QMimeData* mime = new QMimeData();
        mime->setText(QString::fromUtf8(text));
        mime->setImageData(image);
        // Provide raw PNG bytes too so apps that prefer image/png over
        // CF_DIB get the lossless copy verbatim (mirrors applyInboundPng).
        mime->setData(QStringLiteral("image/png"), png);
        cb->setMimeData(mime);
        ClipboardLog::debug("ClipboardSync: applied compound clipboard (text %d B + PNG %d B)",
                     text.size(),
                     png.size());
        return;
    }

    // One flavor was unusable — degrade to a single-flavor apply (each of
    // these records its own echo identities).
    if (textOk) {
        applyInboundText(text);
    } else if (!image.isNull()) {
        applyInboundPng(png);
    }
}
