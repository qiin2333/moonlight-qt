#pragma once

#include "backend/nvcomputer.h"
#include "video/overlaymenupanel.h"

#include <QMutex>
#include <QString>

#include <memory>

namespace FileMappingUx {

struct ProbeState {
    QMutex lock;
    bool pending = false;
    bool available = false;
    bool error = false;
    QString detail;
    QString message;
    QString diagnosticsPath;
};

struct MountState {
    QMutex lock;
    bool pending = false;
    bool ok = false;
    QString detail;
    QString message;
    QString displayPath;
    QString diagnosticsPath;
};

QString stateName(OverlayMenuPanel::FileMappingState state);
QString diagnosticsDirectory();
QString diagnosticsPath();
QString appendDiagnostic(const QString& event,
                         const QString& detail = QString(),
                         const QString& hostUuid = QString(),
                         const QString& sessionId = QString());

void startCapabilityProbe(NvComputer computer,
                          std::shared_ptr<ProbeState> state,
                          int timeoutMs);
void startMount(NvComputer computer,
                QString sessionId,
                std::shared_ptr<MountState> state,
                int timeoutMs);

} // namespace FileMappingUx
