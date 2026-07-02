#pragma once

#include <QByteArray>
#include <QJsonObject>
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
    QString read(QByteArray& buffer, QByteArray& out, bool& needMore);

private:
    QByteArray m_Payload;
    bool m_MessageStarted = false;
};

QString takeFrame(QByteArray& buffer, Frame& frame, bool& needMore);
QString readJsonText(QSslSocket& socket, QByteArray& buffer, QJsonObject& out, int timeoutMs);
bool writeText(QSslSocket& socket, const QByteArray& payload);

} // namespace FileMappingWebSocket
