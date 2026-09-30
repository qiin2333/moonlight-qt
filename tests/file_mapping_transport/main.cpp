#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "streaming/filemappingclient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <atomic>
#include <future>
#include <memory>

extern "C" const char* LiGetLaunchUrlQueryParameters(void)
{
    return "";
}

namespace {
QByteArray fixture(const char* name)
{
    QFile file(QStringLiteral(":/clipboard-test/") + QString::fromLatin1(name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

class TlsServer : public QTcpServer
{
public:
    TlsServer(std::atomic<int>& requests, std::atomic<bool>& invalidUpgrade)
        : m_Requests(requests), m_InvalidUpgrade(invalidUpgrade) {}

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto* socket = new QSslSocket(this);
        socket->setSocketDescriptor(descriptor);
        socket->setLocalCertificate(QSslCertificate(fixture("server.pem")));
        socket->setPrivateKey(QSslKey(fixture("server-key.pem"), QSsl::Rsa));
        socket->setPeerVerifyMode(QSslSocket::VerifyNone);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QSslSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QSslSocket::readyRead, this, [this, socket, buffer] {
            *buffer += socket->readAll();
            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            ++m_Requests;
            const QByteArray headers = buffer->left(headerEnd);
            buffer->clear();
            if (headers.startsWith("GET /api/v1/file-mapping/session")) {
                QByteArray key;
                for (const QByteArray& line : headers.split('\n')) {
                    if (line.toLower().startsWith("sec-websocket-key:")) {
                        key = line.mid(line.indexOf(':') + 1).trimmed();
                    }
                }
                const QByteArray accept = QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                                                  QCryptographicHash::Sha1).toBase64();
                QByteArray response = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n";
                if (!m_InvalidUpgrade.load()) {
                    response += "Upgrade: websocket\r\n";
                }
                response += "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
                // Coalesce the upgrade and hello to exercise buffered early data.
                const QByteArray hello = R"({"type":"hello","mappings":[]})";
                response.append(char(0x81));
                response.append(char(hello.size()));
                response += hello;
                socket->write(response);
            }
            else {
                const QByteArray body = headers.startsWith("GET /api/v1/usb-forwarding")
                        ? QByteArray(R"({"version":1,"enabled":false,"available":false})")
                        : QJsonDocument(QJsonObject { { "ok", true }, { "enabled", true }, { "listening", true },
                                                      { "port", int(serverPort()) }, { "session_token", "test-token" } })
                                  .toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            }
        });
        socket->startServerEncryption();
    }

private:
    std::atomic<int>& m_Requests;
    std::atomic<bool>& m_InvalidUpgrade;
};

class ServerThread : public QThread
{
public:
    std::promise<quint16> port;
    std::atomic<int> requests { 0 };
    std::atomic<bool> invalidUpgrade { false };

    ~ServerThread() override
    {
        quit();
        wait();
    }

    void run() override
    {
        TlsServer server(requests, invalidUpgrade);
        const bool listening = server.listen(QHostAddress::LocalHost);
        port.set_value(listening ? server.serverPort() : 0);
        if (listening) {
            exec();
        }
    }
};
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    QTemporaryDir settingsDirectory;
    if (!require(settingsDirectory.isValid() && QSslSocket::supportsSsl(), QStringLiteral("local TLS test environment"), err)) {
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MoonlightTests"));
    QCoreApplication::setApplicationName(QStringLiteral("FileMappingTransport"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    QSettings settings;
    settings.setValue("certificate", fixture("server.pem"));
    settings.setValue("key", fixture("server-key.pem"));
    settings.setValue("uniqueid", "test-client");
    IdentityManager::get();

    ServerThread server;
    auto listeningPort = server.port.get_future();
    server.start();
    const quint16 port = listeningPort.get();
    if (!require(port != 0, QStringLiteral("TLS server starts"), err)) {
        return 1;
    }
    NvComputer host;
    host.activeAddress = NvAddress(QStringLiteral("127.0.0.1"), port);
    host.activeHttpsPort = port;
    host.serverCert = QSslCertificate(fixture("server.pem"));
    host.uuid = QStringLiteral("test-host");
    bool ok = true;
    FileMappingClient::Capability capability;
    {
        FileMappingClient client(&host);
        capability = client.fetchCapability();
        ok &= require(capability.ok, QStringLiteral("paired self-signed capability request succeeds: %1").arg(capability.error), err);
        QString error;
        const bool connected = client.connectSession(capability, 3000, &error);
        ok &= require(connected, QStringLiteral("paired WebSocket connects and reads coalesced hello: %1").arg(error), err);
    }
    {
        NvHTTP http(host.activeAddress, port, host.serverCert);
        try {
            const auto usb = http.getUsbForwardingCapability();
            ok &= require(!usb.available, QStringLiteral("paired NvHTTP response is accepted"), err);
        }
        catch (const std::exception& error) {
            ok &= require(false, QString::fromUtf8(error.what()), err);
        }
    }
    {
        server.invalidUpgrade = true;
        FileMappingClient client(&host);
        QString error;
        ok &= require(!client.connectSession(capability, 3000, &error),
                      QStringLiteral("101 and accept alone cannot complete a WebSocket upgrade"), err);
        server.invalidUpgrade = false;
    }

    const QSslConfiguration previous = QSslConfiguration::defaultConfiguration();
    auto trusted = previous;
    trusted.setCaCertificates({ host.serverCert });
    QSslConfiguration::setDefaultConfiguration(trusted);
    host.serverCert = QSslCertificate(fixture("other.pem"));
    const int count = server.requests.load();
    {
        FileMappingClient client(&host);
        ok &= require(!client.fetchCapability().ok, QStringLiteral("trusted but unpaired capability server is rejected"), err);
        QString error;
        ok &= require(!client.connectSession(capability, 3000, &error),
                      QStringLiteral("trusted but unpaired WebSocket server is rejected"), err);
    }
    {
        NvHTTP http(host.activeAddress, port, host.serverCert);
        bool rejected = false;
        try {
            http.getUsbForwardingCapability();
        }
        catch (const GfeHttpResponseException& error) {
            rejected = error.getStatusCode() == 401;
        }
        catch (const std::exception&) {}
        ok &= require(rejected, QStringLiteral("trusted but unpaired NvHTTP server is rejected as a certificate mismatch"), err);
    }
    ok &= require(server.requests.load() == count, QStringLiteral("unpaired TLS peers receive no HTTP headers or session token"), err);
    QSslConfiguration::setDefaultConfiguration(previous);
    if (ok) {
        QTextStream(stdout) << "file_mapping_transport=passed\n";
    }
    return ok ? 0 : 1;
}
