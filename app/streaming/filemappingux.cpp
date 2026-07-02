#include "filemappingux.h"

#include "filemappingclient.h"
#include "filemappingprotocoladapter.h"
#include "mount/mount_coordinator.h"
#include "mount/mount_provider_factory.h"
#include "SDL_compat.h"
#include "vfs/protocol_remote_vfs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QObject>
#include <QRunnable>
#include <QStandardPaths>
#include <QThreadPool>

#include <utility>

namespace {
QString singleLineDiagnosticValue(QString value)
{
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return value;
}

class CapabilityTask : public QRunnable
{
public:
    CapabilityTask(NvComputer computer,
                   std::shared_ptr<FileMappingUx::ProbeState> state,
                   int timeoutMs)
        : m_Computer(std::move(computer)),
          m_State(std::move(state)),
          m_TimeoutMs(timeoutMs)
    {
    }

    virtual void run() override
    {
        QString diagnosticsPath = FileMappingUx::appendDiagnostic(
                QStringLiteral("ux_probe.start"),
                QStringLiteral("timeout_ms=%1").arg(m_TimeoutMs),
                m_Computer.uuid,
                QString());

        FileMappingClient client(&m_Computer);
        FileMappingClient::Capability capability = client.fetchCapability(m_TimeoutMs);

        bool available = false;
        bool error = false;
        QString detail;
        QString message;

        if (!capability.ok) {
            error = !capability.error.isEmpty();
            detail = error ? QObject::tr("Error") : QObject::tr("Not shared");
            message = capability.error.isEmpty()
                    ? QObject::tr("No host folders are shared. On the host PC, right-click a folder and share it with Moonlight.")
                    : QObject::tr("Host file sharing could not be checked: %1").arg(capability.error);
        }
        else if (!capability.enabled) {
            detail = QObject::tr("Not shared");
            message = QObject::tr("No host folders are shared. On the host PC, right-click a folder and share it with Moonlight.");
        }
        else if (!capability.listening || capability.port == 0) {
            error = true;
            detail = QObject::tr("Starting");
            message = capability.error.isEmpty()
                    ? QObject::tr("Host file sharing is enabled but not ready yet.")
                    : capability.error;
        }
        else if (capability.sessionToken.isEmpty()) {
            error = true;
            detail = QObject::tr("Retry");
            message = capability.error.isEmpty()
                    ? QObject::tr("Host file sharing is waiting for a session token. Try again in a moment.")
                    : capability.error;
        }
        else {
            available = true;
            detail = QObject::tr("Ready");
            message = QObject::tr("Host files are ready. Shared folders are read-only in this session.");
        }

        diagnosticsPath = FileMappingUx::appendDiagnostic(
                QStringLiteral("ux_probe.result"),
                QStringLiteral("ok=%1 enabled=%2 listening=%3 port=%4 token=%5 detail=%6 message=%7")
                        .arg(capability.ok ? QStringLiteral("true") : QStringLiteral("false"),
                             capability.enabled ? QStringLiteral("true") : QStringLiteral("false"),
                             capability.listening ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(capability.port)
                        .arg(capability.sessionToken.isEmpty() ? QStringLiteral("missing") : QStringLiteral("present"),
                             detail,
                             message),
                m_Computer.uuid,
                QString());

        QMutexLocker locker(&m_State->lock);
        m_State->pending = true;
        m_State->available = available;
        m_State->error = error;
        m_State->detail = detail;
        m_State->message = message;
        m_State->diagnosticsPath = diagnosticsPath;
    }

private:
    NvComputer m_Computer;
    std::shared_ptr<FileMappingUx::ProbeState> m_State;
    int m_TimeoutMs;
};

class MountTask : public QRunnable
{
public:
    MountTask(NvComputer computer,
              QString sessionId,
              std::shared_ptr<FileMappingUx::MountState> state,
              int timeoutMs)
        : m_Computer(std::move(computer)),
          m_SessionId(std::move(sessionId)),
          m_State(std::move(state)),
          m_TimeoutMs(timeoutMs)
    {
    }

