#include "backend/identitymanager.h"
#include "backend/nvaddress.h"
#include "backend/nvcomputer.h"
#include "backend/nvhttp.h"
#include "backend/nvpairingmanager.h"
#include "streaming/filemappingclient.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMap>
#include <QSslCertificate>
#include <QTextStream>

extern "C" const char* LiGetLaunchUrlQueryParameters(void)
{
    return "";
}

namespace {
void printUsage(QTextStream& stream)
{
    stream << "Usage: moonlight-filemapping-smoke "
           << "--host <host> [--https-port <port>] [--server-cert <path> | --pair-pin <pin>] "
           << "--mapping <id> --path <relative-path> "
           << "[--offset <bytes>] [--length <bytes>] [--timeout-ms <ms>]\n";
}

bool parseArgs(const QStringList& args, QMap<QString, QString>& options, QString& error)
{
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args.at(i);
        if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            options.insert(QStringLiteral("help"), QStringLiteral("1"));
            return true;
        }
        if (!arg.startsWith(QStringLiteral("--"))) {
            error = QStringLiteral("Unexpected argument: %1").arg(arg);
            return false;
        }

        QString name = arg.mid(2);
        QString value;
        int equals = name.indexOf('=');
        if (equals >= 0) {
            value = name.mid(equals + 1);
            name = name.left(equals);
        }
        else {
            if (i + 1 >= args.size() || args.at(i + 1).startsWith(QStringLiteral("--"))) {
                error = QStringLiteral("Missing value for --%1").arg(name);
                return false;
            }
            value = args.at(++i);
        }
        options.insert(name, value.trimmed());
    }
    return true;
}

QString optionValue(const QMap<QString, QString>& options,
                    const QString& name,
                    const QString& fallback = {})
{
    return options.value(name, fallback).trimmed();
}

bool parseUInt16(const QString& text, quint16& value)
{
    bool ok = false;
    uint parsed = text.toUInt(&ok);
    if (!ok || parsed == 0 || parsed > 65535) {
        return false;
    }
    value = static_cast<quint16>(parsed);
    return true;
}

bool parseUInt64(const QString& text, quint64& value)
{
    bool ok = false;
    value = text.toULongLong(&ok);
    return ok;
}

bool parseUInt32(const QString& text, quint32& value)
{
    bool ok = false;
    uint parsed = text.toUInt(&ok);
    if (!ok) {
        return false;
    }
    value = static_cast<quint32>(parsed);
    return true;
}

QSslCertificate loadCertificate(const QString& path, QString& error)
{
    if (path.isEmpty()) {
        error = QStringLiteral("Missing --server-cert");
        return QSslCertificate(QByteArray());
    }
    if (!QFileInfo::exists(path)) {
        error = QStringLiteral("Server certificate does not exist: %1").arg(path);
        return QSslCertificate(QByteArray());
    }

    QList<QSslCertificate> certs = QSslCertificate::fromPath(path);
    if (certs.isEmpty()) {
        error = QStringLiteral("Server certificate is unreadable: %1").arg(path);
        return QSslCertificate(QByteArray());
    }
    return certs.first();
}

