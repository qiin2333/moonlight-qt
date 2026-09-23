#include "streaming/clipboardsync.h"
#include "streaming/clipboardipc.h"

#include <QBuffer>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QtEndian>

namespace {
bool check(bool value, const char* name, QTextStream& err)
{
    if (!value)
        err << "FAIL: " << name << '\n';
    return value;
}
QByteArray fixture(const char* name)
{
    QFile file(QStringLiteral(":/clipboard-test/") + QString::fromLatin1(name));
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}
QByteArray wire(int kind, quint32 token, const QByteArray& payload)
{
    QByteArray result;
    result.append(char(1));
    result.append(char(kind));
    const quint32 t = qToLittleEndian(token), size = qToLittleEndian(quint32(payload.size()));
    result.append(reinterpret_cast<const char*>(&t), 4);
    result.append(reinterpret_cast<const char*>(&size), 4);
    return result + payload;
}
void incoming(ClipboardSync& sync, int kind, quint32 token, const QByteArray& payload)
{
    QMetaObject::invokeMethod(&sync, "onIncomingFrame", Qt::DirectConnection,
                              Q_ARG(QByteArray, wire(kind, token, payload)));
}
QByteArray ref(const char* id, int size)
{
    return QJsonDocument(QJsonObject{ { "id", QString::fromLatin1(id) },
                                      { "mime", "text/plain" },
                                      { "size", size } })
        .toJson(QJsonDocument::Compact);
}
bool waitFor(const std::function<bool()>& condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

// Only loopback test traffic. The checked-in key is a public test fixture.
class BlobServer : public QTcpServer
{
public:
    QSslCertificate certificate{ fixture("server.pem") };
    QSslKey key{ fixture("server-key.pem"), QSsl::Rsa };
    QList<QPointer<QSslSocket>> requests;
    void answer(int index, const QByteArray& body, int declaredSize = -1)
    {
        if (index >= requests.size() || requests[index].isNull() ||
            requests[index]->state() != QAbstractSocket::ConnectedState)
            return;
        auto socket = requests[index];
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
                      QByteArray::number(declaredSize < 0 ? body.size() : declaredSize) +
                      "\r\n\r\n" + body);
        socket->flush();
    }
    void answerChunked(int index, const QByteArray& body)
    {
        if (index >= requests.size() || requests[index].isNull())
            return;
        requests[index]->write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" +
                               QByteArray::number(body.size(), 16) + "\r\n" + body +
                               "\r\n0\r\n\r\n");
        requests[index]->flush();
    }

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
            if (end < 0)
                return;
            int contentLength = 0;
            for (auto line : buffer->left(end).split('\n')) {
                if (line.toLower().startsWith("content-length:")) {
                    contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                }
            }
            if (buffer->size() < end + 4 + contentLength)
                return;
            buffer->remove(0, end + 4 + contentLength);
            requests.append(socket);
        });
        socket->startServerEncryption();
    }
};
}

