#include "fileoffer.h"
#include "streaming/clipboardlogging.h"
#include "backend/pairedcertificate.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSslConfiguration>
#include <QThread>
#include <QUrl>
#include <QRandomGenerator>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>

namespace {

// Largest integer a JSON number carries exactly.
constexpr double MAX_JSON_INTEGER = 9007199254740992.0;

bool fail(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool readUnsigned(const QJsonValue& value, quint64& out)
{
    if (!value.isDouble()) {
        return false;
    }
    const double d = value.toDouble();
    if (!std::isfinite(d) || d < 0 || d > MAX_JSON_INTEGER || std::floor(d) != d) {
        return false;
    }
    out = static_cast<quint64>(d);
    return true;
}

// A name Windows refuses or treats as a device, whatever the extension.
bool isReservedName(const QString& component)
{
    static const QSet<QString> reserved = {
        QStringLiteral("CON"),
        QStringLiteral("PRN"),
        QStringLiteral("AUX"),
        QStringLiteral("NUL"),
        QStringLiteral("COM1"),
        QStringLiteral("COM2"),
        QStringLiteral("COM3"),
        QStringLiteral("COM4"),
        QStringLiteral("COM5"),
        QStringLiteral("COM6"),
        QStringLiteral("COM7"),
        QStringLiteral("COM8"),
        QStringLiteral("COM9"),
        QStringLiteral("LPT1"),
        QStringLiteral("LPT2"),
        QStringLiteral("LPT3"),
        QStringLiteral("LPT4"),
        QStringLiteral("LPT5"),
        QStringLiteral("LPT6"),
        QStringLiteral("LPT7"),
        QStringLiteral("LPT8"),
        QStringLiteral("LPT9"),
        QStringLiteral("CONIN$"),
        QStringLiteral("CONOUT$"),
        // Windows also reserves COM and LPT with a superscript digit.
        QStringLiteral("COM\u00B9"),
        QStringLiteral("COM\u00B2"),
        QStringLiteral("COM\u00B3"),
        QStringLiteral("LPT\u00B9"),
        QStringLiteral("LPT\u00B2"),
        QStringLiteral("LPT\u00B3"),
    };
    const QString base = component.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    return reserved.contains(base);
}

bool isValidComponent(const QString& component)
{
    if (component.isEmpty() || component == QStringLiteral(".") ||
        component == QStringLiteral("..")) {
        return false;
    }
    for (const QChar c : component) {
        if (c.unicode() < 0x20 || QStringLiteral("<>:\"/\\|?*").contains(c)) {
            return false;
        }
    }
    // Windows strips a trailing dot or space, so "a." would land as "a".
    if (component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' '))) {
        return false;
    }
    return !isReservedName(component);
}

} // namespace