QString pairStateName(NvPairingManager::PairState state)
{
    switch (state) {
    case NvPairingManager::PAIRED:
        return QStringLiteral("paired");
    case NvPairingManager::PIN_WRONG:
        return QStringLiteral("pin_wrong");
    case NvPairingManager::ALREADY_IN_PROGRESS:
        return QStringLiteral("already_in_progress");
    case NvPairingManager::FAILED:
    default:
        return QStringLiteral("failed");
    }
}
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Moonlight Game Streaming Project"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("moonlight-stream.com"));
    QCoreApplication::setApplicationName(QStringLiteral("Moonlight"));

    QTextStream out(stdout);
    QTextStream err(stderr);

    QMap<QString, QString> options;
    QString parseError;
    if (!parseArgs(QCoreApplication::arguments(), options, parseError)) {
        err << parseError << '\n';
        printUsage(err);
        return 2;
    }
    if (options.contains(QStringLiteral("help"))) {
        printUsage(out);
        return 0;
    }

    const QString host = optionValue(options, QStringLiteral("host"));
    const QString mapping = optionValue(options, QStringLiteral("mapping"));
    const QString relativePath = optionValue(options, QStringLiteral("path"));
    if (host.isEmpty() || mapping.isEmpty() || relativePath.isEmpty()) {
        err << "Missing required --host, --mapping, or --path\n";
        printUsage(err);
        return 2;
    }

    quint16 httpsPort = 0;
    if (!parseUInt16(optionValue(options, QStringLiteral("https-port"), QStringLiteral("47984")), httpsPort)) {
        err << "Invalid --https-port\n";
        return 2;
    }

    quint64 offset = 0;
    if (!parseUInt64(optionValue(options, QStringLiteral("offset"), QStringLiteral("0")), offset)) {
        err << "Invalid --offset\n";
        return 2;
    }

    quint32 length = 0;
    if (!parseUInt32(optionValue(options, QStringLiteral("length"), QStringLiteral("4096")), length) || length == 0) {
        err << "Invalid --length\n";
        return 2;
    }

    quint32 timeoutMs = 0;
    if (!parseUInt32(optionValue(options, QStringLiteral("timeout-ms"), QStringLiteral("5000")), timeoutMs) || timeoutMs == 0) {
        err << "Invalid --timeout-ms\n";
        return 2;
    }

    NvComputer computer;
    const quint16 httpPort = httpsPort <= 65530 ? static_cast<quint16>(httpsPort + 5) : DEFAULT_HTTP_PORT;
    computer.activeAddress = NvAddress(host, httpPort);
    computer.activeHttpsPort = httpsPort;
    computer.isNvidiaServerSoftware = false;

    out << "client_uuid=" << IdentityManager::get()->getUniqueId() << '\n';
    out << "target=https://" << host << ':' << httpsPort << '\n';
    out.flush();

    const QString pairPin = optionValue(options, QStringLiteral("pair-pin"));
    if (!pairPin.isEmpty()) {
        if (pairPin.size() != 4) {
            err << "Invalid --pair-pin\n";
            return 2;
        }

        try {
            NvHTTP discovery(computer.activeAddress, 0, QSslCertificate(QByteArray()), true);
            QString serverInfo = discovery.getServerInfo(NvHTTP::NVLL_NONE, false);
            QString appVersion = NvHTTP::getXmlString(serverInfo, QStringLiteral("appversion"));
            if (appVersion.isEmpty()) {
                appVersion = NvHTTP::getXmlString(serverInfo, QStringLiteral("GfeVersion"));
            }
            computer.activeHttpsPort = discovery.httpsPort();
            computer.uuid = NvHTTP::getXmlString(serverInfo, QStringLiteral("uniqueid"));
            if (appVersion.isEmpty() || computer.activeHttpsPort == 0 || computer.uuid.isEmpty()) {
                err << "Pairing discovery failed: appversion, HttpsPort, or uniqueid missing\n";
                return 1;
            }

            out << "pairing=started\n";
            out << "host_uuid=" << computer.uuid << '\n';
            out << "https_port=" << computer.activeHttpsPort << '\n';
            out.flush();

            NvPairingManager pairing(&computer);
            QSslCertificate pairedCert;
            NvPairingManager::PairState pairState = pairing.pair(appVersion, pairPin, pairedCert);
            if (pairState != NvPairingManager::PAIRED || pairedCert.isNull()) {
                err << "pairing=failed\n";
                err << "pair_state=" << pairStateName(pairState) << '\n';
                return 1;
            }
            computer.serverCert = pairedCert;
            out << "pairing=passed\n";
        }
        catch (const std::exception& e) {
            err << "pairing=failed\n";
            err << "error=" << e.what() << '\n';
            return 1;
        }
    }
    else {
        QString certError;
        computer.serverCert = loadCertificate(optionValue(options, QStringLiteral("server-cert")), certError);
        if (computer.serverCert.isNull()) {
            err << certError << '\n';
            return 2;
        }
    }

    FileMappingClient client(&computer);
    FileMappingClient::SmokeResult result = client.smokeRead(mapping,
                                                             relativePath,
                                                             offset,
                                                             length,
                                                             static_cast<int>(timeoutMs));
    if (!result.ok) {
        err << "file_mapping_smoke=failed\n";
        err << "error=" << result.error << '\n';
        return 1;
    }

    out << "file_mapping_smoke=passed\n";
    out << "mapping=" << mapping << '\n';
    out << "path=" << relativePath << '\n';
    out << "read_type=" << result.read.value(QStringLiteral("type")).toString() << '\n';
    return 0;
}