class ClipboardSyncTest
{
public:
    static bool run(QTextStream& err)
    {
        bool ok = true;
        auto* cb = QGuiApplication::clipboard();
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
#ifndef Q_OS_MACOS
        {
            cb->setText("baseline");
            ClipboardSync sync;
            sync.start();
            incoming(sync, 1, 42, "remote-text");
            ok &= check(cb->text() == "remote-text", "nonzero token text applies immediately", err);
            incoming(sync, 2, 42, png);
            ok &= check(cb->text() == "remote-text" && cb->mimeData()->hasImage(),
                        "compound inbound retains both flavors", err);
            incoming(sync, 1, 0, "newer");
            incoming(sync, 2, 42, png);
            ok &= check(cb->text() == "newer" && !cb->mimeData()->hasImage(),
                        "retired burst cannot overwrite newer clipboard", err);
            incoming(sync, 2, 43, png);
            incoming(sync, 1, 43, "late-text");
            ok &= check(cb->text() == "late-text" && cb->mimeData()->hasImage(),
                        "reverse flavor order aggregates", err);
        }
        {
            cb->clear();
            ClipboardSync sync;
            QList<QByteArray> sent;
            QObject::connect(&sync, &ClipboardSync::outboundFrame,
                             [&](QByteArray b) { sent.append(b); });
            sync.start();
            cb->setText("A");
            cb->setText("B");
            cb->setText("A");
            ok &= check(sent.size() == 3, "A B A must send three updates", err);
            QMetaObject::invokeMethod(&sync, "onLocalClipboardChanged", Qt::DirectConnection);
            ok &= check(sent.size() == 3, "duplicate notification suppressed", err);
            incoming(sync, 1, 0, "host");
            QMetaObject::invokeMethod(&sync, "onLocalClipboardChanged", Qt::DirectConnection);
            ok &= check(sent.size() == 3, "inbound clipboard does not echo", err);
        }
        {
            cb->clear();
            ClipboardSync sync;
            QList<QByteArray> sent;
            QObject::connect(&sync, &ClipboardSync::outboundFrame,
                             [&](QByteArray b) { sent.append(b); });
            sync.start();
            for (const auto& text :
                 { QByteArray("caption1"), QByteArray("caption2"), QByteArray(60000, 'x') }) {
                auto* mime = new QMimeData;
                mime->setText(QString::fromUtf8(text));
                mime->setData("image/png", png);
                cb->setMimeData(mime);
            }
            ok &=
                check(sent.size() == 6 && sent[4][1] == 1 && sent[5][1] == 2,
                      "same image new caption and 60000 byte text preserve full inline burst", err);
            if (sent.size() == 6) {
                const auto token = qFromLittleEndian<quint32>(
                    reinterpret_cast<const uchar*>(sent[4].constData() + 2));
                incoming(sync, 1, token, QByteArray(60000, 'x'));
                ok &= check(cb->mimeData()->data("image/png") == png,
                            "echoed compound text must not replace local image", err);
            }
        }
        {
            QTemporaryDir dir;
            const QString path = dir.filePath("private.png");
            image.save(path);
            cb->clear();
            ClipboardSync sync;
            QList<QByteArray> sent;
            QObject::connect(&sync, &ClipboardSync::outboundFrame,
                             [&](QByteArray b) { sent.append(b); });
            sync.start();
            auto* file = new QMimeData;
            file->setUrls({ QUrl::fromLocalFile(path) });
            cb->setMimeData(file);
            ok &= check(sent.isEmpty(), "file URL cannot upload image file", err);
            auto* html = new QMimeData;
            html->setHtml("<img src='" + QUrl::fromLocalFile(path).toString() + "'>");
            cb->setMimeData(html);
            ok &= check(sent.isEmpty(), "HTML cannot read local image file", err);
            auto* chat = new QMimeData;
            chat->setUrls({ QUrl::fromLocalFile(path) });
            chat->setData("image/png", png);
            chat->setText(path);
            cb->setMimeData(chat);
            ok &= check(sent.size() == 1 && sent[0][1] == 2,
                        "attached chat bitmap syncs without leaking path", err);
            auto* shell = new QMimeData;
            shell->setData("Shell IDList Array", "file");
            shell->setData("image/png", png);
            cb->setMimeData(shell);
            ok &= check(sent.size() == 1, "shell icon cannot bypass file protection", err);
        }
        {
            // A BMP header with excessive dimensions must fail before pixel allocation.
            QByteArray bmp(54, '\0');
            bmp[0] = 'B';
            bmp[1] = 'M';
            qToLittleEndian<quint32>(54, reinterpret_cast<uchar*>(bmp.data() + 2));
            qToLittleEndian<quint32>(54, reinterpret_cast<uchar*>(bmp.data() + 10));
            qToLittleEndian<quint32>(40, reinterpret_cast<uchar*>(bmp.data() + 14));
            qToLittleEndian<quint32>(100000, reinterpret_cast<uchar*>(bmp.data() + 18));
            qToLittleEndian<quint32>(100000, reinterpret_cast<uchar*>(bmp.data() + 22));
            bmp[26] = 1;
            bmp[28] = 24;
            QImage decoded;
            ok &= check(!ClipboardSync::decodeImage(bmp, decoded),
                        "oversized image header rejected", err);
        }
#endif
        {
            ClipboardSync sync;
            QByteArray frame;
            ok &= check(
                sync.encodeFrame(1, 1, QByteArray(ClipboardSync::MAX_INLINE_PAYLOAD, 'x'), frame) &&
                    frame.size() + 24 <= 65535,
                "inline frame fits encrypted control length", err);
            ok &= check(!sync.encodeFrame(
                            1, 1, QByteArray(ClipboardSync::MAX_INLINE_PAYLOAD + 1, 'x'), frame),
                        "inline frame cannot overflow encrypted control length", err);
        }
        {
            QByteArray aggregate;
            const QByteArray line = ClipboardIpc::encodeLocalFrame(1, QByteArray(60000, 'x'));
            for (int i = 0; i < 20; ++i)
                aggregate += line + '\n';
            QString error;
            QByteArray next;
            int lines = 0;
            while (ClipboardIpc::takeLine(aggregate, next, error))
                ++lines;
            ok &= check(lines == 20 && error.isEmpty(),
                        "multiple legal IPC lines may exceed one MiB in aggregate", err);
            aggregate.fill('x', ClipboardIpc::MAX_LINE_BYTES + 1);
            ok &= check(!ClipboardIpc::takeLine(aggregate, next, error) && !error.isEmpty(),
                        "oversized partial IPC line rejected", err);
            ClipboardIpc::Message message;
            ok &=
                check(ClipboardIpc::decodeLine(ClipboardIpc::encodePong(99), message, error) &&
                          message.type == ClipboardIpc::MessageType::Pong && message.sequence == 99,
                      "heartbeat protocol roundtrip", err);
        }
        BlobServer server;
        if (!check(QSslSocket::supportsSsl() && !server.certificate.isNull() &&
                       !server.key.isNull() && server.listen(QHostAddress::LocalHost),
                   "loopback TLS fixture starts", err))
            return false;
        ClipboardSyncHostContext host;
        host.address = "127.0.0.1";
        host.httpsPort = server.serverPort();
        host.serverCertificate = server.certificate;
        {
            cb->setText("network baseline");
            ClipboardSync sync(host);
            sync.start();
            incoming(sync, 3, 0, ref("first", 6));
            ok &= check(waitFor([&] { return server.requests.size() == 1; }),
                        "paired TLS GET reaches server", err);
            server.answer(0, "remote");
            ok &= check(waitFor([&] { return cb->text() == "remote"; }), "valid blob applied", err);
            incoming(sync, 3, 0, ref("stale", 3));
            ok &= check(waitFor([&] { return server.requests.size() == 2; }),
                        "pending stale GET started", err);
            incoming(sync, 1, 0, "newer text");
            server.answer(1, "old");
            QCoreApplication::processEvents();
            ok &= check(sync.m_BlobReplies.isEmpty() && cb->text() == "newer text",
                        "new frame cancels stale download", err);
            incoming(sync, 3, 0, ref("oversize", 4));
            ok &= check(waitFor([&] { return server.requests.size() == 3; }), "limit GET started",
                        err);
            server.answer(2, "", 100000);
            ok &= check(waitFor([&] { return sync.m_BlobReplies.isEmpty(); }) &&
                            cb->text() == "newer text",
                        "oversize response aborted at headers", err);
            incoming(sync, 3, 0, ref("stop", 3));
            ok &= check(waitFor([&] { return server.requests.size() == 4; }), "stop GET started",
                        err);
            sync.stop();
            server.answer(3, "old");
            QCoreApplication::processEvents();
            ok &= check(sync.m_BlobReplies.isEmpty() && cb->text() == "newer text",
                        "stop cancels pending network callbacks", err);
        }
        {
            cb->clear();
            ClipboardSync sync(host);
            QList<QByteArray> sent;
            QObject::connect(&sync, &ClipboardSync::outboundFrame,
                             [&](QByteArray b) { sent.append(b); });
            sync.start();
            const int index = server.requests.size();
            cb->setText(QString(60000, 'x'));
            ok &= check(waitFor([&] { return server.requests.size() > index; }),
                        "large clipboard POST started", err);
            cb->setText("new upload");
            server.answer(index, "{\"id\":\"late\"}");
            QCoreApplication::processEvents();
            ok &= check(sent.size() == 1 && sent[0][1] == 1 && sync.m_BlobReplies.isEmpty(),
                        "new clipboard cancels stale upload REF", err);
        }
        {
            cb->setText("keep");
            ClipboardSync sync(host);
            sync.start();
            int index = server.requests.size();
            incoming(sync, 3, 0, ref("chunked", 4));
            ok &= check(waitFor([&] { return server.requests.size() > index; }),
                        "chunked GET started", err);
            incoming(sync, 3, 0, "invalid JSON");
            ok &= check(!sync.m_BlobReplies.isEmpty(),
                        "malformed REF does not cancel valid transfer", err);
            server.answerChunked(index, "12345678");
            ok &=
                check(waitFor([&] { return sync.m_BlobReplies.isEmpty(); }) && cb->text() == "keep",
                      "streamed body is bounded without Content-Length", err);
            index = server.requests.size();
            incoming(sync, 3, 0, ref("reconfigure", 3));
            ok &= check(waitFor([&] { return server.requests.size() > index; }),
                        "reconfigure GET started", err);
            sync.setHostContext(host);
            server.answer(index, "old");
            QCoreApplication::processEvents();
            ok &= check(sync.m_BlobReplies.isEmpty() && cb->text() == "keep",
                        "reconfigure same host cancels prior generation", err);
        }
        {
            const QSslConfiguration previous = QSslConfiguration::defaultConfiguration();
            auto trusted = previous;
            trusted.setCaCertificates({ server.certificate });
            QSslConfiguration::setDefaultConfiguration(trusted);
            auto wrongHost = host;
            wrongHost.serverCertificate = QSslCertificate(fixture("other.pem"));
            ClipboardSync sync(wrongHost);
            sync.start();
            int sslErrors = 0, encrypted = 0;
            QObject::connect(sync.nam(), &QNetworkAccessManager::sslErrors, [&] { ++sslErrors; });
            QObject::connect(sync.nam(), &QNetworkAccessManager::encrypted, [&] { ++encrypted; });
            const int count = server.requests.size();
            cb->setText(QString(60000, 'z'));
            ok &= check(waitFor([&] { return sync.m_BlobReplies.isEmpty(); }),
                        "wrong paired certificate rejected", err);
            ok &= check(sslErrors == 0 && encrypted == 1 && server.requests.size() == count,
                        "trusted but unpaired certificate receives no HTTP clipboard body", err);
            QSslConfiguration::setDefaultConfiguration(previous);
        }
        return ok;
    }
};

bool runClipboardRegression(QTextStream& err)
{
    return ClipboardSyncTest::run(err);
}
