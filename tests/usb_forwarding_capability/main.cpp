#include "../../app/backend/usbforwardingcapability.h"
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QByteArray token(64, 'a');
    const QByteArray ready = "{\"version\":1,\"enabled\":true,\"available\":true,\"reason\":\"ready\",\"port\":47996,\"token\":\"" + token + "\"}";
    const auto capability = UsbForwarding::Capability::parse(ready);
    if (!capability || !capability->available || capability->port != 47996 || capability->token != token) return 1;
    for (const auto& enabled : {QByteArray("true"), QByteArray("false")}) {
        const auto disabled = UsbForwarding::Capability::parse("{\"version\":1,\"enabled\":" + enabled + ",\"available\":false}");
        if (!disabled || disabled->available || !disabled->token.isEmpty() || disabled->port != 0) return 2;
    }
    QList<QByteArray> invalid {"{}", QByteArray(4097, 'x')};
    for (const auto& replacement : {QByteArray("0"), QByteArray("65536"), QByteArray("47996.5"), QByteArray("\"47996\"")})
        invalid << QByteArray(ready).replace("47996", replacement);
    invalid << QByteArray(ready).replace(token, "")
            << QByteArray(ready).replace("\"version\":1", "\"version\":2")
            << QByteArray(ready).replace("\"version\":1", "\"version\":\"1\"")
            << QByteArray(ready).replace("\"enabled\":true", "\"enabled\":\"true\"");
    for (const auto& bytes : invalid) if (UsbForwarding::Capability::parse(bytes)) return 3;
    qInfo() << "PASS runtime credentials, disabled/unavailable, malformed responses";
    return 0;
}
