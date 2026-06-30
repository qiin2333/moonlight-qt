#pragma once

#include "../protocol/file_mapping_client.h"
#include "remote_vfs.h"

#include <QHash>

namespace FileMapping {

class ProtocolRemoteVfs : public RemoteVfs
{
public:
    explicit ProtocolRemoteVfs(ProtocolClientPtr client, int timeoutMs = 5000);
    ~ProtocolRemoteVfs() override;

    ChildrenResult children(const VfsItemId& parentId) override;
    ItemResult item(const VfsItemId& id) override;
    OpenResult open(const VfsItemId& id) override;
    ReadResult read(const ReadHandle& handle, quint64 offset, quint32 length) override;
    void close(const ReadHandle& handle) override;

    static VfsItemId mappingId(const QString& mappingId);
    static VfsItemId nodeId(const QString& mappingId, const QString& remotePath);

private:
    struct ParsedId {
        enum class Type {
            Invalid,
            Root,
            Mapping,
            Node,
        };

        Type type = Type::Invalid;
        QString mappingId;
        QString remotePath;
    };

    ParsedId parseId(const VfsItemId& id) const;
    VfsItem mappingItem(const RemoteMapping& mapping) const;
    VfsItem entryItem(const VfsItemId& parentId, const RemoteEntry& entry) const;
    Error invalidItemError(const VfsItemId& id) const;

    ProtocolClientPtr m_Client;
    int m_TimeoutMs;
    quint64 m_NextHandleId = 1;
    QHash<quint64, ReadHandle> m_OpenHandles;
};

} // namespace FileMapping
