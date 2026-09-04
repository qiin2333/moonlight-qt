// Runs the real client tunnel without a video session, for transport regression
// tests and real USB/IP end-to-end checks. Credentials are ephemeral test files.
#include "../../app/backend/usbforwardingtunnel.h"
#include <QCoreApplication>
#include <QFile>
#include <QSslKey>
#include <QTextStream>
#include <QTimer>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 10) {
        QTextStream(stderr) << "usage: probe host port peer-cert client-cert key token-file busid local-port stop-file\n";
        return 2;
    }
    const auto read = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        return file.readAll();
    };
    UsbForwarding::TunnelConfig config;
    config.host = args[1];
    config.port = args[2].toUShort();
    config.pinnedServerCertificate = QSslCertificate(read(args[3]));
    config.sslConfiguration.setLocalCertificate(QSslCertificate(read(args[4])));
    config.sslConfiguration.setPrivateKey(QSslKey(read(args[5]), QSsl::Rsa));
    config.sessionToken = read(args[6]).trimmed();
    config.busId = args[7].toUtf8();
    config.localPort = args[8].toUShort();
    UsbForwarding::Tunnel tunnel(std::move(config));
    QObject::connect(&tunnel, &UsbForwarding::Tunnel::forwarding, [] {
        QTextStream(stdout) << "FORWARDING" << Qt::endl;
    });
    QObject::connect(&tunnel, &UsbForwarding::Tunnel::finished, [&](const QString &error) {
        QTextStream(stdout) << "FINISHED " << error << Qt::endl;
        app.exit(error.isEmpty() ? 0 : 1);
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (QFile::exists(args[9])) {
            tunnel.stop();
            QTextStream(stdout) << "STOPPED" << Qt::endl;
            app.quit();
        }
    });
    poll.start(20);
    QTimer::singleShot(120000, &app, [&] { tunnel.stop(); app.exit(3); });
    QString error;
    if (!tunnel.start(&error)) {
        QTextStream(stderr) << error << Qt::endl;
        return 1;
    }
    return app.exec();
}
