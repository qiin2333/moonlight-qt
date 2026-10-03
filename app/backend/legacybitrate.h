#pragma once
#include <QString>
#include <QXmlStreamReader>
#include <optional>

// The old endpoint acknowledges only a submitted target, never SDK or wire-budget application.
inline std::optional<int> acceptedLegacyBitrate(const QString& xml, int requestedKbps)
{
    if (requestedKbps < 1 || requestedKbps > 800000)
        return std::nullopt;
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement() || reader.name() != QStringLiteral("root"))
        return std::nullopt;
    bool valid = false;
    const auto attributes = reader.attributes();
    if (attributes.value(QStringLiteral("status_code")).toUInt(&valid) != 200 || !valid)
        return std::nullopt;
    if (attributes.hasAttribute(QStringLiteral("bitrate"))) {
        const int replyTarget = attributes.value(QStringLiteral("bitrate")).toInt(&valid);
        if (!valid || replyTarget != requestedKbps)
            return std::nullopt;
    }
    int flags = 0;
    bool accepted = false;
    while (reader.readNextStartElement()) {
        if (reader.name() == QStringLiteral("bitrate")) {
            ++flags;
            accepted = reader.readElementText() == QStringLiteral("1");
        } else
            reader.skipCurrentElement();
    }
    while (!reader.atEnd())
        reader.readNext();
    if (reader.hasError() || flags != 1 || !accepted)
        return std::nullopt;
    return requestedKbps;
}
