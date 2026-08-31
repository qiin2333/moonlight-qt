#include "usb_agent_server.h"
#include "usb_agent_backend.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QProcessEnvironment>

#include <utility>

using namespace RemoteUsbAgent;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("moonlight-usb-agent"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Moonlight USB bridge agent"));
    parser.addHelpOption();
    QCommandLineOption socketOption({ QStringLiteral("s"), QStringLiteral("socket") },
                                     QStringLiteral("local IPC socket name"),
                                     QStringLiteral("name"));
    QCommandLineOption tokenOption({ QStringLiteral("t"), QStringLiteral("token") },
                                    QStringLiteral("one-session IPC bearer token"),
                                    QStringLiteral("token"));
    parser.addOption(socketOption);
    parser.addOption(tokenOption);
    parser.process(app);

    QByteArray token = parser.value(tokenOption).toUtf8();
    if (token.isEmpty()) {
        token = qEnvironmentVariable("MOONLIGHT_USB_AGENT_TOKEN").toUtf8();
    }
    LibusbBackend backend;
    Server server(parser.value(socketOption), std::move(token), &backend);
    QString error;
    if (!server.listen(&error)) {
        qCritical().noquote() << error;
        return 2;
    }
    return app.exec();
}
