#include "remoteusb/remote_usb_agent_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QRandomGenerator>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        return 2;
    }

    const QString socketName = QDir::temp().filePath(
        QStringLiteral("moonlight-usb-agent-client-%1.sock")
            .arg(QRandomGenerator::global()->generate()));
    const QByteArray token = QByteArrayLiteral("client-smoke-token");
    RemoteUsb::RemoteUsbAgentClient client;
    bool passed = false;
    QObject::connect(&client, &RemoteUsb::RemoteUsbAgentClient::ready,
                     &app, [&](quint32 version) {
        if (version != 1u) {
            app.exit(3);
            return;
        }
        client.enumerate();
    });
    QObject::connect(&client, &RemoteUsb::RemoteUsbAgentClient::devicesChanged,
                     &app, [&](const QJsonArray &devices) {
        passed = true;
        for (const QJsonValue &value : devices) {
            if (!value.isObject()) {
                passed = false;
                break;
            }
            const QJsonObject device = value.toObject();
            if (device.value(QStringLiteral("deviceId")).toString().isEmpty() ||
                device.value(QStringLiteral("busId")).toString().isEmpty() ||
                !device.value(QStringLiteral("endpoints")).isArray()) {
                passed = false;
                break;
            }
        }
        app.exit(passed ? 0 : 4);
    });
    QObject::connect(&client, &RemoteUsb::RemoteUsbAgentClient::failed,
                     &app, [&](const QString &) { app.exit(5); });

    QString error;
    if (!client.launch(QString::fromLocal8Bit(argv[1]), socketName, token,
                       &error)) {
        return 6;
    }
    QTimer::singleShot(5000, &app, [&] {
        if (!passed) {
            app.exit(7);
        }
    });
    return app.exec();
}
