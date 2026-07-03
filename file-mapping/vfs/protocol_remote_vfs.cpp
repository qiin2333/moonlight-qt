#include "protocol_remote_vfs.h"

#include <QByteArray>
#include <QStringList>

#include <utility>

namespace FileMapping {
namespace {
QString encodePart(const QString& value)
{
    return QString::fromLatin1(value.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool decodePart(const QString& value, QString& out)
{
    QByteArray bytes = QByteArray::fromBase64(value.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    out = QString::fromUtf8(bytes);
    return !out.isNull();
}

QString normalizedRemotePath(QString path)
{
    path.replace('\\', '/');
    while (path.startsWith('/')) {
        path.remove(0, 1);
    }

    QStringList parts;
    const QStringList rawParts = path.split('/', Qt::SkipEmptyParts);
    for (const QString& part : rawParts) {
        if (part == QStringLiteral(".")) {
            continue;
        }
        if (part == QStringLiteral("..")) {
            return {};
        }
        parts.append(part);
    }
    return parts.join(QLatin1Char('/'));
}
} // namespace

ProtocolRemoteVfs::ProtocolRemoteVfs(ProtocolClientPtr client, int timeoutMs)
    : m_Client(std::move(client)),
      m_TimeoutMs(timeoutMs)
{
}

ProtocolRemoteVfs::~ProtocolRemoteVfs() = default;

ChildrenResult ProtocolRemoteVfs::children(const VfsItemId& parentId)
{
    ChildrenResult result;
    if (!m_Client) {
        result.error = Error::make(ErrorKind::Unavailable, QStringLiteral("File mapping protocol client is not available"));
        return result;
    }

    const ParsedId parsed = parseId(parentId);
    if (parsed.type == ParsedId::Type::Root) {
        const QList<RemoteMapping> mappings = m_Client->mappings();
        result.items.reserve(mappings.size());
        for (const RemoteMapping& mapping : mappings) {
            result.items.append(mappingItem(mapping));
        }
        return result;
    }

    if (parsed.type != ParsedId::Type::Mapping && parsed.type != ParsedId::Type::Node) {
        result.error = invalidItemError(parentId);
        return result;
    }

    ListResult listResult = m_Client->list(parsed.mappingId, parsed.remotePath, m_TimeoutMs);
    if (!listResult.ok()) {
        result.error = listResult.error;
        return result;
    }

    result.items.reserve(listResult.entries.size());
    for (const RemoteEntry& entry : listResult.entries) {
        result.items.append(entryItem(parentId, entry));
    }
    return result;
}

ItemResult ProtocolRemoteVfs::item(const VfsItemId& id)
{
    ItemResult result;
    if (!m_Client) {
        result.error = Error::make(ErrorKind::Unavailable, QStringLiteral("File mapping protocol client is not available"));
        return result;
    }

    const ParsedId parsed = parseId(id);
    if (parsed.type == ParsedId::Type::Root) {
        result.item.id = VfsItemId::root();
        result.item.displayName = QStringLiteral("Moonlight Host Files");
        result.item.directory = true;
        return result;
    }

    if (parsed.type == ParsedId::Type::Mapping) {
        const QList<RemoteMapping> mappings = m_Client->mappings();
        for (const RemoteMapping& mapping : mappings) {
            if (mapping.id == parsed.mappingId) {
                result.item = mappingItem(mapping);
                return result;
            }
        }
        result.error = Error::make(ErrorKind::NotFound, QStringLiteral("Shared folder was not found"));
        return result;
    }

    if (parsed.type != ParsedId::Type::Node) {
        result.error = invalidItemError(id);
        return result;
    }

    StatResult statResult = m_Client->stat(parsed.mappingId, parsed.remotePath, m_TimeoutMs);
    if (!statResult.ok()) {
        result.error = statResult.error;
        return result;
    }
    if (!statResult.stat.exists) {
        result.error = Error::make(ErrorKind::NotFound, QStringLiteral("Remote item was not found"));
        return result;
    }

    result.item.id = id;
    result.item.parentId = mappingId(parsed.mappingId);
    const int slash = parsed.remotePath.lastIndexOf('/');
    if (slash > 0) {
        result.item.parentId = nodeId(parsed.mappingId, parsed.remotePath.left(slash));
    }
    result.item.displayName = slash >= 0 ? parsed.remotePath.mid(slash + 1) : parsed.remotePath;
    result.item.mappingId = parsed.mappingId;
    result.item.remotePath = parsed.remotePath;
    result.item.directory = statResult.stat.directory;
    result.item.size = statResult.stat.size;
    result.item.modifiedAt = statResult.stat.modifiedAt;
    return result;
}

OpenResult ProtocolRemoteVfs::open(const VfsItemId& id)
{
    OpenResult result;
    ItemResult itemResult = item(id);
    if (!itemResult.ok()) {
        result.error = itemResult.error;
        return result;
    }
    if (itemResult.item.directory) {
        result.error = Error::make(ErrorKind::Unsupported, QStringLiteral("Cannot open a directory for reading"));
        return result;
    }

    result.handle.id = m_NextHandleId++;
    result.handle.item = itemResult.item;
    m_OpenHandles.insert(result.handle.id, result.handle);
    return result;
}

ReadResult ProtocolRemoteVfs::read(const ReadHandle& handle, quint64 offset, quint32 length)
{
    ReadResult result;
    if (!m_Client) {
        result.error = Error::make(ErrorKind::Unavailable, QStringLiteral("File mapping protocol client is not available"));
        return result;
    }
    if (!handle.isValid() || !m_OpenHandles.contains(handle.id)) {
        result.error = Error::make(ErrorKind::NotFound, QStringLiteral("Read handle is not open"));
        return result;
    }

    const ReadHandle current = m_OpenHandles.value(handle.id);
    if (current.item.directory) {
        result.error = Error::make(ErrorKind::Unsupported, QStringLiteral("Cannot read a directory"));
        return result;
    }
    return m_Client->read(current.item.mappingId, current.item.remotePath, offset, length, m_TimeoutMs);
}

void ProtocolRemoteVfs::close(const ReadHandle& handle)
{
    m_OpenHandles.remove(handle.id);
}

VfsItemId ProtocolRemoteVfs::mappingId(const QString& mappingId)
{
    return { QStringLiteral("mapping:%1").arg(encodePart(mappingId)) };
}

VfsItemId ProtocolRemoteVfs::nodeId(const QString& mappingId, const QString& remotePath)
{
    return { QStringLiteral("node:%1:%2").arg(encodePart(mappingId), encodePart(normalizedRemotePath(remotePath))) };
}

ProtocolRemoteVfs::ParsedId ProtocolRemoteVfs::parseId(const VfsItemId& id) const
{
    ParsedId out;
    if (id.isRoot()) {
        out.type = ParsedId::Type::Root;
        return out;
    }

    if (id.value.startsWith(QStringLiteral("mapping:"))) {
        QString mapping;
        if (decodePart(id.value.mid(8), mapping) && !mapping.isEmpty()) {
            out.type = ParsedId::Type::Mapping;
            out.mappingId = mapping;
        }
        return out;
    }

    if (id.value.startsWith(QStringLiteral("node:"))) {
        const QStringList parts = id.value.split(':');
        if (parts.size() != 3) {
            return out;
        }
        QString mapping;
        QString path;
        if (!decodePart(parts[1], mapping) || !decodePart(parts[2], path)) {
            return out;
        }
        path = normalizedRemotePath(path);
        if (!mapping.isEmpty() && !path.isEmpty()) {
            out.type = ParsedId::Type::Node;
            out.mappingId = mapping;
            out.remotePath = path;
        }
    }
    return out;
}

VfsItem ProtocolRemoteVfs::mappingItem(const RemoteMapping& mapping) const
{
    VfsItem item;
    item.id = mappingId(mapping.id);
    item.parentId = VfsItemId::root();
    item.displayName = mapping.displayName.isEmpty() ? mapping.id : mapping.displayName;
    item.mappingId = mapping.id;
    item.directory = true;
    return item;
}

VfsItem ProtocolRemoteVfs::entryItem(const VfsItemId& parentId, const RemoteEntry& entry) const
{
    VfsItem item;
    item.id = nodeId(entry.mappingId, entry.path);
    item.parentId = parentId;
    item.displayName = entry.displayName;
    item.mappingId = entry.mappingId;
    item.remotePath = normalizedRemotePath(entry.path);
    item.directory = entry.directory;
    item.size = entry.size;
    item.modifiedAt = entry.modifiedAt;
    return item;
}

Error ProtocolRemoteVfs::invalidItemError(const VfsItemId& id) const
{
    return Error::make(ErrorKind::NotFound, QStringLiteral("Invalid file mapping VFS item id: %1").arg(id.value));
}

} // namespace FileMapping
