#include "clipboardipc.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace ClipboardIpc {
namespace {

QString typeToString(MessageType type)
{
    switch (type) {
    case MessageType::Configure:
        return QStringLiteral("configure");
    case MessageType::HostFrame:
        return QStringLiteral("hostFrame");
    case MessageType::LocalFrame:
        return QStringLiteral("localFrame");
    case MessageType::Ready:
        return QStringLiteral("ready");
    case MessageType::Error:
        return QStringLiteral("error");
    case MessageType::Stop:
        return QStringLiteral("stop");
    case MessageType::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

MessageType stringToType(const QString& type)
{
    if (type == QStringLiteral("configure")) {
        return MessageType::Configure;
    }
    if (type == QStringLiteral("hostFrame")) {
        return MessageType::HostFrame;
    }
    if (type == QStringLiteral("localFrame")) {
        return MessageType::LocalFrame;
    }
    if (type == QStringLiteral("ready")) {
        return MessageType::Ready;
    }
    if (type == QStringLiteral("error")) {
        return MessageType::Error;
    }
    if (type == QStringLiteral("stop")) {
        return MessageType::Stop;
    }
    return MessageType::Unknown;
}

QByteArray encodeObject(QJsonObject obj)
{
    obj.insert(QStringLiteral("version"), PROTOCOL_VERSION);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

QJsonObject baseMessage(MessageType type, quint32 sequence)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), typeToString(type));
    obj.insert(QStringLiteral("sequence"), static_cast<double>(sequence));
    return obj;
}

bool readBase64Payload(const QJsonObject& obj, const QString& key, QByteArray& out)
{
    QString encoded = obj.value(key).toString();
    if (encoded.isEmpty()) {
        out.clear();
        return false;
    }

    out = QByteArray::fromBase64(encoded.toLatin1());
    return !out.isEmpty();
}

}

QByteArray encodeConfigure(quint32 sequence, const HostConfig& config)
{
    QJsonObject host;
    host.insert(QStringLiteral("address"), config.address);
    host.insert(QStringLiteral("httpsPort"), static_cast<int>(config.httpsPort));
    host.insert(QStringLiteral("serverCertPem"),
                QString::fromLatin1(config.serverCertPem.toBase64()));
    host.insert(QStringLiteral("clientCertPem"),
                QString::fromLatin1(config.clientCertPem.toBase64()));
    host.insert(QStringLiteral("clientKeyPem"),
                QString::fromLatin1(config.clientKeyPem.toBase64()));

    QJsonObject obj = baseMessage(MessageType::Configure, sequence);
    obj.insert(QStringLiteral("host"), host);
    return encodeObject(obj);
}

QByteArray encodeHostFrame(quint32 sequence, const QByteArray& frame)
{
    QJsonObject obj = baseMessage(MessageType::HostFrame, sequence);
    obj.insert(QStringLiteral("payload"), QString::fromLatin1(frame.toBase64()));
    return encodeObject(obj);
}

QByteArray encodeLocalFrame(quint32 sequence, const QByteArray& frame)
{
    QJsonObject obj = baseMessage(MessageType::LocalFrame, sequence);
    obj.insert(QStringLiteral("payload"), QString::fromLatin1(frame.toBase64()));
    return encodeObject(obj);
}

QByteArray encodeReady(quint32 sequence)
{
    return encodeObject(baseMessage(MessageType::Ready, sequence));
}

QByteArray encodeError(quint32 sequence, const QString& code, const QString& text)
{
    QJsonObject obj = baseMessage(MessageType::Error, sequence);
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("text"), text);
    return encodeObject(obj);
}

QByteArray encodeStop(quint32 sequence)
{
    return encodeObject(baseMessage(MessageType::Stop, sequence));
}

bool decodeLine(const QByteArray& line, Message& outMessage, QString& outError)
{
    outMessage = Message();
    outError.clear();

    if (line.size() > MAX_LINE_BYTES) {
        outError = QStringLiteral("message line too large");
        return false;
    }

    QJsonParseError parseError{};
    QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        outError = QStringLiteral("invalid JSON message: %1").arg(parseError.errorString());
        return false;
    }

    QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("version")).toInt() != PROTOCOL_VERSION) {
        outError = QStringLiteral("unsupported protocol version");
        return false;
    }

    outMessage.type = stringToType(obj.value(QStringLiteral("type")).toString());
    QJsonValue sequenceValue = obj.value(QStringLiteral("sequence"));
    double sequence = sequenceValue.toDouble(-1);
    if (!sequenceValue.isDouble() ||
            sequence < 0 ||
            sequence > 4294967295.0 ||
            static_cast<double>(static_cast<quint32>(sequence)) != sequence) {
        outError = QStringLiteral("invalid sequence number");
        return false;
    }
    outMessage.sequence = static_cast<quint32>(sequence);
    if (outMessage.type == MessageType::Unknown) {
        outError = QStringLiteral("unknown message type");
        return false;
    }

    switch (outMessage.type) {
    case MessageType::Configure:
        {
            QJsonObject host = obj.value(QStringLiteral("host")).toObject();
            outMessage.config.address = host.value(QStringLiteral("address")).toString();
            outMessage.config.httpsPort = static_cast<quint16>(host.value(QStringLiteral("httpsPort")).toInt());
            readBase64Payload(host, QStringLiteral("serverCertPem"), outMessage.config.serverCertPem);
            readBase64Payload(host, QStringLiteral("clientCertPem"), outMessage.config.clientCertPem);
            readBase64Payload(host, QStringLiteral("clientKeyPem"), outMessage.config.clientKeyPem);
            return true;
        }
    case MessageType::HostFrame:
    case MessageType::LocalFrame:
        if (!readBase64Payload(obj, QStringLiteral("payload"), outMessage.frame)) {
            outError = QStringLiteral("missing or invalid frame payload");
            return false;
        }
        return true;
    case MessageType::Error:
        outMessage.code = obj.value(QStringLiteral("code")).toString();
        outMessage.text = obj.value(QStringLiteral("text")).toString();
        return true;
    case MessageType::Ready:
    case MessageType::Stop:
        return true;
    case MessageType::Unknown:
        break;
    }

    outError = QStringLiteral("unhandled message type");
    return false;
}

}
