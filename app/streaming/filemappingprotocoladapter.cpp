#include "filemappingprotocoladapter.h"

#include "filemappingclient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <utility>

namespace {
FileMapping::Error makeError(FileMapping::ErrorKind kind, const QString& message)
{
    return FileMapping::Error::make(kind, message);
}

FileMapping::Error mapRpcError(const QString& message)
{
    if (message.isEmpty()) {
        return FileMapping::Error::none();
    }
    if (message.contains(QStringLiteral("not_found"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("mapping_not_found"), Qt::CaseInsensitive)) {
        return makeError(FileMapping::ErrorKind::NotFound, message);
    }
    if (message.contains(QStringLiteral("forbidden"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("unauthorized"), Qt::CaseInsensitive)) {
        return makeError(FileMapping::ErrorKind::Unauthorized, message);
    }
    if (message.contains(QStringLiteral("timed out"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("timeout"), Qt::CaseInsensitive)) {
        return makeError(FileMapping::ErrorKind::Timeout, message);
    }
    if (message.contains(QStringLiteral("not connected"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("network"), Qt::CaseInsensitive) ||
            message.contains(QStringLiteral("WebSocket"), Qt::CaseInsensitive)) {
        return makeError(FileMapping::ErrorKind::Network, message);
    }
    return makeError(FileMapping::ErrorKind::Internal, message);
}

FileMapping::Capability convertCapability(const FileMappingClient::Capability& source)
{
    FileMapping::Capability out;
    out.available = source.ok && source.enabled && source.listening;
    out.enabled = source.enabled;
    out.listening = source.listening;
    out.port = source.port;
    out.sessionEndpoint = source.sessionEndpoint;
    out.sessionUrl = source.sessionUrl;
    out.sessionToken = source.sessionToken;
    out.clientUuid = source.clientUuid;
    out.error = mapRpcError(source.error);
    if (!source.ok && out.error.ok()) {
        out.error = makeError(FileMapping::ErrorKind::Unavailable, QStringLiteral("File mapping capability is unavailable"));
    }
    return out;
}

bool isDirectoryKind(const QString& kind)
{
    return kind == QStringLiteral("directory");
}

FileMapping::RemoteEntry entryFromJson(const QString& mappingId, const QString& parentPath, const QJsonObject& item)
{
    const QString name = item.value(QStringLiteral("name")).toString();
    QString path = parentPath;
    if (!path.isEmpty() && !path.endsWith('/')) {
        path += QLatin1Char('/');
    }
    path += name;

    FileMapping::RemoteEntry entry;
    entry.mappingId = mappingId;
    entry.path = path;
    entry.displayName = name;
    entry.directory = isDirectoryKind(item.value(QStringLiteral("kind")).toString());
    entry.size = static_cast<quint64>(item.value(QStringLiteral("size")).toDouble(0));
    return entry;
}

FileMapping::RemoteStat statFromJson(const QJsonObject& reply)
{
    FileMapping::RemoteStat stat;
    const QString kind = reply.value(QStringLiteral("kind")).toString();
    stat.exists = !kind.isEmpty();
    stat.directory = isDirectoryKind(kind);
    stat.size = static_cast<quint64>(reply.value(QStringLiteral("size")).toDouble(0));
    return stat;
}
} // namespace

FileMappingProtocolAdapter::FileMappingProtocolAdapter(NvComputer computer)
    : m_Computer(std::move(computer))
{
}

FileMappingProtocolAdapter::~FileMappingProtocolAdapter() = default;

FileMapping::Capability FileMappingProtocolAdapter::fetchCapability(int timeoutMs)
{
    return convertCapability(client().fetchCapability(timeoutMs));
}

FileMapping::Error FileMappingProtocolAdapter::connectSession(const FileMapping::Capability& capability, int timeoutMs)
{
    FileMappingClient::Capability legacyCapability;
    legacyCapability.ok = capability.error.ok();
    legacyCapability.enabled = capability.enabled;
    legacyCapability.listening = capability.listening;
    legacyCapability.port = capability.port;
    legacyCapability.sessionEndpoint = capability.sessionEndpoint;
    legacyCapability.sessionUrl = capability.sessionUrl;
    legacyCapability.sessionToken = capability.sessionToken;
    legacyCapability.clientUuid = capability.clientUuid;
    legacyCapability.error = capability.error.message;

    QString error;
    if (!client().connectSession(legacyCapability, timeoutMs, &error)) {
        return mapRpcError(error);
    }
    return FileMapping::Error::none();
}

FileMapping::ListResult FileMappingProtocolAdapter::list(const QString& mappingId, const QString& path, int timeoutMs)
{
    FileMapping::ListResult result;
    FileMappingClient::RpcResult rpc = client().list(mappingId, path, timeoutMs);
    if (!rpc.ok) {
        result.error = mapRpcError(rpc.error);
        return result;
    }

    const QJsonArray entries = rpc.reply.value(QStringLiteral("entries")).toArray();
    result.entries.reserve(entries.size());
    for (const QJsonValue& value : entries) {
        if (value.isObject()) {
            result.entries.append(entryFromJson(mappingId, path, value.toObject()));
        }
    }
    return result;
}

FileMapping::StatResult FileMappingProtocolAdapter::stat(const QString& mappingId, const QString& path, int timeoutMs)
{
    FileMapping::StatResult result;
    FileMappingClient::RpcResult rpc = client().stat(mappingId, path, timeoutMs);
    if (!rpc.ok) {
        result.error = mapRpcError(rpc.error);
        return result;
    }
    result.stat = statFromJson(rpc.reply);
    return result;
}

FileMapping::ReadResult FileMappingProtocolAdapter::read(const QString& mappingId,
                                                         const QString& path,
                                                         quint64 offset,
                                                         quint32 length,
                                                         int timeoutMs)
{
    FileMapping::ReadResult result;
    FileMappingClient::RpcResult rpc = client().read(mappingId, path, offset, length, timeoutMs);
    if (!rpc.ok) {
        result.error = mapRpcError(rpc.error);
        return result;
    }

    result.data = QByteArray::fromBase64(rpc.reply.value(QStringLiteral("data")).toString().toUtf8());
    return result;
}

FileMappingClient& FileMappingProtocolAdapter::client()
{
    if (!m_Client) {
        m_Client = std::make_unique<FileMappingClient>(&m_Computer);
    }
    return *m_Client;
}
