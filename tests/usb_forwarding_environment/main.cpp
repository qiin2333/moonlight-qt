#include "../../app/backend/usbforwardingenvironment.h"
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using Env = UsbForwardingEnvironment;
    if (!Env::readinessError(Env::Ready).isEmpty()) return 1;
    for (auto state : {Env::Checking, Env::NotInstalled, Env::ServiceStopped,
                       Env::DriverStopped, Env::CheckFailed}) {
        if (Env::readinessError(state).isEmpty()) return 2;
    }
    if (Env::readinessError(Env::DriverStopped) ==
        Env::readinessError(Env::ServiceStopped)) return 3;
    if (app.arguments().contains(QStringLiteral("--require-ready")) &&
        Env::probeServices() != Env::Ready) return 4;
    qInfo() << "PASS readiness errors; optional live SCM readiness probe";
    return 0;
}
