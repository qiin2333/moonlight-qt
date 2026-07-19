#pragma once

#include "backend/nvcomputer.h"

#include <QMutex>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

#include <atomic>
#include <memory>

namespace FileMappingTransfer {

struct State {
    std::atomic_bool stopRequested { false };
    QMutex lock;
    QWaitCondition finishedCondition;
    bool finished = false;
    QString outboxPath;
    QStringList events;
};

void start(NvComputer computer,
           QString mirrorRoot,
           std::shared_ptr<State> state,
           int timeoutMs = 10000);

// Network calls use a bounded timeout, so allow enough time for an in-flight
// request to unwind before the session removes the generated mirror folder.
void stopAndWait(const std::shared_ptr<State>& state, int timeoutMs = 15000);

} // namespace FileMappingTransfer
