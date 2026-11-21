#include "cli/commandlineparser.h"
#include "utils.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>

// The preferences code only needs these window-system queries for defaults.
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

struct ParserResult
{
    int exitCode;
    QProcess::ExitStatus exitStatus;
    QByteArray output;
    QByteArray error;
};

static ParserResult runParser(int initialMode, const QStringList& arguments)
{
    QProcess process;
    QStringList childArguments = { "--child", QString::number(initialMode) };
    childArguments.append(arguments);
    process.start(QCoreApplication::applicationFilePath(), childArguments);
    if (!process.waitForStarted() || !process.waitForFinished(30000)) {
        fprintf(stderr, "Failed to run parser child: %s\n", qPrintable(process.errorString()));
        std::exit(2);
    }
    return { process.exitCode(), process.exitStatus(), process.readAllStandardOutput(),
             process.readAllStandardError() };
}

static int runChild(const QStringList& arguments)
{
    const QString settingsPath = qEnvironmentVariable("MOONLIGHT_CLI_TEST_SETTINGS");
    if (settingsPath.isEmpty()) {
        return 2;
    }
    QCoreApplication::setOrganizationName("MoonlightCliTest");
    QCoreApplication::setApplicationName("cli_mouse_mode");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);

    StreamingPreferences* preferences = StreamingPreferences::get();
    preferences->mouseMode = static_cast<StreamingPreferences::MouseMode>(arguments.at(2).toInt());
    preferences->width = 1920;
    preferences->height = 1080;
    preferences->fps = 60;
    preferences->bitrateKbps = 10000;
    preferences->packetSize = 1392;
    preferences->enableVsync = true;
    preferences->absoluteTouchMode = true;
    preferences->swapMouseButtons = false;
    preferences->enableGamepadUiNavigation = true;

    QStringList parserArguments = { "moonlight" };
    parserArguments.append(arguments.mid(3));
    GlobalCommandLineParser globalParser;
    if (globalParser.parse(parserArguments) != GlobalCommandLineParser::StreamRequested) {
        return 3;
    }
    StreamCommandLineParser parser;
    parser.parse(parserArguments, preferences);

    const QByteArray uiMode = qgetenv("MOONLIGHT_CLI_TEST_UI_MODE");
    if (!uiMode.isEmpty() && !preferences->setProperty("mouseMode", uiMode.toInt())) {
        return 4;
    }

    // A later save (for example from an overlay setting) must not persist a
    // mouse mode selected only for this command-line launch.
    preferences->save();
    QSettings savedSettings;

    QJsonObject result = {
        { "mouseMode", preferences->mouseMode },
        { "savedMouseMode", savedSettings.value("mouseMode", -1).toInt() },
        { "host", parser.getHost() },
        { "app", parser.getAppName() },
        { "width", preferences->width },
        { "height", preferences->height },
        { "fps", preferences->fps },
        { "bitrate", preferences->bitrateKbps },
        { "packetSize", preferences->packetSize },
        { "vsync", preferences->enableVsync },
        { "absoluteTouchMode", preferences->absoluteTouchMode },
        { "swapMouseButtons", preferences->swapMouseButtons },
        { "gamepadUiNavigation", preferences->enableGamepadUiNavigation },
        { "globalGamepadOverride", globalParser.disableGamepadUiNavigation() },
    };
    puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return 0;
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() >= 3 && arguments.at(1) == "--child") {
        return runChild(arguments);
    }

    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) {
        fprintf(stderr, "Failed to create a temporary settings directory\n");
        return 2;
    }
    qputenv("MOONLIGHT_CLI_TEST_SETTINGS", settingsDirectory.path().toUtf8());

    int failures = 0;
    int checks = 0;
    const auto expectSuccess = [&](const QString& name, int initialMode, const QStringList& options,
                                   int expectedMode) {
        QStringList command = { "stream", "host", "app" };
        command.append(options);
        ParserResult result = runParser(initialMode, command);
        QJsonObject output = QJsonDocument::fromJson(result.output).object();
        ++checks;
        if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0 ||
            output.value("mouseMode").toInt(-1) != expectedMode ||
            output.value("savedMouseMode").toInt(-1) != initialMode ||
            output.value("host").toString() != "host" || output.value("app").toString() != "app") {
            fprintf(stderr, "FAIL %s: exit %d, stdout %s, stderr %s\n", qPrintable(name),
                    result.exitCode, result.output.constData(), result.error.constData());
            ++failures;
        }
    };
    const auto expectError = [&](const QString& name, const QStringList& command,
                                 const QByteArray& expectedError) {
        ParserResult result = runParser(StreamingPreferences::MM_RELATIVE, command);
        ++checks;
        if (result.exitStatus != QProcess::NormalExit || result.exitCode != 1 ||
            !result.error.contains(expectedError)) {
            fprintf(stderr, "FAIL %s: exit %d, stderr %s\n", qPrintable(name), result.exitCode,
                    result.error.constData());
            ++failures;
        }
    };

    for (int initial = StreamingPreferences::MM_RELATIVE;
         initial <= StreamingPreferences::MM_ABSOLUTE_LOCAL; ++initial) {
        const QString suffix = QString(" (initial %1)").arg(initial);
        expectSuccess("preserve settings" + suffix, initial, {}, initial);
        expectSuccess("relative" + suffix, initial, { "--mouse-mode", "relative" },
                      StreamingPreferences::MM_RELATIVE);
        expectSuccess("absolute-video" + suffix, initial, { "--mouse-mode", "absolute-video" },
                      StreamingPreferences::MM_ABSOLUTE_VIDEO);
        expectSuccess("absolute-local" + suffix, initial, { "--mouse-mode", "absolute-local" },
                      StreamingPreferences::MM_ABSOLUTE_LOCAL);
        expectSuccess("absolute alias" + suffix, initial, { "--absolute-mouse" },
                      initial == StreamingPreferences::MM_RELATIVE
                          ? StreamingPreferences::MM_ABSOLUTE_LOCAL
                          : initial);
        expectSuccess("relative alias" + suffix, initial, { "--no-absolute-mouse" },
                      StreamingPreferences::MM_RELATIVE);
        expectSuccess("show cursor alias" + suffix, initial, { "--show-local-cursor" },
                      StreamingPreferences::MM_ABSOLUTE_LOCAL);
        expectSuccess("hide cursor alias" + suffix, initial, { "--no-show-local-cursor" },
                      initial == StreamingPreferences::MM_RELATIVE
                          ? StreamingPreferences::MM_RELATIVE
                          : StreamingPreferences::MM_ABSOLUTE_VIDEO);
        expectSuccess("absolute and show" + suffix, initial,
                      { "--absolute-mouse", "--show-local-cursor" },
                      StreamingPreferences::MM_ABSOLUTE_LOCAL);
        expectSuccess("show and absolute" + suffix, initial,
                      { "--show-local-cursor", "--absolute-mouse" },
                      StreamingPreferences::MM_ABSOLUTE_LOCAL);
        expectSuccess("absolute and hide" + suffix, initial,
                      { "--absolute-mouse", "--no-show-local-cursor" },
                      StreamingPreferences::MM_ABSOLUTE_VIDEO);
        expectSuccess("hide and absolute" + suffix, initial,
                      { "--no-show-local-cursor", "--absolute-mouse" },
                      StreamingPreferences::MM_ABSOLUTE_VIDEO);
        expectSuccess("relative and hide" + suffix, initial,
                      { "--no-absolute-mouse", "--no-show-local-cursor" },
                      StreamingPreferences::MM_RELATIVE);
        expectSuccess("hide and relative" + suffix, initial,
                      { "--no-show-local-cursor", "--no-absolute-mouse" },
                      StreamingPreferences::MM_RELATIVE);
    }
    expectSuccess("case-insensitive choice", StreamingPreferences::MM_RELATIVE,
                  { "--mouse-mode=ABSOLUTE-VIDEO" }, StreamingPreferences::MM_ABSOLUTE_VIDEO);
    expectSuccess("repeated alias", StreamingPreferences::MM_RELATIVE,
                  { "--absolute-mouse", "--absolute-mouse" },
                  StreamingPreferences::MM_ABSOLUTE_LOCAL);

    const QStringList legacyOptions = { "--absolute-mouse", "--no-absolute-mouse",
                                        "--show-local-cursor", "--no-show-local-cursor" };
    for (const QString& legacyOption : legacyOptions) {
        expectError("new and legacy " + legacyOption,
                    { "stream", "host", "app", "--mouse-mode", "relative", legacyOption },
                    "--mouse-mode cannot be combined");
        expectError("legacy and new " + legacyOption,
                    { "stream", "host", "app", legacyOption, "--mouse-mode", "relative" },
                    "--mouse-mode cannot be combined");
    }
    expectSuccess("absolute toggle last off", StreamingPreferences::MM_ABSOLUTE_LOCAL,
                  { "--absolute-mouse", "--no-absolute-mouse" }, StreamingPreferences::MM_RELATIVE);
    expectSuccess("absolute toggle last on", StreamingPreferences::MM_RELATIVE,
                  { "--no-absolute-mouse", "--absolute-mouse" },
                  StreamingPreferences::MM_ABSOLUTE_LOCAL);
    expectSuccess("cursor toggle last off", StreamingPreferences::MM_ABSOLUTE_LOCAL,
                  { "--show-local-cursor", "--no-show-local-cursor" },
                  StreamingPreferences::MM_ABSOLUTE_VIDEO);
    expectSuccess("cursor toggle last on", StreamingPreferences::MM_ABSOLUTE_VIDEO,
                  { "--no-show-local-cursor", "--show-local-cursor" },
                  StreamingPreferences::MM_ABSOLUTE_LOCAL);
    const QList<QPair<QString, QString>> conflicts = {
        { "--no-absolute-mouse", "--show-local-cursor" },
    };
    for (const auto& conflict : conflicts) {
        expectError("conflict " + conflict.first,
                    { "stream", "host", "app", conflict.first, conflict.second }, "conflicts with");
        expectError("reverse conflict " + conflict.first,
                    { "stream", "host", "app", conflict.second, conflict.first }, "conflicts with");
    }
    expectError("invalid mode", { "stream", "host", "app", "--mouse-mode", "invalid" },
                "Invalid mouse-mode choice");
    expectError("missing mode", { "stream", "host", "app", "--mouse-mode" }, "Missing value");
    expectError("unknown option", { "stream", "host", "app", "--unknown" }, "Unknown option");
    expectError("missing host", { "stream" }, "Host not provided");
    expectError("missing app", { "stream", "host" }, "App not provided");
    expectError("invalid bitrate", { "stream", "host", "app", "--bitrate", "100" },
                "Bitrate must be in range");

    qputenv("MOONLIGHT_CLI_TEST_UI_MODE",
            QByteArray::number(StreamingPreferences::MM_ABSOLUTE_VIDEO));
    ParserResult explicitUiChoice =
        runParser(StreamingPreferences::MM_RELATIVE,
                  { "stream", "host", "app", "--mouse-mode", "absolute-local" });
    qunsetenv("MOONLIGHT_CLI_TEST_UI_MODE");
    const QJsonObject explicitUiResult = QJsonDocument::fromJson(explicitUiChoice.output).object();
    ++checks;
    if (explicitUiChoice.exitStatus != QProcess::NormalExit || explicitUiChoice.exitCode != 0 ||
        explicitUiResult.value("mouseMode").toInt(-1) != StreamingPreferences::MM_ABSOLUTE_VIDEO ||
        explicitUiResult.value("savedMouseMode").toInt(-1) !=
            StreamingPreferences::MM_ABSOLUTE_VIDEO) {
        fprintf(stderr, "FAIL explicit settings choice: stdout %s, stderr %s\n",
                explicitUiChoice.output.constData(), explicitUiChoice.error.constData());
        ++failures;
    }

    ParserResult beforeAction =
        runParser(StreamingPreferences::MM_RELATIVE,
                  { "--mouse-mode", "absolute-video", "stream", "host", "app" });
    ++checks;
    if (beforeAction.exitStatus != QProcess::NormalExit || beforeAction.exitCode != 0 ||
        QJsonDocument::fromJson(beforeAction.output).object().value("mouseMode").toInt(-1) !=
            StreamingPreferences::MM_ABSOLUTE_VIDEO) {
        fprintf(stderr, "FAIL mouse mode before action: %s\n", beforeAction.error.constData());
        ++failures;
    }

    ParserResult otherOptions =
        runParser(StreamingPreferences::MM_ABSOLUTE_LOCAL,
                  { "stream", "host", "app", "--720", "--fps", "120", "--bitrate", "20000",
                    "--packet-size", "1200", "--no-vsync", "--vsync", "--no-vsync",
                    "--touchscreen-trackpad", "--mouse-buttons-swap", "--disable-ui-gamepad" });
    QJsonObject otherValues = QJsonDocument::fromJson(otherOptions.output).object();
    ++checks;
    if (otherOptions.exitStatus != QProcess::NormalExit || otherOptions.exitCode != 0 ||
        otherValues.value("mouseMode").toInt(-1) != StreamingPreferences::MM_ABSOLUTE_LOCAL ||
        otherValues.value("width").toInt() != 1280 || otherValues.value("height").toInt() != 720 ||
        otherValues.value("fps").toInt() != 120 || otherValues.value("bitrate").toInt() != 20000 ||
        otherValues.value("packetSize").toInt() != 1200 ||
        otherValues.value("vsync").toBool(true) ||
        otherValues.value("absoluteTouchMode").toBool(true) ||
        !otherValues.value("swapMouseButtons").toBool() ||
        otherValues.value("gamepadUiNavigation").toBool(true) ||
        !otherValues.value("globalGamepadOverride").toBool()) {
        fprintf(stderr, "FAIL existing options: stdout %s, stderr %s\n",
                otherOptions.output.constData(), otherOptions.error.constData());
        ++failures;
    }

    ParserResult help = runParser(StreamingPreferences::MM_RELATIVE, { "stream", "--help" });
    ++checks;
    if (help.exitStatus != QProcess::NormalExit || help.exitCode != 0 ||
        !help.output.contains("--mouse-mode") || !help.output.contains("--show-local-cursor") ||
        !help.output.contains("--no-show-local-cursor") ||
        !help.output.contains("Compatibility alias") ||
        !help.output.contains("For repeated legacy toggles the last value wins")) {
        fprintf(stderr, "FAIL help: stdout %s, stderr %s\n", help.output.constData(),
                help.error.constData());
        ++failures;
    }

    if (failures) {
        fprintf(stderr, "%d of %d CLI checks failed\n", failures, checks);
        return 1;
    }
    printf("%d CLI checks passed\n", checks);
    return 0;
}
