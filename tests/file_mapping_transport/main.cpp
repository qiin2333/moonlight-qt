#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "backend/pairedcertificate.h"
#include "streaming/filemappingclient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

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

enum class RpcReplyMode
{
    None,
    Matching,
    Wrong,
    Missing,
    String,
    Error,
    Stale
};

struct ServerState
{
    std::atomic<int> requests{ 0 };
    std::atomic<int> connections{ 0 };
    std::atomic<bool> invalidUpgrade{ false };
    std::atomic<bool> keepAlive{ false };
    std::atomic<quint16> redirectPort{ 0 };
    std::atomic<bool> redirectPlaintext{ false };
    std::atomic<int> redirectStatus{ 302 };
    std::atomic<bool> plaintext{ false };
    std::atomic<int> serverInfoStatus{ 200 };
    std::atomic<quint16> advertisedHttpsPort{ 0 };
    std::atomic<RpcReplyMode> rpcReplyMode{ RpcReplyMode::None };
};

QByteArray textFrame(const QJsonObject& message)
{
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    Q_ASSERT(payload.size() < 126);
    return QByteArray(1, char(0x81)) + QByteArray(1, char(payload.size())) + payload;
}

// Exercise actual connection reuse with the same SSL configuration as NvHTTP.
bool get(QNetworkAccessManager& manager, quint16 port)
{
    QNetworkRequest request(QUrl(QStringLiteral("https://127.0.0.1:%1/warmup").arg(port)));
    request.setSslConfiguration(IdentityManager::get()->getSslConfig());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QEventLoop loop;
    QNetworkReply* reply = manager.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    if (!reply->isFinished()) {
        loop.exec();
    }
    const bool ok = reply->isFinished() && reply->error() == QNetworkReply::NoError;
    reply->abort();
    delete reply;
    return ok;
}

template <typename Request> bool throwsHttpStatus(Request request, int status)
{
    try {
        request();
    } catch (const GfeHttpResponseException& error) {
        return error.getStatusCode() == status;
    } catch (const std::exception&) {
    }
    return false;
}

template <typename Request> bool rejectsCertificate(Request request)
{
    try {
        request();
    } catch (const QtNetworkReplyException& error) {
        return error.getError() == QNetworkReply::SslHandshakeFailedError;
    } catch (const std::exception&) {
    }
    return false;
}

class TlsServer : public QTcpServer
{
public:
    explicit TlsServer(ServerState& state) : m_State(state) {}

protected:
    void incomingConnection(qintptr descriptor) override
    {
        ++m_State.connections;
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
            ++m_State.requests;
            const QByteArray headers = buffer->left(headerEnd);
            buffer->clear();
            if (m_State.redirectPort.load() != 0) {
                socket->write("HTTP/1.1 " + QByteArray::number(m_State.redirectStatus.load()) +
                              " Redirect\r\nLocation: " +
                              (m_State.redirectPlaintext.load() ? "http" : "https") +
                              "://127.0.0.1:" + QByteArray::number(m_State.redirectPort.load()) +
                              "/redirect-target?session_token=test-token\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            if (headers.startsWith("GET /api/v1/file-mapping/session")) {
                QByteArray key;
                for (const QByteArray& line : headers.split('\n')) {
                    if (line.toLower().startsWith("sec-websocket-key:")) {
                        key = line.mid(line.indexOf(':') + 1).trimmed();
                    }
                }
                const QByteArray accept =
                    QCryptographicHash::hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                             QCryptographicHash::Sha1)
                        .toBase64();
                QByteArray response = "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n";
                if (!m_State.invalidUpgrade.load()) {
                    response += "Upgrade: websocket\r\n";
                }
                response += "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
                // Coalesce the upgrade and hello to exercise buffered early data.
                const QByteArray hello = R"({"type":"hello","mappings":[]})";
                response.append(char(0x81));
                response.append(char(hello.size()));
                response += hello;
                // Queue complete replies to exercise unsolicited/stale responses,
                // independently of the client's request-ID serialization.
                const auto mode = m_State.rpcReplyMode.load();
                if (mode != RpcReplyMode::None) {
                    QJsonObject reply{ { "type", "result" }, { "ok", true }, { "id", 1 } };
                    if (mode == RpcReplyMode::Wrong)
                        reply["id"] = 2;
                    if (mode == RpcReplyMode::Missing)
                        reply.remove("id");
                    if (mode == RpcReplyMode::String)
                        reply["id"] = QStringLiteral("1");
                    if (mode == RpcReplyMode::Error) {
                        reply["type"] = QStringLiteral("error");
                        reply["message"] = QStringLiteral("test error");
                    }
                    response += textFrame(reply);
                    response += textFrame({ { "type", "result" },
                                            { "ok", true },
                                            { "id", mode == RpcReplyMode::Stale ? 1 : 2 } });
                }
                socket->write(response);
            } else {
                QByteArray body =
                    headers.startsWith("GET /api/v1/usb-forwarding")
                        ? QByteArray(R"({"version":1,"enabled":false,"available":false})")
                        : QJsonDocument(QJsonObject{ { "ok", true },
                                                     { "enabled", true },
                                                     { "listening", true },
                                                     { "port", int(serverPort()) },
                                                     { "session_token", "test-token" } })
                              .toJson(QJsonDocument::Compact);
                if (headers.startsWith("GET /serverinfo?")) {
                    body = "<root status_code=\"" +
                           QByteArray::number(m_State.serverInfoStatus.load()) +
                           "\" status_message=\"test status\"><uniqueid>test-host</uniqueid>"
                           "<hostname>test host</hostname><PairStatus>0</PairStatus><HttpsPort>" +
                           QByteArray::number(m_State.advertisedHttpsPort.load() != 0
                                                  ? m_State.advertisedHttpsPort.load()
                                                  : serverPort()) +
                           "</HttpsPort></root>";
                }
                const bool keepAlive = m_State.keepAlive.load();
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                    QByteArray::number(body.size()) +
                    "\r\nConnection: " + (keepAlive ? "keep-alive" : "close") + "\r\n\r\n" + body);
                if (!keepAlive)
                    socket->disconnectFromHost();
            }
        });
        if (!m_State.plaintext.load()) {
            socket->startServerEncryption();
        }
    }

