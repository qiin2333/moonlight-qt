#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QList>
#include <QSslSocket>
#include <QString>

namespace FileMappingWebSocket {

constexpr quint64 kMaxMessageBytes = 16ULL * 1024ULL * 1024ULL;

struct Frame {
    bool fin = false;
    quint8 opcode = 0;
    QByteArray payload;
};

class TextMessageReader
{
public:
    QString read(QByteArray& buffer, QByteArray& out, bool& needMore, QList<QByteArray>* pongPayloads = nullptr);

private:
    QByteArray m_Payload;
    bool m_MessageStarted = false;
};

QString takeFrame(QByteArray& buffer, Frame& frame, bool& needMore);
// Keep the transport usable with the existing Network-only Qt/Steam Link SDKs.
// Reads, control replies, and writes share the caller's total deadline.
QString readJsonText(QSslSocket& socket, QByteArray& buffer, QJsonObject& out,
                     const QDeadlineTimer& deadline);
bool writeText(QSslSocket& socket, const QByteArray& payload, const QDeadlineTimer& deadline);
bool writePong(QSslSocket& socket, const QByteArray& payload, const QDeadlineTimer& deadline);

} // namespace FileMappingWebSocket