bool FileOffer::parse(const QByteArray& json, FileOffer& out, QString* error)
{
    out = FileOffer();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(error, QStringLiteral("not a JSON object"));
    }
    const QJsonObject obj = doc.object();

    out.id = obj.value(QStringLiteral("id")).toString();
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (!idPattern.match(out.id).hasMatch()) {
        return fail(error, QStringLiteral("bad id"));
    }
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (!type.isEmpty() && type != QStringLiteral("file") && type != QStringLiteral("files")) {
        return fail(error, QStringLiteral("unsupported type"));
    }

    QJsonArray files;
    const QJsonValue filesValue = obj.value(QStringLiteral("files"));
    if (filesValue.isArray()) {
        files = filesValue.toArray();
        out.source = obj.value(QStringLiteral("source")).toString() == QStringLiteral("send")
                         ? QStringLiteral("send")
                         : QStringLiteral("clipboard");
    } else if (type != QStringLiteral("files")) {
        // The first version: one file, "send to client", at its own route.
        const QString downloadUrl = obj.value(QStringLiteral("download_url")).toString();
        if (!downloadUrl.isEmpty() &&
            downloadUrl != QStringLiteral("/api/v1/file-transfer/") + out.id) {
            return fail(error, QStringLiteral("unexpected download_url"));
        }
        files.append(QJsonObject{ { QStringLiteral("path"), obj.value(QStringLiteral("name")) },
                                  { QStringLiteral("size"), obj.value(QStringLiteral("size")) },
                                  { QStringLiteral("dir"), false } });
        out.source = QStringLiteral("send");
        out.singleFileRoute = true;
    } else {
        return fail(error, QStringLiteral("no files"));
    }
    if (files.isEmpty() || files.size() > MAX_ENTRIES) {
        return fail(error, QStringLiteral("bad entry count"));
    }

    QSet<QString> seen;    // every path, case-folded (Windows names are case-insensitive)
    QSet<QString> folders; // folder paths, case-folded
    out.entries.reserve(static_cast<size_t>(files.size()));
    for (const QJsonValue& value : files) {
        if (!value.isObject()) {
            return fail(error, QStringLiteral("bad entry"));
        }
        const QJsonObject file = value.toObject();
        const QString path = file.value(QStringLiteral("path")).toString();
        const bool dir = file.value(QStringLiteral("dir")).toBool(false);
        quint64 size = 0;
        if (!readUnsigned(file.value(QStringLiteral("size")), size) || (dir && size != 0)) {
            return fail(error, QStringLiteral("bad size"));
        }
        quint64 mtime = 0;
        if (file.contains(QStringLiteral("mtime")) &&
            !readUnsigned(file.value(QStringLiteral("mtime")), mtime)) {
            mtime = 0;
        }

        const QStringList components = path.split(QLatin1Char('/'));
        for (const QString& component : components) {
            if (!isValidComponent(component)) {
                return fail(error, QStringLiteral("bad path"));
            }
        }
        const QString windowsPath = components.join(QLatin1Char('\\'));
        if (windowsPath.size() > MAX_PATH_UNITS) {
            return fail(error, QStringLiteral("path too long"));
        }
        const QString folded = windowsPath.toCaseFolded();
        if (seen.contains(folded)) {
            return fail(error, QStringLiteral("duplicate path"));
        }
        if (components.size() > 1) {
            const QString parent =
                components.mid(0, components.size() - 1).join(QLatin1Char('\\')).toCaseFolded();
            if (!folders.contains(parent)) {
                return fail(error, QStringLiteral("entry before its folder"));
            }
        }
        seen.insert(folded);
        if (dir) {
            folders.insert(folded);
        }

        VirtualFiles::Entry entry;
        entry.path = windowsPath.toStdU16String();
        entry.size = size;
        entry.dir = dir;
        entry.mtime = static_cast<std::int64_t>(mtime);
        out.entries.push_back(std::move(entry));
        if (out.totalSize > std::numeric_limits<quint64>::max() - size) {
            return fail(error, QStringLiteral("bad size"));
        }
        out.totalSize += size;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Downloader

namespace {

// Bytes held per open stream before the socket is left to wait.
constexpr qint64 HIGH_WATER = 4 * 1024 * 1024;
constexpr qint64 LOW_WATER = 1 * 1024 * 1024;
constexpr qint64 REPLY_BUFFER = 1 * 1024 * 1024;
// A paste that sees no byte for this long fails instead of hanging the target.
constexpr std::chrono::seconds STALL_TIMEOUT{ 60 };

// "bytes <offset>-<total - 1>/<total>": the range a resumed read asked for.
bool isContentRange(const QByteArray& header, quint64 offset, quint64 total)
{
    static const QRegularExpression pattern(
        QStringLiteral("^bytes (\\d{1,20})-(\\d{1,20})/(\\d{1,20})$"));
    const QRegularExpressionMatch match = pattern.match(QString::fromLatin1(header.trimmed()));
    if (!match.hasMatch() || total == 0) {
        return false;
    }
    bool okFirst = false, okLast = false, okTotal = false;
    const quint64 first = match.captured(1).toULongLong(&okFirst);
    const quint64 last = match.captured(2).toULongLong(&okLast);
    const quint64 size = match.captured(3).toULongLong(&okTotal);
    return okFirst && okLast && okTotal && first == offset && last == total - 1 && size == total;
}

struct StreamState
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<QByteArray> chunks;
    qint64 headOffset = 0; // bytes of chunks.front() already handed out
    qint64 buffered = 0;   // bytes waiting in chunks
    quint64 expected = 0;  // bytes this response must carry
    quint64 received = 0;
    quint64 delivered = 0; // bytes handed to the reader
    bool done = false;
    bool failed = false;
    VirtualFiles::Failure failure = VirtualFiles::Failure::Other; // the first cause, once failed

    // Worker thread only.
    QPointer<QNetworkReply> reply;
};

} // namespace

