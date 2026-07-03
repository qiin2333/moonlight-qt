#include "streaming/filemappingwebsocket.h"

#include <QCoreApplication>
#include <QList>
#include <QTextStream>

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

    if (!ok) {
        return 1;
    }

    out << "file_mapping_websocket_framing=passed\n";
    return 0;
}
