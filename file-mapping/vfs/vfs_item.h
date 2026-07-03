#pragma once

#include <QDateTime>
#include <QString>

namespace FileMapping {

struct VfsItemId {
    QString value;

    bool isRoot() const { return value == QStringLiteral("root"); }
    bool isEmpty() const { return value.isEmpty(); }

    static VfsItemId root() { return { QStringLiteral("root") }; }
};

struct VfsItem {
    VfsItemId id;
    VfsItemId parentId;
    QString displayName;
    QString mappingId;
    QString remotePath;
    bool directory = false;
    quint64 size = 0;
    QDateTime modifiedAt;
};

} // namespace FileMapping
