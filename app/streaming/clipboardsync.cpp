#include "clipboardsync.h"
#include "clipboardlogging.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QSslConfiguration>
#include <QSslError>
#include <QTimer>
#include <QUrl>
#include <QtEndian>

#include <cstring>
#include <memory>
#include <cmath>

#ifdef Q_OS_MACOS
extern "C" int ClipboardHelperPasteboardChangeCount();
extern "C" bool ClipboardHelperWritePasteboardCompound(const char* utf8Text,
                                                       const unsigned char* pngBytes,
                                                       int pngLength);
extern "C" bool ClipboardHelperIsFinderPasteboard();
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
    m_Clock.start();
    // Tokens are shared with peers. Avoid every helper restart beginning at 1.
    m_NextToken = QRandomGenerator::global()->generate();
    if (m_NextToken == 0) {
        m_NextToken = 1;
    }
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
    beginClipboardChange();
    m_RetiredTokens.clear();
    m_HaveClipboardSnapshot = false;
    if (m_HostContext.address == hostContext.address &&
        m_HostContext.httpsPort == hostContext.httpsPort &&
        m_HostContext.serverCertificate == hostContext.serverCertificate &&
        m_HostContext.clientCertificate == hostContext.clientCertificate &&
        m_HostContext.clientPrivateKey == hostContext.clientPrivateKey) {
        return;
    }
    // Pooled TLS connections must not survive a change of paired identity.
    delete m_Nam;
    m_Nam = nullptr;
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

    m_Active = false;
    beginClipboardChange();
    m_RetiredTokens.clear();
    m_HaveClipboardSnapshot = false;
#ifdef Q_OS_MACOS
    if (m_PasteboardPollTimer != nullptr) {
        m_PasteboardPollTimer->stop();
    }
    m_LastPasteboardChangeCount = -1;
#endif

    ClipboardLog::info("ClipboardSync: stopped");
}

