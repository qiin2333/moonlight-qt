#include "unavailable_mount_provider.h"

#include <utility>

namespace FileMapping {

UnavailableMountProvider::UnavailableMountProvider(MountProviderKind kind, QString displayName, QString message)
    : m_Kind(kind),
      m_DisplayName(std::move(displayName)),
      m_Message(std::move(message))
{
}

MountProviderKind UnavailableMountProvider::kind() const
{
    return m_Kind;
}

QString UnavailableMountProvider::displayName() const
{
    return m_DisplayName;
}

MountStatus UnavailableMountProvider::status(const MountId& id)
{
    Q_UNUSED(id);

    MountStatus status;
    status.state = MountState::Unavailable;
    status.message = m_Message;
    return status;
}

MountResult UnavailableMountProvider::mount(const MountRequest& request)
{
    Q_UNUSED(request);

    MountResult result;
    result.error = MountError::make(ErrorKind::Unsupported, m_Message);
    result.status = status({});
    return result;
}

MountError UnavailableMountProvider::reveal(const MountId& id)
{
    Q_UNUSED(id);
    return MountError::make(ErrorKind::Unsupported, m_Message);
}

void UnavailableMountProvider::unmount(const MountId& id)
{
    Q_UNUSED(id);
}

} // namespace FileMapping