// What Sunshine's answer to a download means: 409 the file changed since the
// copy, 404 no such offer (expired) or entry, 410 the file can no longer be read.
static VirtualFiles::Failure failureForStatus(int status)
{
    switch (status) {
    case 409:
        return VirtualFiles::Failure::Changed;
    case 404:
    case 410:
        return VirtualFiles::Failure::Gone;
    default:
        return VirtualFiles::Failure::Other;
    }
}

struct FileOfferDownloader::Core
{
    ClipboardSyncHostContext host;
    QString offerId;
    bool singleFileRoute = false;
    std::vector<VirtualFiles::Entry> entries;

    QThread thread;
    QObject* context = nullptr;           // lives on `thread`
    QNetworkAccessManager* nam = nullptr; // child of context

    Core(const ClipboardSyncHostContext& h, const FileOffer& offer)
        : host(h), offerId(offer.id), singleFileRoute(offer.singleFileRoute), entries(offer.entries)
    {
        thread.setObjectName(QStringLiteral("file-offer-download"));
        context = new QObject();
        context->moveToThread(&thread);
        thread.start();
        QMetaObject::invokeMethod(
            context,
            [this]() {
                nam = new QNetworkAccessManager(context);
                const QSslCertificate pinned = host.serverCertificate;
                PairedCertificate::enforce(nam, context, pinned);
                QObject::connect(nam, &QNetworkAccessManager::sslErrors, context,
                                 [pinned](QNetworkReply* reply, const QList<QSslError>& errors) {
                                     if (PairedCertificate::canIgnoreErrors(pinned, errors)) {
                                         reply->ignoreSslErrors(errors);
                                     }
                                 });
            },
            Qt::BlockingQueuedConnection);
    }

    ~Core()
    {
        // Deleting the context on its thread aborts and frees every reply.
        QObject* doomed = context;
        QMetaObject::invokeMethod(
            doomed, [doomed]() { delete doomed; }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }

    // Worker thread: move what the reply holds into the stream, up to HIGH_WATER.
    static void pump(const std::shared_ptr<StreamState>& st)
    {
        QNetworkReply* reply = st->reply;
        if (reply == nullptr) {
            return;
        }
        bool overrun = false;
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (st->failed) {
                return;
            }
            while (st->buffered < HIGH_WATER && reply->bytesAvailable() > 0) {
                QByteArray chunk = reply->read(HIGH_WATER - st->buffered);
                if (chunk.isEmpty()) {
                    break;
                }
                st->received += static_cast<quint64>(chunk.size());
                st->buffered += chunk.size();
                st->chunks.push_back(std::move(chunk));
            }
            if (st->received > st->expected) {
                st->failed = true;
                overrun = true;
            }
        }
        st->changed.notify_all();
        if (overrun) {
            reply->abort();
        }
    }

    static void failStream(const std::shared_ptr<StreamState>& st,
                           VirtualFiles::Failure failure = VirtualFiles::Failure::Other)
    {
        {
            std::lock_guard<std::mutex> lock(st->mutex);
            if (!st->failed) {
                st->failure = failure;
            }
            st->failed = true;
        }
        st->changed.notify_all();
        if (st->reply != nullptr) {
            st->reply->abort();
        }
    }

