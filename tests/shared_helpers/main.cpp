#include "backend/pairedcertificate.h"
#include "settings/compatfetcher.h"
#include "versionutils.h"

#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

namespace {
bool require(bool condition, const QString& message, QTextStream& err)
{
    if (!condition) {
        err << "FAIL: " << message << '\n';
    }
    return condition;
}

QSslCertificate certificate(const char* name)
{
    QFile file(QStringLiteral(":/clipboard-test/") + QString::fromLatin1(name));
    return file.open(QIODevice::ReadOnly) ? QSslCertificate(file.readAll()) : QSslCertificate();
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream err(stderr);
    bool ok = true;

    ok &= require(VersionUtils::parseRelease("v6.2.82+14.g13ca12da.dirty") ==
                      QVersionNumber(6, 2, 82),
                  QStringLiteral("Git build metadata must not alter the release version"), err);
    ok &= require(VersionUtils::parseRelease(" V6.2.82-14-g13ca12da ") == QVersionNumber(6, 2, 82),
                  QStringLiteral("legacy Git suffix and uppercase prefix are accepted"), err);
    ok &= require(VersionUtils::compare(QVersionNumber(1, 2), QVersionNumber(1, 2, 0)) == 0,
                  QStringLiteral("missing trailing segments must compare as zero"), err);
    ok &= require(VersionUtils::compare(QVersionNumber(6, 9), QVersionNumber(6, 10)) < 0,
                  QStringLiteral("version components compare numerically"), err);
    for (const char* invalid : { "", "garbage", "1.2garbage", "1..2", "-1.2", "1.2.2147483648" }) {
        ok &= require(VersionUtils::parseNumeric(QString::fromLatin1(invalid)).isNull(),
                      QStringLiteral("invalid numeric version must not be partially accepted: %1")
                          .arg(invalid),
                      err);
    }

    // Isolate the compatibility cache from the user's real settings.
    QTemporaryDir settingsDirectory;
    ok &= require(settingsDirectory.isValid(), QStringLiteral("isolated settings directory"), err);
    if (!settingsDirectory.isValid()) {
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MoonlightTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SharedHelpers"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory.path());
    QSettings settings;
    ok &= require(CompatFetcher::isGfeVersionSupported("99.0"),
                  QStringLiteral("missing compatibility data remains permissive"), err);
    settings.setValue("latestsupportedversion-v1", "3.27");
    ok &= require(CompatFetcher::isGfeVersionSupported("3.27.0"),
                  QStringLiteral("equivalent GFE version is supported"), err);
    ok &= require(CompatFetcher::isGfeVersionSupported("3.26.99"),
                  QStringLiteral("older GFE version is supported"), err);
    ok &= require(!CompatFetcher::isGfeVersionSupported("3.28"),
                  QStringLiteral("newer GFE version is rejected"), err);
    ok &= require(CompatFetcher::isGfeVersionSupported("3.28garbage"),
                  QStringLiteral("malformed GFE version remains permissive"), err);
    settings.setValue("latestsupportedversion-v1", "garbage");
    ok &= require(CompatFetcher::isGfeVersionSupported("99.0"),
                  QStringLiteral("malformed compatibility data remains permissive"), err);

    const QSslCertificate pinned = certificate("server.pem");
    const QSslCertificate other = certificate("other.pem");
    ok &= require(!pinned.isNull() && !other.isNull(), QStringLiteral("certificate fixtures load"),
                  err);
    ok &= require(PairedCertificate::matches(pinned, pinned),
                  QStringLiteral("paired leaf certificate matches"), err);
    ok &= require(!PairedCertificate::matches(pinned, other),
                  QStringLiteral("unpaired certificate is rejected"), err);
    ok &= require(!PairedCertificate::matches(QSslCertificate(), QSslCertificate()),
                  QStringLiteral("two missing certificates never match"), err);
    ok &= require(PairedCertificate::canIgnoreErrors(
                      pinned, { QSslError(QSslError::SelfSignedCertificate, pinned),
                                QSslError(QSslError::HostNameMismatch, pinned) }),
                  QStringLiteral("paired self-signed/IP-address TLS errors may be ignored"), err);
    ok &= require(!PairedCertificate::canIgnoreErrors(
                      pinned, { QSslError(QSslError::SelfSignedCertificate, pinned),
                                QSslError(QSslError::SelfSignedCertificate, other) }),
                  QStringLiteral("mixed certificate errors must not be ignored"), err);
    ok &= require(
        !PairedCertificate::canIgnoreErrors(pinned, { QSslError(QSslError::NoPeerCertificate) }),
        QStringLiteral("TLS errors without a certificate must not be ignored"), err);

    if (ok) {
        QTextStream(stdout) << "shared_helpers=passed\n";
    }
    return ok ? 0 : 1;
}
