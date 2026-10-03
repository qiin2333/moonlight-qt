#pragma once
#include <QJsonObject>
#include <QString>
#include <QXmlStreamReader>
#include <optional>
#include <stdexcept>

struct LegacyTransportScope
{
    QString sessionId;
    QString connectionEpoch;
    static bool validIdentity(const QString& value, const QString& maximum)
    {
        if (value.isEmpty() || value.startsWith('0') || value.size() > maximum.size())
            return false;
        for (const auto c : value)
            if (c < QLatin1Char('0') || c > QLatin1Char('9'))
                return false;
        return value.size() < maximum.size() || value <= maximum;
    }
    static bool valid(const QString& id, const QString& epoch)
    {
        return validIdentity(id, QStringLiteral("4294967295")) &&
               validIdentity(epoch, QStringLiteral("18446744073709551615"));
    }
    QString querySuffix() const
    {
        return QStringLiteral("&sessionId=%1&connectionEpoch=%2").arg(sessionId, connectionEpoch);
    }
    void appendTo(QJsonObject& body) const
    {
        body[QStringLiteral("sessionId")] = sessionId;
        body[QStringLiteral("connectionEpoch")] = connectionEpoch;
    }
};

inline std::optional<LegacyTransportScope> parseLegacyTransportScope(const QString& xml)
{
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement() || reader.name() != QStringLiteral("root"))
        throw std::invalid_argument("Invalid launch response");
    std::optional<QString> ack, id, epoch;
    while (reader.readNextStartElement()) {
        std::optional<QString>* field =
            reader.name() == QStringLiteral("transportScope")             ? &ack
            : reader.name() == QStringLiteral("transportSessionId")       ? &id
            : reader.name() == QStringLiteral("transportConnectionEpoch") ? &epoch
                                                                          : nullptr;
        if (field) {
            if (*field)
                throw std::invalid_argument("Duplicate launch identity");
            *field = reader.readElementText();
        } else
            reader.skipCurrentElement();
    }
    while (!reader.atEnd())
        reader.readNext();
    if (reader.hasError())
        throw std::invalid_argument("Invalid launch response");
    if (!ack && !epoch)
        return std::nullopt;
    if (!ack || *ack != QStringLiteral("1") || !id || !epoch ||
        !LegacyTransportScope::valid(*id, *epoch))
        throw std::invalid_argument("Invalid transport scope handshake");
    return LegacyTransportScope{ *id, *epoch };
}
