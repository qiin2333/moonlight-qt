// Files the host copied (clipboard kind 4): offer validation and the
// downloader that feeds a paste, against a loopback TLS server.

#include "streaming/filetransfer/fileoffer.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <atomic>
#include <functional>
#include <thread>

namespace {

bool check(bool value, const char* name, QTextStream& err)
{
    if (!value) {
        err << "FAIL: " << name << '\n';
    }
    return value;
}

QByteArray fixture(const char* name)
{
    QFile file(QStringLiteral(":/clipboard-test/") + QString::fromLatin1(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

const QString kId =
    QStringLiteral("0b5e3a8c1d2f4a6b8c9d0e1f2a3b4c5d0b5e3a8c1d2f4a6b8c9d0e1f2a3b4c5d");

QByteArray offerJson(const QJsonArray& files, const QString& id = kId)
{
    return QJsonDocument(QJsonObject{ { "version", 2 },
                                      { "id", id },
                                      { "type", "files" },
                                      { "source", "clipboard" },
                                      { "files", files } })
        .toJson(QJsonDocument::Compact);
}

QJsonObject file(const QString& path, double size, bool dir = false)
{
    return QJsonObject{
        { "path", path }, { "size", size }, { "dir", dir }, { "mtime", 1700000000 }
    };
}

bool parses(const QByteArray& json)
{
    FileOffer offer;
    return FileOffer::parse(json, offer);
}

// Serves GET /api/v1/file-transfer/<id>/<index> from `files`, honouring "Range: bytes=N-".
class FileServer : public QTcpServer
{
public:
    QSslCertificate certificate{ fixture("server.pem") };
    QSslKey key{ fixture("server-key.pem"), QSsl::Rsa };
    QList<QByteArray> files;
    QList<QByteArray> ranges; // Range header of each request, empty when none
    QList<QByteArray> paths;  // path of each request
    std::function<QByteArray(int index, const QByteArray& body, qint64 offset)> respond;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        socket->setSocketDescriptor(descriptor);
        socket->setLocalCertificate(certificate);
        socket->setPrivateKey(key);
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::readyRead, this, [this, socket, buffer]() {
            *buffer += socket->readAll();
            const int end = buffer->indexOf("\r\n\r\n");
            if (end < 0) {
                return;
            }
            const QByteArray head = buffer->left(end);
            buffer->remove(0, end + 4);
            const QList<QByteArray> lines = head.split('\n');
            const QByteArray path = lines.value(0).split(' ').value(1);
            QByteArray range;
            for (const QByteArray& line : lines) {
                if (line.toLower().startsWith("range:")) {
                    range = line.mid(line.indexOf(':') + 1).trimmed();
                }
            }
            ranges.append(range);
            paths.append(path);
            const int index = path.mid(path.lastIndexOf('/') + 1).toInt();
            const QByteArray body = files.value(index);
            qint64 offset = 0;
            if (range.startsWith("bytes=")) {
                offset = range.mid(6, range.size() - 7).toLongLong();
            }
            QByteArray response;
            if (respond) {
                response = respond(index, body, offset);
            } else if (offset > 0) {
                const QByteArray part = body.mid(offset);
                response = "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                           QByteArray::number(part.size()) + "\r\nContent-Range: bytes " +
                           QByteArray::number(offset) + "-" + QByteArray::number(body.size() - 1) +
                           "/" + QByteArray::number(body.size()) + "\r\n\r\n" + part;
            } else {
                response = "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                           "\r\n\r\n" + body;
            }
            socket->write(response);
            socket->flush();
        });
        socket->startServerEncryption();
    }
};

// Reads entry `index` from `offset` to the end on a plain thread (a paste
// target blocks like this), while this thread keeps the server running.
struct ReadResult
{
    QByteArray bytes;
    bool failed = false;
    VirtualFiles::Failure failure = VirtualFiles::Failure::Other;
};

ReadResult readAll(VirtualFiles::Source& source, std::size_t index, std::uint64_t offset = 0,
                   std::uint32_t chunk = 65536)
{
    ReadResult result;
    std::atomic_bool finished = false;
    std::thread reader([&]() {
        auto stream = source.open(index, offset);
        QByteArray buffer(static_cast<int>(chunk), Qt::Uninitialized);
        while (true) {
            const std::int64_t got = stream->read(buffer.data(), chunk);
            if (got < 0) {
                result.failed = true;
                result.failure = stream->failure();
                break;
            }
            if (got == 0) {
                break;
            }
            result.bytes.append(buffer.constData(), static_cast<int>(got));
        }
        finished = true;
    });
    // The loopback server runs on this thread: keep it serving until the reader
    // is done (a stalled download ends on the reader's own timeout).
    while (!finished) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    reader.join();
    return result;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray pullJson(const QJsonObject& overrides)
{
    QJsonObject obj{ { "ticket", "0123456789abcdef0123456789abcdef" },
                     { "id", kId },
                     { "index", 2 },
                     { "offset", 0 },
                     { "length", 4096 } };
    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        obj.insert(it.key(), it.value());
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

bool runLocalFileOffers(QTextStream& err)
{
    bool ok = true;
    QTemporaryDir tmp;
    if (!check(tmp.isValid(), "temporary folder", err)) {
        return false;
    }
    const QDir root(tmp.path());
    QByteArray big(LocalFileOffers::MAX_CHUNK + 1000, Qt::Uninitialized);
    for (int i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>((i * 17) & 0xFF);
    }
    root.mkpath(QStringLiteral("dir/sub"));
    ok &= check(writeFile(root.filePath("a.txt"), "hello") &&
                    writeFile(root.filePath("dir/b.bin"), big),
                "fixture files", err);
    // A link inside a copied folder is left out (only checked where one can be made).
    bool linked = false;
#ifndef Q_OS_WIN
    linked = QFile::link(root.filePath("a.txt"), root.filePath("dir/link.txt"));
    ok &= check(linked, "link fixture", err);
#endif

    LocalFileOffers local;
    QByteArray json;
    QString error;
    ok &= check(local.add({ root.filePath("a.txt"), root.filePath("dir") }, json, &error),
                "copied files are offered", err);
    FileOffer offer;
    ok &= check(FileOffer::parse(json, offer), "the offer passes the host's rules", err);
    ok &= check(offer.entries.size() == 4 && offer.entries[0].path == u"a.txt" &&
                    offer.entries[1].path == u"dir" && offer.entries[1].dir &&
                    offer.entries[2].path == u"dir\\b.bin" &&
                    offer.entries[2].size == static_cast<quint64>(big.size()) &&
                    offer.entries[3].path == u"dir\\sub" &&
                    offer.totalSize == static_cast<quint64>(big.size()) + 5,
                linked ? "folders listed before their contents, links left out"
                       : "folders listed before their contents",
                err);

    auto chunk = local.read(offer.id, 2, 0, LocalFileOffers::MAX_CHUNK);
    ok &= check(chunk.error.isEmpty() && chunk.bytes == big.left(LocalFileOffers::MAX_CHUNK),
                "a chunk is read from the start", err);
    chunk = local.read(offer.id, 2, LocalFileOffers::MAX_CHUNK, LocalFileOffers::MAX_CHUNK);
    ok &= check(chunk.error.isEmpty() && chunk.bytes == big.mid(LocalFileOffers::MAX_CHUNK),
                "the last chunk is shorter", err);
    chunk = local.read(offer.id, 0, 1, 2);
    ok &= check(chunk.error.isEmpty() && chunk.bytes == "el", "a chunk from an offset", err);
    chunk = local.read(offer.id, 0, 5, 10);
    ok &= check(chunk.error.isEmpty() && chunk.bytes.isEmpty(), "reading at the end gives nothing",
                err);
    ok &= check(local.read(offer.id, 0, 6, 1).error == "range", "past the end is refused", err);
    ok &= check(local.read(offer.id, 1, 0, 1).error == "not_found", "a folder has no bytes", err);
    ok &= check(local.read(offer.id, 9, 0, 1).error == "not_found", "no such entry", err);
    ok &= check(local.read(kId, 0, 0, 1).error == "not_found", "no such offer", err);

    {
        // Another file put in its place, with the same size and modification
        // time, between two chunks: the second chunk is refused.
        ok &= check(local.read(offer.id, 2, 0, 16).error.isEmpty(), "first chunk before the swap",
                    err);
        const QString target = root.filePath("dir/b.bin");
        const QDateTime mtime = QFileInfo(target).lastModified();
        const QString swap = root.filePath("dir/swap.tmp");
        writeFile(swap, QByteArray(big.size(), 'z'));
        QFile swapped(swap);
        swapped.open(QIODevice::ReadWrite);
        swapped.setFileTime(mtime, QFileDevice::FileModificationTime);
        swapped.close();
        QFile::remove(target);
        QFile::rename(swap, target);
        ok &= check(QFileInfo(target).size() == big.size() &&
                        QFileInfo(target).lastModified() == mtime,
                    "replacement fixture has the same size and time", err);
        ok &= check(local.read(offer.id, 2, 16, 16).error == "changed",
                    "a file replaced with the same size and time is refused", err);
    }
    {
        // Same size, another modification time: possibly another file.
        QFile same(root.filePath("a.txt"));
        same.open(QIODevice::ReadWrite);
        same.write("HELLO");
        same.setFileTime(QDateTime::currentDateTime().addSecs(-3600),
                         QFileDevice::FileModificationTime);
        same.close();
        ok &= check(local.read(offer.id, 0, 0, 5).error == "changed",
                    "a file rewritten with the same size", err);
    }
    writeFile(root.filePath("a.txt"), "changed!");
    ok &= check(local.read(offer.id, 0, 0, 5).error == "changed", "a file that changed size", err);
    QFile::remove(root.filePath("a.txt"));
    ok &= check(local.read(offer.id, 0, 0, 5).error == "gone", "a file that went away", err);

    root.mkpath(QStringLiteral("other"));
    writeFile(root.filePath("other/A.TXT"), "x");
    writeFile(root.filePath("dir/a.txt"), "y");
    ok &= check(!local.add({ root.filePath("other/A.TXT"), root.filePath("dir/a.txt") }, json),
                "two copied files with the same name are refused", err);
    ok &= check(!local.add({ root.filePath("missing") }, json), "a missing path is refused", err);

    for (int i = 0; i < LocalFileOffers::MAX_OFFERS; ++i) {
        local.add({ root.filePath("dir/a.txt") }, json);
    }
    ok &= check(local.read(offer.id, 2, 0, 1).error == "not_found",
                "the oldest offer is forgotten past the limit", err);
    local.clear();

    FilePull pull;
    ok &= check(FilePull::parse(pullJson({}), pull) && pull.index == 2 && pull.length == 4096 &&
                    pull.ticket.size() == 32,
                "a chunk request parses", err);
    ok &= check(!FilePull::parse(pullJson({ { "ticket", "../x" } }), pull), "bad ticket", err);
    ok &= check(!FilePull::parse(pullJson({ { "id", "x" } }), pull), "bad offer id", err);
    ok &= check(!FilePull::parse(pullJson({ { "length", 0 } }), pull), "empty chunk", err);
    ok &= check(
        !FilePull::parse(pullJson({ { "length", double(LocalFileOffers::MAX_CHUNK + 1) } }), pull),
        "chunk over the limit", err);
    ok &= check(!FilePull::parse(pullJson({ { "offset", -1 } }), pull), "negative offset", err);
    ok &= check(!FilePull::parse(pullJson({ { "index", 1.5 } }), pull), "fractional index", err);
    return ok;
}

} // namespace

bool runFileOfferRegression(QTextStream& err)
{
    bool ok = true;

    // ---- Validation ----
    {
        FileOffer offer;
        const bool parsed =
            FileOffer::parse(offerJson({ file("a.txt", 5), file("dir", 0, true),
                                         file("dir/sub", 0, true), file("dir/sub/b.bin", 1000) }),
                             offer);
        ok &= check(parsed && offer.entries.size() == 4 && offer.totalSize == 1005 &&
                        offer.entries[3].path == u"dir\\sub\\b.bin" && offer.entries[1].dir &&
                        offer.entries[0].mtime == 1700000000 &&
                        offer.source == QStringLiteral("clipboard") && !offer.singleFileRoute,
                    "valid offer parses with Windows separators", err);
    }
    ok &= check(!parses(offerJson({ file("../a.txt", 1) })), "traversal rejected", err);
    ok &= check(!parses(offerJson({ file("dir", 0, true), file("dir/../../a", 1) })),
                "nested traversal rejected", err);
    ok &= check(!parses(offerJson({ file("a\\b.txt", 1) })), "backslash in a name rejected", err);
    ok &= check(!parses(offerJson({ file("C:/a.txt", 1) })), "drive letter rejected", err);
    ok &= check(!parses(offerJson({ file("/a.txt", 1) })), "absolute path rejected", err);
    ok &= check(!parses(offerJson({ file("CON.txt", 1) })), "reserved device name rejected", err);
    ok &=
        check(!parses(offerJson({ file("lpt1", 1) })), "reserved name in lower case rejected", err);
    ok &= check(!parses(offerJson({ file("a.", 1) })), "trailing dot rejected", err);
    ok &= check(!parses(offerJson({ file(QStringLiteral("COM\u00B9.txt"), 1) })),
                "superscript device name rejected", err);
    ok &= check(!parses(offerJson({ file(QStringLiteral("lpt\u00B3"), 1) })),
                "superscript device name in lower case rejected", err);
    ok &=
        check(!parses(offerJson({ file("dir/a.txt", 1) })), "file before its folder rejected", err);
    ok &= check(!parses(offerJson({ file("A.txt", 1), file("a.TXT", 1) })),
                "case-insensitive duplicate rejected", err);
    ok &= check(!parses(offerJson({ file("a.txt", -1) })), "negative size rejected", err);
    ok &= check(!parses(offerJson({ file("a.txt", 1.5) })), "fractional size rejected", err);
    ok &= check(!parses(offerJson({ file("d", 3, true) })), "folder with a size rejected", err);
    ok &= check(!parses(offerJson({ file(QString(260, 'x'), 1) })), "path over MAX_PATH rejected",
                err);
    ok &= check(parses(offerJson({ file(QString(259, 'x'), 1) })), "path at MAX_PATH - 1 accepted",
                err);
    ok &= check(!parses(offerJson({ file("a.txt", 1) }, QStringLiteral("../../x"))),
                "bad id rejected", err);
    ok &= check(!parses(offerJson({})), "empty offer rejected", err);
    ok &= check(!parses(offerJson({ file("a", 1) },
                                  QStringLiteral("0b5e3a8c-1d2f-4a6b-8c9d-0e1f2a3b4c5d"))),
                "a UUID is not an offer id", err);
    {
        // The first, single-file version: one file at /api/v1/file-transfer/<id>.
        FileOffer offer;
        const QByteArray single =
            QJsonDocument(QJsonObject{ { "id", kId },
                                       { "type", "file" },
                                       { "name", "report.pdf" },
                                       { "size", 42 },
                                       { "mime", "application/octet-stream" },
                                       { "download_url", "/api/v1/file-transfer/" + kId },
                                       { "expires_in", 600 } })
                .toJson(QJsonDocument::Compact);
        ok &= check(FileOffer::parse(single, offer) && offer.singleFileRoute &&
                        offer.entries.size() == 1 && offer.entries[0].path == u"report.pdf" &&
                        offer.entries[0].size == 42 && offer.source == QStringLiteral("send"),
                    "a single-file offer of the first version parses", err);
        QJsonObject other = QJsonDocument::fromJson(single).object();
        other.insert("download_url", "/elsewhere");
        ok &= check(!FileOffer::parse(QJsonDocument(other).toJson(), offer),
                    "a single-file offer pointing elsewhere is rejected", err);
        other = QJsonDocument::fromJson(single).object();
        other.insert("name", "../x");
        ok &= check(!FileOffer::parse(QJsonDocument(other).toJson(), offer),
                    "a single-file offer with a bad name is rejected", err);
        other = QJsonDocument::fromJson(single).object();
        other.insert("type", "folder");
        ok &= check(!FileOffer::parse(QJsonDocument(other).toJson(), offer),
                    "unknown type rejected", err);
    }
    ok &= check(!parses("[1]"), "non-object rejected", err);

    // ---- Files copied on this client ----
    ok &= runLocalFileOffers(err);

    // ---- Downloads ----
    FileServer server;
    if (!check(!server.certificate.isNull() && !server.key.isNull() &&
                   server.listen(QHostAddress::LocalHost),
               "file server listening", err)) {
        return false;
    }
    QByteArray big(3 * 1024 * 1024 + 123, Qt::Uninitialized);
    for (int i = 0; i < big.size(); ++i) {
        big[i] = static_cast<char>((i * 131) & 0xFF);
    }
    server.files = { big, QByteArray("hello"), QByteArray() };

    ClipboardSyncHostContext host;
    host.address = QStringLiteral("127.0.0.1");
    host.httpsPort = server.serverPort();
    host.serverCertificate = server.certificate;

    FileOffer offer;
    FileOffer::parse(
        offerJson({ file("big.bin", big.size()), file("small.txt", 5), file("empty", 0) }), offer);
    {
        FileOfferDownloader source(host, offer);
        const ReadResult whole = readAll(source, 0);
        ok &= check(!whole.failed && whole.bytes == big, "whole file arrives intact", err);
        ok &= check(server.ranges.size() == 1 && server.ranges.value(0).isEmpty(),
                    "first read sends one request, without Range", err);

        const int requests = server.ranges.size();
        const ReadResult tail = readAll(source, 0, 1000000);
        ok &= check(!tail.failed && tail.bytes == big.mid(1000000), "resumed read gets the rest",
                    err);
        ok &=
            check(server.ranges.size() == requests + 1 && server.ranges.last() == "bytes=1000000-",
                  "resumed read asks for bytes=N-", err);

        const ReadResult tiny = readAll(source, 1, 0, 1);
        ok &= check(!tiny.failed && tiny.bytes == "hello", "one-byte reads", err);

        const int before = server.ranges.size();
        const ReadResult empty = readAll(source, 2);
        ok &= check(!empty.failed && empty.bytes.isEmpty() && server.ranges.size() == before,
                    "empty file needs no request", err);
    }
    {
        server.respond = [](int, const QByteArray&, qint64) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nhel");
        };
        FileOfferDownloader source(host, offer);
        const ReadResult shortBody = readAll(source, 1);
        ok &= check(shortBody.failed && shortBody.failure == VirtualFiles::Failure::Network,
                    "a body shorter than the offer fails the read as a broken transfer", err);

        // Sunshine's answer names the cause the paste target shows.
        const std::pair<const char*, VirtualFiles::Failure> answers[] = {
            { "409 Conflict", VirtualFiles::Failure::Changed },
            { "410 Gone", VirtualFiles::Failure::Gone },
            { "404 Not Found", VirtualFiles::Failure::Gone },
            { "503 Service Unavailable", VirtualFiles::Failure::Other },
        };
        for (const auto& answer : answers) {
            const QByteArray status(answer.first);
            server.respond = [status](int, const QByteArray&, qint64) {
                return "HTTP/1.1 " + status + "\r\nContent-Length: 0\r\n\r\n";
            };
            const ReadResult failed = readAll(source, 1);
            ok &= check(failed.failed && failed.failure == answer.second,
                        "an HTTP error fails the read with its cause", err);
        }

        server.respond = [](int, const QByteArray& body, qint64) {
            // Ignores the Range and sends the whole file with 200.
            return "HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                   "\r\n\r\n" + body;
        };
        ok &= check(readAll(source, 0, 10).failed, "a resumed read answered with 200 fails", err);

        server.respond = [](int, const QByteArray& body, qint64 offset) {
            // A 206 of the right length, but the wrong range.
            const QByteArray part = body.left(body.size() - offset);
            return "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                   QByteArray::number(part.size()) + "\r\nContent-Range: bytes 0-" +
                   QByteArray::number(part.size() - 1) + "/" + QByteArray::number(body.size()) +
                   "\r\n\r\n" + part;
        };
        ok &= check(readAll(source, 0, 10).failed, "a resumed read of another range fails", err);

        server.respond = [](int, const QByteArray&, qint64) {
            // The five bytes, then more than the Content-Length said.
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA");
        };
        const ReadResult extra = readAll(source, 1, 0, 1);
        ok &= check(extra.bytes.size() <= 5, "no more bytes than the file has", err);
        server.respond = nullptr;
    }
    {
        auto wrongHost = host;
        wrongHost.serverCertificate = QSslCertificate(fixture("other.pem"));
        FileOfferDownloader source(wrongHost, offer);
        const int before = server.ranges.size();
        ok &= check(readAll(source, 1).failed && server.ranges.size() == before,
                    "an unpaired server certificate gets no request", err);
    }
    {
        FileOfferDownloader source(host, offer);
        ok &= check(readAll(source, 7).failed, "an index outside the offer fails", err);
    }
    {
        FileOffer single;
        FileOffer::parse(
            QJsonDocument(QJsonObject{ { "id", kId },
                                       { "type", "file" },
                                       { "name", "big.bin" },
                                       { "size", static_cast<double>(big.size()) },
                                       { "download_url", "/api/v1/file-transfer/" + kId } })
                .toJson(),
            single);
        FileOfferDownloader source(host, single);
        const ReadResult whole = readAll(source, 0);
        ok &= check(!whole.failed && whole.bytes == big &&
                        server.paths.last() == "/api/v1/file-transfer/" + kId.toLatin1(),
                    "a single-file offer downloads from its own route", err);
    }
    return ok;
}