    virtual void run() override
    {
        bool ok = false;
        QString detail = QObject::tr("Error");
        QString message = QObject::tr("Host files could not be opened.");
        QString displayPath;
        QString diagnosticsPath = FileMappingUx::appendDiagnostic(
                QStringLiteral("mount_task.start"),
                QStringLiteral("timeout_ms=%1").arg(m_TimeoutMs),
                m_Computer.uuid,
                m_SessionId);

        auto client = std::make_shared<FileMappingProtocolAdapter>(m_Computer);
        FileMapping::Capability capability = client->fetchCapability(m_TimeoutMs);
        diagnosticsPath = FileMappingUx::appendDiagnostic(
                QStringLiteral("mount_task.capability"),
                QStringLiteral("error_ok=%1 enabled=%2 listening=%3 port=%4 token=%5 error=%6")
                        .arg(capability.error.ok() ? QStringLiteral("true") : QStringLiteral("false"),
                             capability.enabled ? QStringLiteral("true") : QStringLiteral("false"),
                             capability.listening ? QStringLiteral("true") : QStringLiteral("false"))
                        .arg(capability.port)
                        .arg(capability.sessionToken.isEmpty() ? QStringLiteral("missing") : QStringLiteral("present"),
                             capability.error.message),
                m_Computer.uuid,
                m_SessionId);
        if (!capability.error.ok()) {
            message = QObject::tr("Host file sharing could not be checked: %1").arg(capability.error.message);
        }
        else if (!capability.enabled) {
            detail = QObject::tr("Not shared");
            message = QObject::tr("No host folders are shared. On the host PC, right-click a folder and share it with Moonlight.");
        }
        else if (!capability.listening || capability.port == 0) {
            detail = QObject::tr("Starting");
            message = QObject::tr("Host file sharing is enabled but not ready yet.");
        }
        else if (capability.sessionToken.isEmpty()) {
            detail = QObject::tr("Retry");
            message = QObject::tr("Host file sharing is waiting for a session token. Try again in a moment.");
        }
        else {
            FileMapping::Error connectError = client->connectSession(capability, m_TimeoutMs);
            diagnosticsPath = FileMappingUx::appendDiagnostic(
                    QStringLiteral("mount_task.connect"),
                    QStringLiteral("ok=%1 error=%2")
                            .arg(connectError.ok() ? QStringLiteral("true") : QStringLiteral("false"),
                                 connectError.message),
                    m_Computer.uuid,
                    m_SessionId);
            if (!connectError.ok()) {
                message = QObject::tr("Host files could not be connected: %1").arg(connectError.message);
            }
            else {
                auto vfs = std::make_shared<FileMapping::ProtocolRemoteVfs>(client, m_TimeoutMs);
                FileMapping::ChildrenResult root = vfs->children(FileMapping::VfsItemId::root());
                diagnosticsPath = FileMappingUx::appendDiagnostic(
                        QStringLiteral("mount_task.root_list"),
                        root.ok()
                                ? QStringLiteral("ok=true mappings=%1").arg(root.items.size())
                                : QStringLiteral("ok=false error=%1").arg(root.error.message),
                        m_Computer.uuid,
                        m_SessionId);
                if (!root.ok()) {
                    message = QObject::tr("Host files root could not be listed: %1").arg(root.error.message);
                }
                else {
                    FileMapping::MountCoordinator coordinator(FileMapping::createDefaultMountProviders());
                    FileMapping::MountRequest request;
                    request.hostUuid = m_Computer.uuid;
                    request.hostName = m_Computer.name;
                    request.sessionId = m_SessionId;
                    request.vfs = vfs;

                    FileMapping::MountResult mount = coordinator.ensureMounted(request);
                    diagnosticsPath = FileMappingUx::appendDiagnostic(
                            QStringLiteral("mount_task.mount"),
                            QStringLiteral("ok=%1 state=%2 provider=%3 display_path=%4 error=%5 message=%6 diagnostics=%7")
                                    .arg(mount.ok() ? QStringLiteral("true") : QStringLiteral("false"))
                                    .arg(static_cast<int>(mount.status.state))
                                    .arg(mount.providerName)
                                    .arg(mount.status.displayPath,
                                         mount.error.message,
                                         mount.status.message,
                                         mount.diagnostics.join(QStringLiteral(" | "))),
                            m_Computer.uuid,
                            m_SessionId);
                    if (mount.ok() && mount.status.state == FileMapping::MountState::Mounted) {
                        ok = true;
                        detail = QObject::tr("Open");
                        displayPath = mount.status.displayPath;
                        const QString mountDiagnostics = mount.diagnostics.join(QStringLiteral("\n"));
                        const bool finderMirrorFallback = mount.providerName == QStringLiteral("macOS Finder mirror");
                        const bool fileProviderFallback = finderMirrorFallback
                                && mountDiagnostics.contains(QStringLiteral("File Provider"), Qt::CaseInsensitive);
                        const bool macFuseFallback = finderMirrorFallback
                                && mountDiagnostics.contains(QStringLiteral("macFUSE"), Qt::CaseInsensitive);
                        message = mount.status.message.isEmpty()
                                ? QObject::tr("Host files are ready.")
                                : mount.status.message;
                        if (fileProviderFallback) {
                            message = QObject::tr("Host files are ready as a Finder folder. Native Finder integration is not available in this build yet.");
                        }
                        else if (macFuseFallback) {
                            message = QObject::tr("Host files are ready as a Finder folder. Install macFUSE to mount them as a Finder volume.");
                        }
                    }
                    else {
                        message = mount.error.message.isEmpty()
                                ? QObject::tr("Host files could not be prepared.")
                                : mount.error.message;
                    }
                }
            }
        }

        QMutexLocker locker(&m_State->lock);
        m_State->pending = true;
        m_State->ok = ok;
        m_State->detail = detail;
        m_State->message = message;
        m_State->displayPath = displayPath;
        m_State->diagnosticsPath = diagnosticsPath;
    }

private:
    NvComputer m_Computer;
    QString m_SessionId;
    std::shared_ptr<FileMappingUx::MountState> m_State;
    int m_TimeoutMs;
};
} // namespace

namespace FileMappingUx {

QString stateName(OverlayMenuPanel::FileMappingState state)
{
    switch (state) {
    case OverlayMenuPanel::FileMappingState::Unknown:
        return QStringLiteral("unknown");
    case OverlayMenuPanel::FileMappingState::Checking:
        return QStringLiteral("checking");
    case OverlayMenuPanel::FileMappingState::Unavailable:
        return QStringLiteral("unavailable");
    case OverlayMenuPanel::FileMappingState::Available:
        return QStringLiteral("available");
    case OverlayMenuPanel::FileMappingState::Mounting:
        return QStringLiteral("mounting");
    case OverlayMenuPanel::FileMappingState::Open:
        return QStringLiteral("open");
    case OverlayMenuPanel::FileMappingState::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("unknown");
}

QString diagnosticsDirectory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (base.isEmpty()) {
        base = QDir::homePath();
    }
    if (base.isEmpty()) {
        base = QDir::tempPath();
    }
    return QDir(base).filePath(QStringLiteral("Moonlight Host Files"));
}

QString diagnosticsPath()
{
    return QDir(diagnosticsDirectory()).filePath(QStringLiteral("Moonlight File Mapping Diagnostics.log"));
}

QString appendDiagnostic(const QString& event,
                         const QString& detail,
                         const QString& hostUuid,
                         const QString& sessionId)
{
    const QString dir = diagnosticsDirectory();
    QDir().mkpath(dir);

    const QString path = diagnosticsPath();
    const QString line = QStringLiteral("%1 event=%2 host=%3 session=%4 detail=%5\n")
            .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs),
                 singleLineDiagnosticValue(event),
                 singleLineDiagnosticValue(hostUuid),
                 singleLineDiagnosticValue(sessionId),
                 singleLineDiagnosticValue(detail));

    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        file.write(line.toUtf8());
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "File mapping diagnostic: event=%s host=%s session=%s detail=%s path=%s",
                event.toUtf8().constData(),
                hostUuid.toUtf8().constData(),
                sessionId.toUtf8().constData(),
                detail.toUtf8().constData(),
                path.toUtf8().constData());
    return path;
}

void startCapabilityProbe(NvComputer computer,
                          std::shared_ptr<ProbeState> state,
                          int timeoutMs)
{
    QThreadPool::globalInstance()->start(new CapabilityTask(std::move(computer),
                                                            std::move(state),
                                                            timeoutMs));
}

void startMount(NvComputer computer,
                QString sessionId,
                std::shared_ptr<MountState> state,
                int timeoutMs)
{
    QThreadPool::globalInstance()->start(new MountTask(std::move(computer),
                                                       std::move(sessionId),
                                                       std::move(state),
                                                       timeoutMs));
}

} // namespace FileMappingUx