private:
    ServerState& m_State;
};

class ServerThread : public QThread
{
public:
    std::promise<quint16> port;
    ServerState state;

    ~ServerThread() override
    {
        quit();
        wait();
    }

    void run() override
    {
        TlsServer server(state);
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
    if (!require(settingsDirectory.isValid() && QSslSocket::supportsSsl(),
                 QStringLiteral("local TLS test environment"), err)) {
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
        ok &= require(capability.ok,
                      QStringLiteral("paired self-signed capability request succeeds: %1")
                          .arg(capability.error),
                      err);
        QString error;
        const bool connected = client.connectSession(capability, 3000, &error);
        ok &= require(
            connected,
            QStringLiteral("paired WebSocket connects and reads coalesced hello: %1").arg(error),
            err);
    }
    {
        NvHTTP http(host.activeAddress, port, host.serverCert);
        try {
            const auto usb = http.getUsbForwardingCapability();
            ok &=
                require(!usb.available, QStringLiteral("paired NvHTTP response is accepted"), err);
        } catch (const std::exception& error) {
            ok &= require(false, QString::fromUtf8(error.what()), err);
        }
    }
    for (auto mode : { RpcReplyMode::Matching, RpcReplyMode::Wrong, RpcReplyMode::Missing,
                       RpcReplyMode::String, RpcReplyMode::Error, RpcReplyMode::Stale }) {
        server.state.rpcReplyMode = mode;
        FileMappingClient client(&host);
        QString error;
        if (!require(client.connectSession(capability, 3000, &error),
                     QStringLiteral("RPC test session starts: %1").arg(error), err)) {
            ok = false;
            continue;
        }
        // A synchronous socket signal can reenter the client while writing.
        bool nestedRejected = false;
        const auto nested = QObject::connect(
            client.findChild<QSslSocket*>(), &QSslSocket::bytesWritten, &client, [&] {
                nestedRejected =
                    client.stat("mapping", "path").error.contains("already in progress");
            });
        const auto first = client.list("mapping", "path");
        QObject::disconnect(nested);
        ok &= require(nestedRejected, QStringLiteral("reentrant RPC cannot consume a reply"), err);
        if (mode == RpcReplyMode::Matching || mode == RpcReplyMode::Stale) {
            ok &= require(first.ok, QStringLiteral("matching RPC ID is accepted"), err);
        } else if (mode == RpcReplyMode::Error) {
            ok &= require(!first.ok && first.error == QStringLiteral("test error"),
                          QStringLiteral("matching error ID preserves the host error"), err);
        } else {
            ok &= require(!first.ok && first.error.contains("ID"),
                          QStringLiteral("wrong, missing or string RPC ID is rejected"), err);
        }
        const auto second = client.stat("mapping", "path");
        if (mode == RpcReplyMode::Matching || mode == RpcReplyMode::Error) {
            ok &= require(second.ok, QStringLiteral("next matching RPC remains usable"), err);
        } else if (mode == RpcReplyMode::Stale) {
            ok &= require(!second.ok && second.error.contains("ID") &&
                              client.stat("mapping", "path").error.contains("not connected"),
                          QStringLiteral("stale reply closes the session"), err);
        } else {
            ok &= require(!second.ok && second.error.contains("not connected"),
                          QStringLiteral("ID mismatch prevents reuse of queued replies"), err);
        }
    }
    server.state.rpcReplyMode = RpcReplyMode::None;
    {
        server.state.invalidUpgrade = true;
        FileMappingClient client(&host);
        QString error;
        ok &= require(!client.connectSession(capability, 3000, &error),
                      QStringLiteral("101 and accept alone cannot complete a WebSocket upgrade"),
                      err);
        server.state.invalidUpgrade = false;
    }

    const QSslConfiguration previous = QSslConfiguration::defaultConfiguration();
    auto trusted = previous;
    trusted.setCaCertificates({ host.serverCert });
    QSslConfiguration::setDefaultConfiguration(trusted);
    server.state.keepAlive = true;
    {
        QNetworkAccessManager manager;
        int handshakes = 0;
        QObject::connect(&manager, &QNetworkAccessManager::encrypted, &app,
                         [&](QNetworkReply*) { ++handshakes; });
        auto enforcement = PairedCertificate::enforce(&manager, &app, host.serverCert);
        const int connections = server.state.connections.load();
        ok &=
            require(get(manager, port) && get(manager, port) && handshakes == 1 &&
                        server.state.connections.load() == connections + 1,
                    QStringLiteral("same paired identity reuses one verified TLS connection"), err);
        QObject::disconnect(enforcement);
        const int requests = server.state.requests.load();
        enforcement =
            PairedCertificate::enforce(&manager, &app, QSslCertificate(fixture("other.pem")));
        ok &= require(
            !get(manager, port) && server.state.requests.load() == requests,
            QStringLiteral("changed paired identity cannot send over a cached connection"), err);
        QObject::disconnect(enforcement);
    }
    {
        QNetworkAccessManager manager;
        int handshakes = 0;
        QObject::connect(&manager, &QNetworkAccessManager::encrypted, &app,
                         [&](QNetworkReply*) { ++handshakes; });
        ok &= require(get(manager, port) && get(manager, port) && handshakes == 1,
                      QStringLiteral("injected manager has a live unpinned connection"), err);
        const int requests = server.state.requests.load();
        NvHTTP http(host.activeAddress, port, QSslCertificate(fixture("other.pem")), false,
                    &manager);
        ok &= require(rejectsCertificate([&] { http.getUsbForwardingCapability(); }) &&
                          server.state.requests.load() == requests,
                      QStringLiteral("NvHTTP adopts an injected manager without leaking a request"),
                      err);
    }
    server.state.keepAlive = false;
    {
        ServerThread target;
        auto targetPort = target.port.get_future();
        target.start();
        server.state.redirectPort = targetPort.get();
        ok &=
            require(server.state.redirectPort != 0, QStringLiteral("redirect target starts"), err);
        for (bool plaintext : { false, true }) {
            server.state.redirectPlaintext = plaintext;
            for (int status : { 302, 307 }) {
                server.state.redirectStatus = status;
                FileMappingClient client(&host);
                const auto redirected = client.fetchCapability();
                ok &= require(!redirected.ok && redirected.error.contains("redirect"),
                              QStringLiteral("capability redirect is rejected"), err);
                NvHTTP http(host.activeAddress, port, host.serverCert);
                ok &= require(
                    throwsHttpStatus([&] { http.getUsbForwardingCapability(); }, status) &&
                        throwsHttpStatus(
                            [&] {
                                http.openConnectionToString(http.m_BaseUrlHttps, "serverinfo", {},
                                                            3000);
                            },
                            status) &&
                        throwsHttpStatus([&] { http.getAbrCapabilities(nullptr); }, status) &&
                        throwsHttpStatus([&] { http.configureAbr(false, 0, 0, {}, 3000); }, status),
                    QStringLiteral("NvHTTP GET/JSON GET/POST redirects are rejected"), err);
            }
        }
        ok &= require(
            target.state.connections.load() == 0,
            QStringLiteral("HTTPS and HTTP redirects receive no connection or credentials"), err);
        server.state.redirectPort = 0;
    }
    {
        ServerThread discovery;
        discovery.state.plaintext = true;
        discovery.state.advertisedHttpsPort = port;
        auto discoveryPort = discovery.port.get_future();
        discovery.start();
        const quint16 httpPort = discoveryPort.get();
        if (!require(httpPort != 0, QStringLiteral("plaintext discovery server starts"), err)) {
            return 1;
        }
        const NvAddress address(QStringLiteral("127.0.0.1"), httpPort);
        const QSslCertificate wrongPin(fixture("other.pem"));
        for (bool trustedPeer : { false, true }) {
            QSslConfiguration::setDefaultConfiguration(trustedPeer ? trusted : previous);
            NvHTTP http(address, port, wrongPin);
            const int requests = server.state.requests.load();
            ok &= require(
                rejectsCertificate([&] { http.getServerInfo(NvHTTP::NVLL_NONE, true); }) &&
                    rejectsCertificate([&] { http.getAbrCapabilities(nullptr); }) &&
                    rejectsCertificate([&] { http.configureAbr(false, 0, 0, {}, 3000); }),
                QStringLiteral("trusted or self-signed wrong peer raises a local TLS error"), err);
            ok &= require(
                discovery.state.connections.load() == 0 && server.state.requests.load() == requests,
                QStringLiteral(
                    "local certificate rejection sends neither TLS data nor HTTP fallback"),
                err);
        }
        QSslConfiguration::setDefaultConfiguration(trusted);
        // A remote GFE 401 arrives over correctly pinned TLS, unlike local rejection.
        server.state.serverInfoStatus = 401;
        {
            NvHTTP http(address, port, host.serverCert);
            try {
                const QString recovered = http.getServerInfo(NvHTTP::NVLL_NONE, true);
                ok &= require(NvHTTP::getXmlString(recovered, "uniqueid") == host.uuid &&
                                  discovery.state.requests.load() == 1,
                              QStringLiteral("authenticated remote 401 retains recovery behavior"),
                              err);
            } catch (const std::exception& error) {
                ok &= require(false, QString::fromUtf8(error.what()), err);
            }
        }
        server.state.serverInfoStatus = 403;
        {
            NvHTTP http(address, port, host.serverCert);
            ok &= require(
                throwsHttpStatus([&] { http.getServerInfo(NvHTTP::NVLL_NONE, true); }, 403) &&
                    discovery.state.requests.load() == 1,
                QStringLiteral("other remote status errors never use HTTP recovery"), err);
        }
        server.state.serverInfoStatus = 200;
        {
            NvHTTP http(address, 0, wrongPin);
            const int requests = discovery.state.requests.load();
            ok &= require(
                rejectsCertificate([&] { http.getServerInfo(NvHTTP::NVLL_NONE, true); }) &&
                    discovery.state.requests.load() == requests + 1 && http.httpsPort() == port,
                QStringLiteral(
                    "HTTP port discovery cannot turn a pin rejection into host metadata"),
                err);
        }
        {
            NvHTTP http(address, 0, QSslCertificate());
            try {
                const QString discovered = http.getServerInfo(NvHTTP::NVLL_NONE, true);
                ok &= require(NvHTTP::getXmlString(discovered, "uniqueid") == host.uuid &&
                                  http.httpsPort() == port,
                              QStringLiteral("unpaired discovery remains available"), err);
            } catch (const std::exception& error) {
                ok &= require(false, QString::fromUtf8(error.what()), err);
            }
        }
    }
    host.serverCert = QSslCertificate(fixture("other.pem"));
    const int count = server.state.requests.load();
    {
        FileMappingClient client(&host);
        ok &= require(!client.fetchCapability().ok,
                      QStringLiteral("trusted but unpaired capability server is rejected"), err);
        QString error;
        ok &= require(!client.connectSession(capability, 3000, &error),
                      QStringLiteral("trusted but unpaired WebSocket server is rejected"), err);
    }
    {
        NvHTTP http(host.activeAddress, port, host.serverCert);
        bool rejected = false;
        try {
            http.getUsbForwardingCapability();
        } catch (const QtNetworkReplyException& error) {
            rejected = error.getError() == QNetworkReply::SslHandshakeFailedError;
        } catch (const std::exception&) {
        }
        ok &=
            require(rejected,
                    QStringLiteral(
                        "trusted but unpaired NvHTTP server is rejected as a certificate mismatch"),
                    err);
    }
    ok &=
        require(server.state.requests.load() == count,
                QStringLiteral("unpaired TLS peers receive no HTTP headers or session token"), err);
    QSslConfiguration::setDefaultConfiguration(previous);
    if (ok) {
        QTextStream(stdout) << "file_mapping_transport=passed\n";
    }
    return ok ? 0 : 1;
}
