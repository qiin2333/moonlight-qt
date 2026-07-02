#pragma once

#include "mount_provider.h"

#include <QHash>
#include <QStringList>

namespace FileMapping {

class MacOSFinderMirrorProvider : public MountProvider
{
public:
    MacOSFinderMirrorProvider();

    MountProviderKind kind() const override;
    QString displayName() const override;
    MountStatus status(const MountId& id) override;
    MountResult mount(const MountRequest& request) override;
    MountError reveal(const MountId& id) override;
    void unmount(const MountId& id) override;

private:
    struct MirrorLimits {
        int maxDepth = 4;
        int maxFiles = 128;
        quint64 maxBytes = 32ULL * 1024ULL * 1024ULL;
        quint64 maxFileBytes = 8ULL * 1024ULL * 1024ULL;
        quint32 chunkBytes = 64U * 1024U;
    };

    struct MirrorState {
        MountStatus status;
        QString rootPath;
    };

    struct MirrorCounters {
        int files = 0;
        quint64 bytes = 0;
        QStringList warnings;
    };

    static QString safeName(const QString& name, const QString& fallback);
    static QString uniqueChildPath(const QString& parentPath, const QString& requestedName);
    static QString cacheBasePath();
    static QString cacheRootPath(const MountRequest& request);
    static QString latestAliasPath();
    static void createLatestAlias(const QString& rootPath);
    static void removeLatestAlias(const QString& rootPath);
    static MirrorLimits limitsFromEnvironment();
    static bool writeTextFile(const QString& path, const QString& text);
    static void makeReadOnly(const QString& path, bool directory);

    Error mirrorChildren(RemoteVfs& vfs,
                         const VfsItem& parent,
                         const QString& localPath,
                         int depth,
                         const MirrorLimits& limits,
                         MirrorCounters& counters) const;
    Error mirrorFile(RemoteVfs& vfs,
                     const VfsItem& item,
                     const QString& localPath,
                     const MirrorLimits& limits,
                     MirrorCounters& counters) const;

    QHash<QString, MirrorState> m_Mounts;
};

} // namespace FileMapping