    // Worker thread.
    void start(const std::shared_ptr<StreamState>& st, std::size_t index, quint64 offset)
    {
        QUrl url;
        url.setScheme(QStringLiteral("https"));
        url.setHost(host.address);
        url.setPort(host.httpsPort);
        url.setPath(singleFileRoute
                        ? QStringLiteral("/api/v1/file-transfer/%1").arg(offerId)
                        : QStringLiteral("/api/v1/file-transfer/%1/%2").arg(offerId).arg(index));

        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
        QSslConfiguration ssl(QSslConfiguration::defaultConfiguration());
        ssl.setLocalCertificate(host.clientCertificate);
        ssl.setPrivateKey(host.clientPrivateKey);
        request.setSslConfiguration(ssl);
        if (offset != 0) {
            request.setRawHeader("Range",
                                 QByteArrayLiteral("bytes=") + QByteArray::number(offset) + '-');
        }

        QNetworkReply* reply = nam->get(request);
        reply->setReadBufferSize(REPLY_BUFFER);
        st->reply = reply;
        const int wanted = offset != 0 ? 206 : 200;
        const QSslCertificate pinned = host.serverCertificate;

        const quint64 total = offset + st->expected;
        QObject::connect(
            reply, &QNetworkReply::metaDataChanged, reply, [st, reply, wanted, offset, total]() {
                const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                if (status.isValid() && status.toInt() != wanted) {
                    ClipboardLog::warn("FileOffer: download answered HTTP %d", status.toInt());
                    failStream(st, failureForStatus(status.toInt()));
                    return;
                }
                // A resumed read must be exactly the rest of the file.
                if (status.isValid() && wanted == 206 &&
                    !isContentRange(reply->rawHeader("Content-Range"), offset, total)) {
                    ClipboardLog::warn("FileOffer: resumed download answered another range (%s)",
                                       reply->rawHeader("Content-Range").constData());
                    failStream(st);
                }
            });
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [st]() { pump(st); });
        QObject::connect(reply, &QNetworkReply::finished, reply, [st, reply, wanted, pinned]() {
            const bool transportOk =
                reply->error() == QNetworkReply::NoError &&
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == wanted &&
                PairedCertificate::matches(pinned, reply->sslConfiguration().peerCertificate());
            {
                std::lock_guard<std::mutex> lock(st->mutex);
                if (transportOk && !st->failed) {
                    // The reply holds at most REPLY_BUFFER more; take it all.
                    while (reply->bytesAvailable() > 0) {
                        QByteArray chunk = reply->readAll();
                        if (chunk.isEmpty()) {
                            break;
                        }
                        st->received += static_cast<quint64>(chunk.size());
                        st->buffered += chunk.size();
                        st->chunks.push_back(std::move(chunk));
                    }
                }
                if (!transportOk || st->failed || st->received != st->expected) {
                    if (!st->failed) {
                        ClipboardLog::warn("FileOffer: download failed (%s)",
                                           reply->errorString().toUtf8().constData());
                        // A connection that broke off; an answer that was wrong stays Other.
                        const auto error = reply->error();
                        const bool network =
                            (error >= QNetworkReply::ConnectionRefusedError &&
                             error <= QNetworkReply::UnknownNetworkError &&
                             error != QNetworkReply::SslHandshakeFailedError) ||
                            (error == QNetworkReply::NoError && st->received < st->expected);
                        st->failure = error == QNetworkReply::TimeoutError
                                          ? VirtualFiles::Failure::Timeout
                                      : network ? VirtualFiles::Failure::Network
                                                : VirtualFiles::Failure::Other;
                    }
                    st->failed = true;
                } else {
                    st->done = true;
                }
            }
            st->changed.notify_all();
            reply->deleteLater();
        });
    }
};

namespace {

class FailedReader : public VirtualFiles::Reader
{
public:
    std::int64_t read(void*, std::uint32_t) override { return -1; }
};

class HttpReader : public VirtualFiles::Reader
{
public:
    HttpReader(std::shared_ptr<FileOfferDownloader::Core> core, std::shared_ptr<StreamState> st)
        : m_Core(std::move(core)), m_State(std::move(st))
    {
    }

    ~HttpReader() override
    {
        // A paste target that stops early (cancel, error) must not leave the transfer running.
        const auto st = m_State;
        QMetaObject::invokeMethod(
            m_Core->context,
            [st]() {
                if (st->reply != nullptr) {
                    st->reply->abort();
                }
            },
            Qt::QueuedConnection);
    }

