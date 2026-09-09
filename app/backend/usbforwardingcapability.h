#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <optional>

namespace UsbForwarding {
// Credentials stay in memory; never log or persist this object.
struct Capability {
    bool available = false;
    QString reason;
    quint16 port = 0;
    QByteArray token;

    static std::optional<Capability> parse(const QByteArray& bytes) {
        if (bytes.size() > 4096) return std::nullopt;
        const auto document = QJsonDocument::fromJson(bytes);
        if (!document.isObject()) return std::nullopt;
        const auto json = document.object();
        if (json.value("version").toDouble(-1) != 1 ||
            !json.value("enabled").isBool() || !json.value("available").isBool()) return std::nullopt;
        Capability result;
        result.available = json.value("enabled").toBool() && json.value("available").toBool();
        result.reason = json.value("reason").toString();
        if (result.available) {
            const double port = json.value("port").toDouble(-1);
            const QString token = json.value("token").toString();
            if (port < 1 || port > 65535 || port != static_cast<int>(port) ||
                !QRegularExpression(QStringLiteral("\\A[0-9a-fA-F]{64}\\z")).match(token).hasMatch()) return std::nullopt;
            result.port = static_cast<quint16>(port);
            result.token = token.toLatin1();
        }
        return result;
    }
};
}
