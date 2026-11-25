#include "settings/streamingpreferences.h"

#include <Limelight.h>
#include <QCoreApplication>
#include <QMetaProperty>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>

// Display environment detection is unrelated to the preference being tested.
namespace WMUtils {
bool isRunningWayland()
{
    return false;
}
bool isGpuSlow()
{
    return false;
}
}

static bool require(bool condition, const char* message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }
    return condition;
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir settingsDir;
    if (!require(settingsDir.isValid(), "temporary settings directory")) {
        return 1;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("MoonlightTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PyrowavePreferences"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir.path());

    qunsetenv("COLOR_SPACE_OVERRIDE");
    qunsetenv("COLOR_RANGE_OVERRIDE");
    qunsetenv("MOONLIGHT_ENABLE_PYROWAVE");
    auto* prefs = StreamingPreferences::get();
    bool ok = true;
    ok &= require(!prefs->enablePyrowave, "PyroWave defaults to disabled");
    ok &=
        require(!prefs->pyrowaveForcedByEnvironment(), "environment override defaults to disabled");
    qputenv("MOONLIGHT_ENABLE_PYROWAVE", "1");
    ok &= require(prefs->pyrowaveForcedByEnvironment(), "environment override enables PyroWave");
    qputenv("MOONLIGHT_ENABLE_PYROWAVE", "0");
    ok &= require(!prefs->pyrowaveForcedByEnvironment(), "zero does not enable PyroWave");
    qunsetenv("MOONLIGHT_ENABLE_PYROWAVE");
#ifdef HAVE_PYROWAVE
    ok &= require(prefs->pyrowaveAvailable(), "enabled build exposes PyroWave");
#else
    ok &= require(!prefs->pyrowaveAvailable(), "disabled build hides PyroWave availability");
#endif
    const int index = prefs->metaObject()->indexOfProperty("enablePyrowave");
    ok &= require(index >= 0, "preference is available to QML");
    if (index >= 0) {
        int changes = 0;
        const QMetaObject::Connection connection = QObject::connect(
            prefs, &StreamingPreferences::enablePyrowaveChanged, [&changes]() { ++changes; });
        const QMetaProperty property = prefs->metaObject()->property(index);
        ok &= require(property.isWritable() && property.hasNotifySignal(),
                      "QML preference has writable and observable state");
        ok &= require(property.write(prefs, true), "enable preference through QML property");
        ok &= require(prefs->enablePyrowave && changes == 1,
                      "QML write changes the value and notifies bindings");
        QObject::disconnect(connection);
    }
    prefs->save();
    prefs->enablePyrowave = false;
    prefs->reload();
    ok &= require(prefs->enablePyrowave, "enabled preference survives save and reload");
    prefs->enablePyrowave = false;
    prefs->save();
    prefs->enablePyrowave = true;
    prefs->reload();
    ok &= require(!prefs->enablePyrowave, "disabled preference survives save and reload");

    ok &= require(prefs->pyrowaveColorOverridesCompatible(), "default colors are compatible");
    qputenv("COLOR_SPACE_OVERRIDE", QByteArray::number(COLORSPACE_REC_709));
    qputenv("COLOR_RANGE_OVERRIDE", QByteArray::number(COLOR_RANGE_FULL));
    ok &= require(prefs->pyrowaveColorOverridesCompatible(), "BT.709 full range is compatible");
    qputenv("COLOR_SPACE_OVERRIDE", QByteArray::number(COLORSPACE_REC_601));
    ok &= require(!prefs->pyrowaveColorOverridesCompatible(), "BT.601 override is incompatible");
    qputenv("COLOR_SPACE_OVERRIDE", QByteArray::number(COLORSPACE_REC_709));
    qputenv("COLOR_RANGE_OVERRIDE", QByteArray::number(COLOR_RANGE_LIMITED));
    ok &= require(!prefs->pyrowaveColorOverridesCompatible(),
                  "limited range override is incompatible");

    if (ok) {
        QTextStream(stdout) << "pyrowave_preferences=passed\n";
    }
    return ok ? 0 : 1;
}