    std::int64_t read(void* buffer, std::uint32_t size) override
    {
        if (size == 0) {
            return 0;
        }
        std::unique_lock<std::mutex> lock(m_State->mutex);
        // The last byte of the file waits until the response has been checked
        // (status, length, certificate): a paste target that has every byte
        // reads no further, so a failure found after it would never reach it.
        const auto releasable = [this]() -> qint64 {
            const bool last =
                m_State->delivered + static_cast<quint64>(m_State->buffered) >= m_State->expected;
            return m_State->done || !last ? m_State->buffered
                                          : std::max<qint64>(0, m_State->buffered - 1);
        };
        const bool ready = m_State->changed.wait_for(lock, STALL_TIMEOUT, [&]() {
            return releasable() > 0 || m_State->done || m_State->failed;
        });
        if (!ready) {
            if (!m_State->failed) {
                m_State->failure = VirtualFiles::Failure::Timeout;
            }
            m_State->failed = true;
            lock.unlock();
            ClipboardLog::warn("FileOffer: download stalled");
            const auto st = m_State;
            QMetaObject::invokeMethod(
                m_Core->context, [st]() { FileOfferDownloader::Core::failStream(st); },
                Qt::QueuedConnection);
            return -1;
        }
        if (m_State->failed) {
            return -1;
        }
        const qint64 allowed = std::min<qint64>(releasable(), static_cast<qint64>(size));
        if (allowed == 0) {
            return 0; // done, nothing left
        }

        auto* out = static_cast<char*>(buffer);
        std::int64_t copied = 0;
        while (copied < allowed && !m_State->chunks.empty()) {
            const QByteArray& front = m_State->chunks.front();
            const qint64 available = front.size() - m_State->headOffset;
            const qint64 take = std::min<qint64>(available, allowed - copied);
            std::memcpy(out + copied, front.constData() + m_State->headOffset,
                        static_cast<size_t>(take));
            copied += take;
            m_State->headOffset += take;
            if (m_State->headOffset == front.size()) {
                m_State->chunks.pop_front();
                m_State->headOffset = 0;
            }
        }
        m_State->buffered -= copied;
        m_State->delivered += static_cast<quint64>(copied);
        const bool refill = m_State->buffered < LOW_WATER && !m_State->done && !m_State->failed;
        lock.unlock();
        if (refill) {
            const auto st = m_State;
            QMetaObject::invokeMethod(
                m_Core->context, [st]() { FileOfferDownloader::Core::pump(st); },
                Qt::QueuedConnection);
        }
        return copied;
    }

    VirtualFiles::Failure failure() const override
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        return m_State->failure;
    }

private:
    std::shared_ptr<FileOfferDownloader::Core> m_Core;
    std::shared_ptr<StreamState> m_State;
};

} // namespace

FileOfferDownloader::FileOfferDownloader(const ClipboardSyncHostContext& host,
                                         const FileOffer& offer)
    : m_Core(std::make_shared<Core>(host, offer))
{
}

FileOfferDownloader::~FileOfferDownloader() = default;

std::unique_ptr<VirtualFiles::Reader> FileOfferDownloader::open(std::size_t index,
                                                                std::uint64_t offset)
{
    if (index >= m_Core->entries.size() || m_Core->entries[index].dir ||
        offset > m_Core->entries[index].size) {
        return std::make_unique<FailedReader>();
    }
    auto st = std::make_shared<StreamState>();
    st->expected = m_Core->entries[index].size - offset;
    if (st->expected == 0) {
        // Nothing to fetch (an empty file, or a seek to its end).
        st->done = true;
        return std::make_unique<HttpReader>(m_Core, st);
    }
    Core* core = m_Core.get();
    QMetaObject::invokeMethod(
        core->context, [core, st, index, offset]() { core->start(st, index, offset); },
        Qt::QueuedConnection);
    return std::make_unique<HttpReader>(m_Core, st);
}

// ---- Files copied on this client ----

LocalFileOffers::LocalFileOffers()
{
    m_Clock.start();
}

