#pragma once

#include <QList>

#include "../protocol/file_mapping_messages.h"
#include "vfs_handle.h"
#include "vfs_item.h"

namespace FileMapping {

struct ChildrenResult {
    Error error;
    QList<VfsItem> items;

    bool ok() const { return error.ok(); }
};

struct ItemResult {
    Error error;
    VfsItem item;

    bool ok() const { return error.ok(); }
};

struct OpenResult {
    Error error;
    ReadHandle handle;

    bool ok() const { return error.ok(); }
};

class RemoteVfs {
public:
    virtual ~RemoteVfs();

    virtual ChildrenResult children(const VfsItemId& parentId) = 0;
    virtual ItemResult item(const VfsItemId& id) = 0;
    virtual OpenResult open(const VfsItemId& id) = 0;
    virtual ReadResult read(const ReadHandle& handle, quint64 offset, quint32 length) = 0;
    virtual void close(const ReadHandle& handle) = 0;
};

} // namespace FileMapping