void ClipboardSync::handleIncomingFrame(const char* data, int length)
{
    if (data == nullptr || length < 10 || length > MAX_PAYLOAD + 10) {
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

    if (kind != KIND_TEXT && kind != KIND_PNG && kind != KIND_REF) {
        return;
    }
    QString id, mime;
    qint64 size = 0;
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
        id = obj.value(QStringLiteral("id")).toString();
        mime = obj.value(QStringLiteral("mime")).toString();
        const double declaredSize = obj.value(QStringLiteral("size")).toDouble(-1);
        if (!std::isfinite(declaredSize) || declaredSize < 0 || declaredSize > MAX_BLOB_BYTES ||
            std::floor(declaredSize) != declaredSize) {
            return;
        }
        size = static_cast<qint64>(declaredSize);
        if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(id).hasMatch()) {
            ClipboardLog::warn("ClipboardSync: dropping inbound REF with invalid blob id");
            return;
        }
        if (mime != QStringLiteral("image/png") && !mime.startsWith(QStringLiteral("text/"))) {
            return;
        }
    }
    // A server may broadcast our own frames back while an image upload is
    // still pending. Such echoes must not cancel that upload or split the
    // local compound clipboard into a text-only value.
    if (m_HaveClipboardSnapshot && token != 0 && token == m_OutgoingToken) {
        if (kind == KIND_TEXT && hashBytes(payload) == m_LastTextHash) {
            return;
        }
        QImage image;
        if (kind == KIND_PNG && decodeImage(payload, image, "PNG") &&
            hashImagePixels(image) == m_LastImageHash) {
            return;
        }
    }
    if (kind == KIND_REF && !m_LastOutboundRef.isEmpty() && payload == m_LastOutboundRef) {
        return;
    }
    while (!m_RetiredTokens.isEmpty() &&
           m_Clock.elapsed() - m_RetiredTokens.head().second > 30000) {
        m_RetiredTokens.dequeue();
    }
    for (const auto& retired : m_RetiredTokens) {
        if (token != 0 && token == retired.first) {
            return;
        }
    }
    if (token == 0 || token != m_CurrentToken) {
        beginClipboardChange(token);
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
        if (token == 0) {
            fetchRefAndApply(id, mime, size);
            return;
        }

        // Burst REF: start the fetch now; the bytes land in the aggregator
        // (or apply standalone if the burst window already closed).
        // Unsupported MIME types were rejected before starting this change.
        const uint8_t flavorKind = (mime == QStringLiteral("image/png")) ? KIND_PNG : KIND_TEXT;
        fetchBlob(id, size,
                  [this, token, flavorKind](const QByteArray& bytes, const QString& fetchedMime) {
                      Q_UNUSED(fetchedMime);
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

    rememberClipboard(QString::fromUtf8(payload).toUtf8(), QImage());
    cb->setText(QString::fromUtf8(payload));
}

void ClipboardSync::applyInboundPng(const QByteArray& payload)
{
    QImage image;
    if (!decodeImage(payload, image, "PNG") || image.isNull()) {
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

    rememberClipboard(QByteArray(), image);

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

    if (shouldTransferOutOfBand(png.size())) {
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

    if (token == 0 && shouldTransferOutOfBand(utf8.size())) {
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
        if (!decodeImage(bytes, image)) {
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
            if (!decodeImage(bytes, image)) {
                continue;
            }

            if (looksLikePngBytes(bytes)) {
                const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
                if (pixels <= 0 || pixels > MAX_IMAGE_PIXELS) {
                    ClipboardLog::info("ClipboardSync: image too large from html data URL (%dx%d), dropping",
                                image.width(),
                                image.height());
                    return false;
                }
                outPng = bytes;
            } else if (!encodeImageAsPng(image, outPng, "html data url")) {
                continue;
            }

            if (outSourceDescription != nullptr) {
                *outSourceDescription = QStringLiteral("html:data-url");
            }
            return true;
        }

        // Never open local files referenced by markup from another application.
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

    QClipboard* cb = QGuiApplication::clipboard();
    if (cb == nullptr) {
        return;
    }

    const QMimeData* mime = cb->mimeData();

    // File copies must not leak their contents as clipboard messages:
    // Finder/Explorer copies would send the file name as text and their
    // icon bitmap as an image. But document- and chat-app image copies
    // (Preview, WeChat/QQ message copies) also attach local file
    // references alongside the real bitmap — those must still sync, or
    // the most common macOS image-copy flows silently do nothing.
    // Distinguish by origin: Finder stamps its file clipboards with
    // noderef/fndf markers; anything else carrying a transferable image
    // is treated as an image copy. File references without a usable
    // image stay skipped (the inbound guards below still protect a
    // pending local file paste from being clobbered by remote content).
    bool finderOrigin = false;
#ifdef Q_OS_MACOS
    // Finder stamps its file copies with node-reference flavors that Qt
    // never surfaces in QMimeData::formats(); ask the raw pasteboard.
    finderOrigin = ClipboardHelperIsFinderPasteboard();
#endif
    if (mime != nullptr) {
        for (const QString& format : mime->formats()) {
            if (format.contains(QStringLiteral("Shell IDList Array"), Qt::CaseInsensitive) ||
                format.contains(QStringLiteral("FileGroupDescriptor"), Qt::CaseInsensitive)) {
                finderOrigin = true;
            }
        }
    }
    if (finderOrigin) {
        beginClipboardChange();
        m_HaveClipboardSnapshot = false;
        ClipboardLog::debug("ClipboardSync: Finder file clipboard detected; sync skipped");
        return;
    }

    // Image takes precedence — some applications attach a fallback text label
    // (file path, alt text) alongside the bitmap; we want the picture, not the
    // path. To match HarmonyOS' record iteration behavior more closely, try
    // multiple extraction paths rather than only mime->imageData().
    QByteArray png;
    QString imageSourceDescription;
    const bool havePng = extractClipboardPng(mime, png, &imageSourceDescription);
    if (hasFileReferences(mime) && !havePng) {
        beginClipboardChange();
        m_HaveClipboardSnapshot = false;
        ClipboardLog::debug(
            "ClipboardSync: file clipboard without transferable image; sync skipped");
        return;
    }

    // A clipboard change can genuinely carry both flavors (browser image
    // copies attach the source URL, IM clients alt text). Emit both as a
    // compound burst so aggregating peers restore the full clipboard; peers
    // without aggregation apply the frames in order and keep the v1 outcome.
    // Note the burst text frame is inline-only: a REF text frame would race
    // the image frame onto the wire (blob upload latency) and flip legacy
    // peers' final state to text, so oversize text is dropped from bursts.
    QString text = hasFileReferences(mime) ? QString() : cb->text();
    QByteArray utf8;
    if (!text.isEmpty()) {
        utf8 = text.toUtf8();
    }

    QImage image;
    if (havePng && !decodeImage(png, image, "PNG")) {
        return;
    }
    const uint64_t textHash = hashBytes(utf8);
    const uint64_t imageHash = hashImagePixels(image);
    if (m_HaveClipboardSnapshot && textHash == m_LastTextHash && imageHash == m_LastImageHash) {
        return;
    }
    beginClipboardChange();
    rememberClipboard(utf8, image);

    if (havePng) {
        if (!utf8.isEmpty() && utf8.size() <= MAX_INLINE_PAYLOAD) {
            const quint32 token = nextToken();
            m_OutgoingToken = token;
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
    if (payload.size() > MAX_INLINE_PAYLOAD) {
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

    if (length > MAX_PAYLOAD || length != static_cast<quint32>(frame.size() - 10)) {
        return false;
    }

    outPayload = QByteArray(reinterpret_cast<const char*>(p + 10),
                            static_cast<int>(length));
    return true;
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

bool ClipboardSync::decodeImage(const QByteArray& bytes, QImage& image, const char* format)
{
    if (bytes.isEmpty() || bytes.size() > MAX_BLOB_BYTES) {
        return false;
    }
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, format ? QByteArray(format) : QByteArray());
    const QSize size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 ||
        qint64(size.width()) * size.height() > MAX_IMAGE_PIXELS) {
        return false;
    }
    image = reader.read();
    return !image.isNull() && qint64(image.width()) * image.height() <= MAX_IMAGE_PIXELS;
}

void ClipboardSync::rememberClipboard(const QByteArray& text, const QImage& image)
{
    m_HaveClipboardSnapshot = true;
    m_LastTextHash = hashBytes(text);
    m_LastImageHash = hashImagePixels(image);
}

void ClipboardSync::beginClipboardChange(quint32 token)
{
    ++m_Revision;
    if (m_CurrentToken != 0) {
        m_RetiredTokens.enqueue(qMakePair(m_CurrentToken, m_Clock.elapsed()));
        while (m_RetiredTokens.size() > MAX_PENDING_BURSTS) {
            m_RetiredTokens.dequeue();
        }
    }
    m_CurrentToken = token;
    m_OutgoingToken = 0;
    m_LastOutboundRef.clear();
    const auto replies = m_BlobReplies.values();
    for (QNetworkReply* reply : replies) {
        reply->abort();
    }
    const auto tokens = m_PendingBursts.keys();
    for (quint32 oldToken : tokens) {
        removeBurst(oldToken);
    }
}

QNetworkAccessManager* ClipboardSync::nam()
{
    if (m_Nam == nullptr) {
        m_Nam = new QNetworkAccessManager(this);
        // Validate every new TLS connection before HTTP data is sent. Reused
        // connections belong to this manager's immutable paired identity.
        const QSslCertificate pinned = m_HostContext.serverCertificate;
        connect(m_Nam, &QNetworkAccessManager::encrypted, this, [pinned](QNetworkReply* reply) {
            if (pinned.isNull() || reply->sslConfiguration().peerCertificate() != pinned) {
                reply->abort();
            }
        });
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
    outUrl = QUrl();
    outUrl.setScheme(QStringLiteral("https"));
    outUrl.setHost(m_HostContext.address);
    outUrl.setPort(m_HostContext.httpsPort);
    outUrl.setPath(QStringLiteral("/api/v1/clipboard") + tail);
    return outUrl.isValid();
}

void ClipboardSync::readBlobReply(
    QNetworkReply* reply, qint64 limit,
    const std::function<void(const QByteArray&, const QString&)>& onFetched)
{
    m_BlobReplies.insert(reply);
    const quint64 revision = m_Revision;
    const QSslCertificate pinned = m_HostContext.serverCertificate;
    const auto bytes = std::make_shared<QByteArray>();
    reply->setReadBufferSize(64 * 1024);
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(30000);
    const auto drain = [reply, bytes, limit]() {
        if (!reply->isOpen() || reply->error() != QNetworkReply::NoError) {
            return;
        }
        bytes->append(reply->read(limit - bytes->size() + 1));
        if (bytes->size() > limit) {
            reply->abort();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::metaDataChanged, this, [reply, limit]() {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit) {
            reply->abort();
        }
    });
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, bytes, revision, pinned, timeout, onFetched, limit]() {
            timeout->stop();
            m_BlobReplies.remove(reply);
            reply->deleteLater();
            if (!m_Active || revision != m_Revision) {
                return;
            }
            // Read the last bytes even when the backend omitted a final readyRead.
            // Do not re-enter finished via abort() while draining the completed reply.
            if (reply->error() == QNetworkReply::NoError && reply->isOpen()) {
                bytes->append(reply->read(limit - bytes->size() + 1));
            }
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || bytes->size() > limit ||
                pinned.isNull() || reply->sslConfiguration().peerCertificate() != pinned ||
                status < 200 || status >= 300) {
                ClipboardLog::warn("ClipboardSync: blob transfer failed or exceeded its limits");
                // Let an explicit repeat copy retry a failed upload.
                if (reply->operation() == QNetworkAccessManager::PostOperation) {
                    m_HaveClipboardSnapshot = false;
                }
                return;
            }
            onFetched(*bytes, reply->header(QNetworkRequest::ContentTypeHeader).toString());
        });
}

void ClipboardSync::uploadAndSendRef(const QByteArray& payload, const QString& mime, quint32 token)
{
    QUrl url;
    if (!m_Active || payload.size() > MAX_BLOB_BYTES || m_BlobReplies.size() >= 4 ||
        !buildBlobUrl(QStringLiteral("/blob"), url)) {
        m_HaveClipboardSnapshot = false;
        return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    req.setRawHeader("X-Clipboard-Mime", mime.toUtf8());
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);

    readBlobReply(nam()->post(req, payload), 16 * 1024,
                  [this, mime, token, payloadSize = payload.size()](const QByteArray& response,
                                                                    const QString&) {
                      const QJsonDocument doc = QJsonDocument::fromJson(response);
                      const QString id = doc.object().value(QStringLiteral("id")).toString();
                      if (!QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"))
                               .match(id)
                               .hasMatch()) {
                          m_HaveClipboardSnapshot = false;
                          return;
                      }
                      QJsonObject meta;
                      meta.insert(QStringLiteral("id"), id);
                      meta.insert(QStringLiteral("mime"), mime);
                      meta.insert(QStringLiteral("size"), static_cast<double>(payloadSize));
                      QByteArray frame;
                      m_LastOutboundRef = QJsonDocument(meta).toJson(QJsonDocument::Compact);
                      if (encodeFrame(KIND_REF, token, m_LastOutboundRef, frame)) {
                          emit outboundFrame(frame);
                      }
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
    if (!m_Active || m_BlobReplies.size() >= 4 ||
        !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,128}$")).match(id).hasMatch() ||
        !buildBlobUrl(QStringLiteral("/blob/") + id, url)) {
        return;
    }
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(m_HostContext.clientCertificate);
    sslConfig.setPrivateKey(m_HostContext.clientPrivateKey);
    req.setSslConfiguration(sslConfig);
    readBlobReply(nam()->get(req), advertisedSize > 0 ? advertisedSize : MAX_BLOB_BYTES,
                  [advertisedSize, onFetched](const QByteArray& bytes, const QString& mime) {
                      if (advertisedSize > 0 && bytes.size() != advertisedSize) {
                          ClipboardLog::warn("ClipboardSync: blob size mismatch");
                          return;
                      }
                      onFetched(bytes, mime);
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
            if (token == m_CurrentToken && !m_BlobReplies.isEmpty()) {
                m_PendingBursts[token].timer->start(BURST_RETENTION_MS);
                return;
            }
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

    if (state.haveText && state.havePng) {
        applyCompound(state.textPayload, state.pngPayload);
    } else if (kind == KIND_TEXT) {
        applyInboundText(state.textPayload);
    } else {
        applyInboundPng(state.pngPayload);
    }
}

void ClipboardSync::burstFlavorResolved(quint32 token, uint8_t kind, const QByteArray& payload)
{
    if (kind == 0) {
        // Unsupported REF mime: nothing to contribute.
        return;
    }

    if (token != m_CurrentToken) {
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
    if (!decodeImage(png, image, "PNG") || image.isNull()) {
        image = QImage();
    }

    if (textOk && !image.isNull()) {
#ifdef Q_OS_MACOS
        // Qt's macOS clipboard backend drops the text flavor from a
        // QMimeData that also carries an image, so compound writes go
        // through the native pasteboard instead.
        rememberClipboard(QString::fromUtf8(text).toUtf8(), image);
        if (ClipboardHelperWritePasteboardCompound(
                text.constData(), reinterpret_cast<const unsigned char*>(png.constData()),
                png.size())) {
            ClipboardLog::debug("ClipboardSync: applied compound clipboard (text %d B + PNG %d B)",
                         text.size(),
                         png.size());
            return;
        }
        ClipboardLog::warn("ClipboardSync: native compound pasteboard write failed; falling back to image only");
#endif

        rememberClipboard(QString::fromUtf8(text).toUtf8(), image);

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