void LocalFileOffers::expire()
{
    while (!m_Offers.empty() && m_Clock.elapsed() - m_Offers.front().createdMs > TTL_MS) {
        m_Offers.pop_front();
    }
}

void LocalFileOffers::clear()
{
    m_Offers.clear();
}

bool LocalFileOffers::add(const QStringList& paths, QByteArray& json, QString* error)
{
    expire();
    Offer offer;
    // The id shape Sunshine's offers use: 32 random bytes as 64 lower-case hex digits.
    quint32 random[8];
    QRandomGenerator::system()->fillRange(random);
    offer.id = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(random), sizeof(random)).toHex());
    offer.createdMs = m_Clock.elapsed();
    QJsonArray files;
    quint64 total = 0;
    QString failure;

    // Appends `info` as `relative` and, for a folder, everything below it.
    std::function<bool(const QFileInfo&, const QString&)> append =
        [&](const QFileInfo& info, const QString& relative) -> bool {
        if (static_cast<int>(offer.items.size()) >= FileOffer::MAX_ENTRIES) {
            failure = QStringLiteral("too many entries");
            return false;
        }
        const bool dir = info.isDir();
        Item item;
        item.localPath = info.absoluteFilePath();
        item.dir = dir;
        item.size = dir ? 0 : static_cast<quint64>(std::max<qint64>(info.size(), 0));
        item.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        if (dir && !canList(item.localPath)) {
            // Listed as empty, the host would paste a folder missing everything in it.
            failure = QStringLiteral("cannot read folder: ") + item.localPath;
            return false;
        }
        if (!dir) {
            // Opened once now: a file that cannot be read fails here, not at the paste.
            QFile file(item.localPath);
            if (!file.open(QIODevice::ReadOnly) || !identityOf(file, item.volume, item.fileId)) {
                failure = QStringLiteral("cannot read file: ") + item.localPath;
                return false;
            }
        }
        offer.items.push_back(item);
        total += item.size;
        files.append(QJsonObject{
            { QStringLiteral("path"), relative },
            { QStringLiteral("size"), static_cast<double>(item.size) },
            { QStringLiteral("dir"), dir },
            { QStringLiteral("mtime"),
              static_cast<double>(info.lastModified().toSecsSinceEpoch()) },
        });
        if (!dir) {
            return true;
        }
        const QFileInfoList children = QDir(info.absoluteFilePath())
                                           .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                              QDir::Hidden | QDir::System,
                                                          QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& child : children) {
            if (isLink(child)) {
                continue;
            }
            if (!append(child, relative + QLatin1Char('/') + child.fileName())) {
                return false;
            }
        }
        return true;
    };

    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (!info.exists()) {
            return fail(error, QStringLiteral("not found: ") + path);
        }
        if (isLink(info) || info.fileName().isEmpty()) {
            // A link, or a drive root such as C:\, which has no name to paste under.
            return fail(error, QStringLiteral("unsupported: ") + path);
        }
        if (!append(info, info.fileName())) {
            return fail(error, failure);
        }
    }
    if (offer.items.empty()) {
        return fail(error, QStringLiteral("no files"));
    }

    json = QJsonDocument(QJsonObject{
                             { QStringLiteral("version"), 2 },
                             { QStringLiteral("id"), offer.id },
                             { QStringLiteral("type"), QStringLiteral("files") },
                             { QStringLiteral("source"), QStringLiteral("clipboard") },
                             { QStringLiteral("total_size"), static_cast<double>(total) },
                             { QStringLiteral("expires_in"), static_cast<double>(TTL_MS / 1000) },
                             { QStringLiteral("files"), files },
                         })
               .toJson(QJsonDocument::Compact);

    // The host applies the same rules (names Windows rejects, MAX_PATH,
    // duplicates); refuse here what it would refuse there.
    FileOffer check;
    QString reason;
    if (!FileOffer::parse(json, check, &reason)) {
        return fail(error, reason);
    }

    m_Offers.push_back(std::move(offer));
    while (static_cast<int>(m_Offers.size()) > MAX_OFFERS) {
        m_Offers.pop_front();
    }
    return true;
}

