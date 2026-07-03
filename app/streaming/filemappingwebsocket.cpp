#include "filemappingwebsocket.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QObject>
#include <QRandomGenerator>

#include <utility>

namespace {
bool waitForReadyBytes(QSslSocket& socket, QByteArray& buffer, int timeoutMs)
{
    if (!socket.waitForReadyRead(timeoutMs)) {
        return false;
    }
    buffer += socket.readAll();
    return true;
}

bool writeMaskedFrame(QSslSocket& socket, quint8 opcode, const QByteArray& payload)
{
    QByteArray frame;
    frame.append(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        frame.append(static_cast<char>(0x80 | payload.size()));
    }
    else if (payload.size() <= 0xffff) {
        frame.append(static_cast<char>(0x80 | 126));
        frame.append(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.append(static_cast<char>(payload.size() & 0xff));
    }
    else {
        frame.append(static_cast<char>(0x80 | 127));
        quint64 size = static_cast<quint64>(payload.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.append(static_cast<char>((size >> shift) & 0xff));
        }
    }

    QByteArray mask(4, Qt::Uninitialized);
    quint32 maskValue = QRandomGenerator::global()->generate();
    mask[0] = static_cast<char>((maskValue >> 24) & 0xff);
    mask[1] = static_cast<char>((maskValue >> 16) & 0xff);
    mask[2] = static_cast<char>((maskValue >> 8) & 0xff);
    mask[3] = static_cast<char>(maskValue & 0xff);
    frame.append(mask);

    for (int i = 0; i < payload.size(); ++i) {
        frame.append(static_cast<char>(static_cast<quint8>(payload[i]) ^ static_cast<quint8>(mask[i % 4])));
    }

    return socket.write(frame) == frame.size() && socket.waitForBytesWritten(3000);
}
} // namespace

namespace FileMappingWebSocket {

QString takeFrame(QByteArray& buffer, Frame& frame, bool& needMore)
{
    frame = {};
    needMore = false;
    if (buffer.size() < 2) {
        needMore = true;
        return {};
    }

    int cursor = 0;
    const quint8 first = static_cast<quint8>(buffer[cursor++]);
    const quint8 second = static_cast<quint8>(buffer[cursor++]);

    frame.fin = (first & 0x80) != 0;
    frame.opcode = first & 0x0f;
    if ((first & 0x70) != 0) {
        return QObject::tr("Unsupported WebSocket frame flags");
    }

    quint64 length = second & 0x7f;
    if (length == 126) {
        if (buffer.size() < cursor + 2) {
            needMore = true;
            return {};
        }
        length = (static_cast<quint8>(buffer[cursor]) << 8) |
                 static_cast<quint8>(buffer[cursor + 1]);
        cursor += 2;
    }
    else if (length == 127) {
        if (buffer.size() < cursor + 8) {
            needMore = true;
            return {};
        }
        length = 0;
        for (int i = 0; i < 8; ++i) {
            length = (length << 8) | static_cast<quint8>(buffer[cursor + i]);
        }
        cursor += 8;
    }

    if (length > kMaxMessageBytes) {
        return QObject::tr("WebSocket frame is too large");
    }

    const bool masked = (second & 0x80) != 0;
    QByteArray mask;
    if (masked) {
        if (buffer.size() < cursor + 4) {
            needMore = true;
            return {};
        }
        mask = buffer.mid(cursor, 4);
        cursor += 4;
    }

    if (buffer.size() - cursor < static_cast<int>(length)) {
        needMore = true;
        return {};
    }

    frame.payload = buffer.mid(cursor, static_cast<int>(length));
    cursor += static_cast<int>(length);
    buffer.remove(0, cursor);

    if (masked) {
        for (int i = 0; i < frame.payload.size(); ++i) {
            frame.payload[i] = static_cast<char>(static_cast<quint8>(frame.payload[i]) ^ static_cast<quint8>(mask[i % 4]));
        }
    }
    return {};
}

QString TextMessageReader::read(QByteArray& buffer, QByteArray& out, bool& needMore, QList<QByteArray>* pongPayloads)
{
    out.clear();
    needMore = false;

    for (;;) {
        Frame frame;
        QString frameError = takeFrame(buffer, frame, needMore);
        if (!frameError.isEmpty() || needMore) {
            return frameError;
        }

        if (static_cast<quint64>(m_Payload.size()) + static_cast<quint64>(frame.payload.size()) > kMaxMessageBytes) {
            return QObject::tr("WebSocket message is too large");
        }

        if (frame.opcode == 0x8) {
            return QObject::tr("WebSocket closed by host");
        }
        if (frame.opcode == 0x9 || frame.opcode == 0xa) {
            if (!frame.fin || frame.payload.size() > 125) {
                return QObject::tr("Invalid WebSocket control frame");
            }
            if (frame.opcode == 0x9 && pongPayloads != nullptr) {
                pongPayloads->append(frame.payload);
            }
            continue;
        }
        if (frame.opcode == 0x1) {
            if (m_MessageStarted) {
                return QObject::tr("Unexpected WebSocket text frame");
            }
            m_MessageStarted = true;
            m_Payload += frame.payload;
        }
        else if (frame.opcode == 0x0) {
            if (!m_MessageStarted) {
                return QObject::tr("Unexpected WebSocket continuation frame");
            }
            m_Payload += frame.payload;
        }
        else {
            return QObject::tr("Unexpected WebSocket opcode %1").arg(frame.opcode);
        }

        if (frame.fin) {
            out = std::move(m_Payload);
            m_Payload.clear();
            m_MessageStarted = false;
            return {};
        }
    }
}

QString readJsonText(QSslSocket& socket, QByteArray& buffer, QJsonObject& out, int timeoutMs)
{
    TextMessageReader reader;
    QByteArray payload;
    for (;;) {
        bool needMore = false;
        QList<QByteArray> pongPayloads;
        QString readError = reader.read(buffer, payload, needMore, &pongPayloads);
        if (!readError.isEmpty()) {
            return readError;
        }
        for (const QByteArray& pongPayload : pongPayloads) {
            if (!writePong(socket, pongPayload)) {
                const QString socketError = socket.errorString();
                return socketError.isEmpty()
                        ? QObject::tr("Failed to write WebSocket pong")
                        : socketError;
            }
        }
        if (!needMore) {
            break;
        }
        if (!waitForReadyBytes(socket, buffer, timeoutMs)) {
            return QObject::tr("Timed out waiting for WebSocket frame");
        }
    }

    QJsonParseError parseError {};
    QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return QObject::tr("WebSocket reply was not valid JSON: %1").arg(parseError.errorString());
    }
    out = doc.object();
    return {};
}

bool writeText(QSslSocket& socket, const QByteArray& payload)
{
    return writeMaskedFrame(socket, 0x1, payload);
}

bool writePong(QSslSocket& socket, const QByteArray& payload)
{
    if (payload.size() > 125) {
        return false;
    }
    return writeMaskedFrame(socket, 0xa, payload);
}

} // namespace FileMappingWebSocket
