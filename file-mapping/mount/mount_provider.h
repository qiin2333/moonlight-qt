#pragma once

#include <memory>

#include <QString>

#include "mount_errors.h"
#include "mount_session.h"
#include "../vfs/remote_vfs.h"

namespace FileMapping {

enum class MountProviderKind {
    Fallback,
    MacFileProvider,
    MacFuse,
    WindowsWinFsp,
    LinuxFuse,
    MobileDocumentProvider,
};

struct MountRequest {
    QString hostUuid;
    QString hostName;
    QString sessionId;
    std::shared_ptr<RemoteVfs> vfs;
};

struct MountResult {
    MountError error;
    MountId id;
    MountStatus status;

    bool ok() const { return error.ok(); }
};

class MountProvider {
public:
    virtual ~MountProvider();

    virtual MountProviderKind kind() const = 0;
    virtual QString displayName() const = 0;
    virtual MountStatus status(const MountId& id) = 0;
    virtual MountResult mount(const MountRequest& request) = 0;
    virtual MountError reveal(const MountId& id) = 0;
    virtual void unmount(const MountId& id) = 0;
};

using MountProviderPtr = std::shared_ptr<MountProvider>;

} // namespace FileMapping