bool LocalFileOffers::identityOf(QFile& file, quint64& volume, quint64& fileId)
{
    const int fd = file.handle();
    if (fd < 0) {
        return false;
    }
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    BY_HANDLE_FILE_INFORMATION info;
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info)) {
        return false;
    }
    volume = info.dwVolumeSerialNumber;
    fileId = (static_cast<quint64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
#else
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return false;
    }
    volume = static_cast<quint64>(st.st_dev);
    fileId = static_cast<quint64>(st.st_ino);
#endif
    return true;
}

bool LocalFileOffers::canList(const QString& folder)
{
#ifdef Q_OS_WIN
    // QDir lists a folder it may not open as empty; ask Windows directly.
    const std::wstring pattern =
        QDir::toNativeSeparators(folder + QStringLiteral("/*")).toStdWString();
    WIN32_FIND_DATAW data;
    const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                         FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    FindClose(find);
    return true;
#else
    return QDir(folder).isReadable();
#endif
}

bool LocalFileOffers::isLink(const QFileInfo& info)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return info.isSymbolicLink() || info.isJunction();
#else
    return info.isSymLink() && !info.isShortcut();
#endif
}

LocalFileOffers::Chunk LocalFileOffers::read(const QString& id, qint64 index, quint64 offset,
                                             quint64 length)
{
    expire();
    Chunk out;
    const auto offer = std::find_if(m_Offers.begin(), m_Offers.end(),
                                    [&id](const Offer& o) { return o.id == id; });
    if (offer == m_Offers.end() || index < 0 || index >= static_cast<qint64>(offer->items.size()) ||
        offer->items[static_cast<std::size_t>(index)].dir) {
        out.error = "not_found";
        return out;
    }
    const Item& item = offer->items[static_cast<std::size_t>(index)];
    QFile file(item.localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        out.error = "gone";
        return out;
    }
    quint64 volume = 0;
    quint64 fileId = 0;
    if (!identityOf(file, volume, fileId)) {
        out.error = "gone";
        return out;
    }
    if (volume != item.volume || fileId != item.fileId ||
        static_cast<quint64>(file.size()) != item.size ||
        QFileInfo(item.localPath).lastModified().toMSecsSinceEpoch() != item.mtimeMs) {
        out.error = "changed";
        return out;
    }
    if (offset > item.size) {
        out.error = "range";
        return out;
    }
    const quint64 want = std::min({ length, item.size - offset, MAX_CHUNK });
    if (want == 0) {
        return out;
    }
    if (!file.seek(static_cast<qint64>(offset))) {
        out.error = "gone";
        return out;
    }
    out.bytes = file.read(static_cast<qint64>(want));
    if (static_cast<quint64>(out.bytes.size()) != want) {
        out.bytes.clear();
        out.error = "gone";
    }
    return out;
}

bool FilePull::parse(const QByteArray& json, FilePull& out)
{
    out = FilePull();
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject obj = doc.object();
    static const QRegularExpression ticketPattern(QStringLiteral("^[0-9a-f]{32}$"));
    static const QRegularExpression idPattern(QStringLiteral("^[0-9a-f]{64}$"));
    out.ticket = obj.value(QStringLiteral("ticket")).toString();
    out.id = obj.value(QStringLiteral("id")).toString();
    if (!ticketPattern.match(out.ticket).hasMatch() || !idPattern.match(out.id).hasMatch()) {
        return false;
    }
    const auto integer = [&obj](const char* key, double max, double& value) {
        value = obj.value(QLatin1String(key)).toDouble(-1);
        return std::isfinite(value) && value >= 0 && value <= max && std::floor(value) == value;
    };
    double index = 0, offset = 0, length = 0;
    if (!integer("index", FileOffer::MAX_ENTRIES - 1, index) ||
        !integer("offset", MAX_JSON_INTEGER, offset) ||
        !integer("length", static_cast<double>(LocalFileOffers::MAX_CHUNK), length) ||
        length == 0) {
        return false;
    }
    out.index = static_cast<qint64>(index);
    out.offset = static_cast<quint64>(offset);
    out.length = static_cast<quint64>(length);
    return true;
}
