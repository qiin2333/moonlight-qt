#include "streaming/filemappingwebsocket.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QThread>

#include <future>
#include <thread>

namespace {
QByteArray serverFrame(bool fin, quint8 opcode, const QByteArray& payload)
{
    QByteArray frame;
    frame.append(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
    if (payload.size() < 126) {
        frame.append(static_cast<char>(payload.size()));
    }
    else if (payload.size() <= 0xffff) {
        frame.append(static_cast<char>(126));
        frame.append(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.append(static_cast<char>(payload.size() & 0xff));
    }
    else {
        frame.append(static_cast<char>(127));
        quint64 size = static_cast<quint64>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.append(static_cast<char>((size >> shift) & 0xff));
        }
    }
    frame.append(payload);
    return frame;
}

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

bool readMessage(QByteArray& buffer, QByteArray& out, QString& error, QList<QByteArray>* pongPayloads = nullptr)
{
    FileMappingWebSocket::TextMessageReader reader;
    bool needMore = false;
    error = reader.read(buffer, out, needMore, pongPayloads);
    return error.isEmpty() && !needMore;
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    bool ok = true;
    QString error;
    QByteArray payload;

    QByteArray single = serverFrame(true, 0x1, R"({"type":"hello","ok":true})");
    ok &= require(readMessage(single, payload, error), QStringLiteral("single frame read failed: %1").arg(error), err);
    ok &= require(payload == R"({"type":"hello","ok":true})", QStringLiteral("single frame payload mismatch"), err);
    ok &= require(single.isEmpty(), QStringLiteral("single frame buffer was not consumed"), err);

    QByteArray fragmented;
    fragmented += serverFrame(false, 0x1, R"({"type":)");
    fragmented += serverFrame(true, 0x0, R"("result","ok":true})");
    ok &= require(readMessage(fragmented, payload, error), QStringLiteral("fragmented read failed: %1").arg(error), err);
    ok &= require(payload == R"({"type":"result","ok":true})", QStringLiteral("fragmented payload mismatch"), err);

    QByteArray withPing;
    withPing += serverFrame(false, 0x1, R"({"type":)");
    withPing += serverFrame(true, 0x9, QByteArrayLiteral("ping"));
    withPing += serverFrame(true, 0x0, R"("result","id":1})");
    QList<QByteArray> pongPayloads;
    ok &= require(readMessage(withPing, payload, error, &pongPayloads), QStringLiteral("ping interleaved read failed: %1").arg(error), err);
    ok &= require(payload == R"({"type":"result","id":1})", QStringLiteral("ping interleaved payload mismatch"), err);
    ok &= require(pongPayloads == QList<QByteArray> { QByteArrayLiteral("ping") },
                  QStringLiteral("ping payload was not surfaced for pong"),
                  err);

    QByteArray withPong;
    withPong += serverFrame(true, 0xa, QByteArrayLiteral("pong"));
    withPong += serverFrame(true, 0x1, R"({"type":"result","id":2})");
    QList<QByteArray> pongControlPayloads;
    ok &= require(readMessage(withPong, payload, error, &pongControlPayloads), QStringLiteral("pong control read failed: %1").arg(error), err);
    ok &= require(payload == R"({"type":"result","id":2})", QStringLiteral("pong control payload mismatch"), err);
    ok &= require(pongControlPayloads.isEmpty(), QStringLiteral("pong control frame requested a reply"), err);

    FileMappingWebSocket::TextMessageReader incrementalReader;
    QByteArray partial = serverFrame(false, 0x1, R"({"type":)");
    bool needMore = false;
    payload.clear();
    error = incrementalReader.read(partial, payload, needMore);
    ok &= require(error.isEmpty() && needMore, QStringLiteral("partial read did not request more data: %1").arg(error), err);
    ok &= require(partial.isEmpty(), QStringLiteral("complete first fragment was not consumed"), err);
    partial += serverFrame(true, 0x0, R"("result"})");
    error = incrementalReader.read(partial, payload, needMore);
    ok &= require(error.isEmpty() && !needMore, QStringLiteral("incremental read failed: %1").arg(error), err);
    ok &= require(payload == R"({"type":"result"})", QStringLiteral("incremental payload mismatch"), err);

    for (int length : { 126, 65536 }) {
        const QByteArray expected(length, 'x');
        QByteArray large = serverFrame(true, 0x1, expected);
        ok &= require(readMessage(large, payload, error) && payload == expected,
                      QStringLiteral("extended frame length %1").arg(length), err);
    }

    const QList<QByteArray> invalidFrames {
        QByteArray::fromHex("8180"), // Servers must not mask their frames.
        QByteArray::fromHex("c100"), // No extensions were negotiated.
        QByteArray::fromHex("837f"), // Reserved opcode.
        QByteArray::fromHex("817e0001"), // Non-minimal 16-bit length.
        QByteArray::fromHex("817f000000000000007e"), // Non-minimal 64-bit length.
        QByteArray::fromHex("817f8000000000000000"), // Reserved high length bit.
        QByteArray::fromHex("817f0000000001000001"), // Above the frame size limit.
        serverFrame(false, 0x8, {}),
        serverFrame(true, 0x8, "x"),
        serverFrame(true, 0x9, QByteArray(126, 'x')),
        serverFrame(true, 0x0, "orphan"),
        serverFrame(false, 0x1, "first") + serverFrame(true, 0x1, "second"),
        serverFrame(true, 0x1, QByteArray::fromHex("c0af")) // Invalid UTF-8.
    };
    for (QByteArray invalid : invalidFrames) {
        readMessage(invalid, payload, error);
        ok &= require(!error.isEmpty(), QStringLiteral("invalid frame must fail without waiting for its payload"), err);
    }

    // Continuous partial input must not restart the caller's total timeout.
    std::promise<quint16> listeningPort;
    auto port = listeningPort.get_future();
    std::thread dripServer([&] {
        QTcpServer server;
        const bool listening = server.listen(QHostAddress::LocalHost);
        listeningPort.set_value(listening ? server.serverPort() : 0);
        if (!listening || !server.waitForNewConnection(2000)) {
            return;
        }
        QTcpSocket* peer = server.nextPendingConnection();
        const QByteArray frame = serverFrame(true, 0x1, QByteArray(100, 'x'));
        for (char byte : frame) {
            if (peer->write(QByteArray(1, byte)) != 1 || !peer->waitForBytesWritten(500)) {
                break;
            }
            QThread::msleep(20);
        }
        delete peer;
    });
    QSslSocket socket;
    const quint16 serverPort = port.get();
    socket.connectToHost(QHostAddress::LocalHost, serverPort);
    const bool connected = serverPort != 0 && socket.waitForConnected(2000);
    ok &= require(connected, QStringLiteral("local drip server connects"), err);
    if (connected) {
        QElapsedTimer elapsed;
        elapsed.start();
        QByteArray buffer;
        QJsonObject reply;
        error = FileMappingWebSocket::readJsonText(socket, buffer, reply, QDeadlineTimer(150));
        ok &= require(!error.isEmpty() && elapsed.elapsed() < 1000,
                      QStringLiteral("partial frames must respect the total deadline"), err);
    }
    socket.abort();
    dripServer.join();

    if (!ok) {
        return 1;
    }

    out << "file_mapping_websocket_framing=passed\n";
    return 0;
}
