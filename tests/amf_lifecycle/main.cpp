#include "streaming/video/ffmpeg-renderers/amfsession.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <windows.h>

#include <iostream>
#include <type_traits>

bool testAmfHardware();

namespace {
int failures = 0;

void require(bool condition, const QString& message)
{
    if (!condition) {
        std::cerr << qPrintable(message) << std::endl;
        ++failures;
    }
}

AmfSession::Parameters parameters()
{
    AmfSession::Parameters result;
    // The fixture accepts an opaque D3D handle and never dereferences it.
    result.device = reinterpret_cast<ID3D11Device*>(1);
    result.width = 1280;
    result.height = 720;
    result.outputWidth = 1920;
    result.outputHeight = 1080;
    return result;
}

void setMode(const wchar_t* mode)
{
    SetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_MODE", mode);
}

QStringList events(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

void requireUnloaded(const QString& path)
{
    require(GetModuleHandleW(L"amf_runtime_fixture.dll") == nullptr, "session retained the DLL");
    const auto log = events(path);
    require(!log.isEmpty() && log.last() == "unload:0",
            "DLL unloaded with live interfaces: " + log.join(','));
}

void testFailures(const QString& library, const QString& log)
{
    for (const auto* stage : { L"init", L"nullfactory", L"context", L"nullcontext", L"dx11",
                               L"component", L"nullcomponent", L"caps", L"nullcaps", L"unsupported",
                               L"surface", L"nullsurface", L"property", L"scalerinit" }) {
        QFile::remove(log);
        setMode(stage);
        AmfSession session(library);
        require(!session.initialize(parameters()) && !session.isInitialized(),
                "initialization accepted failure at " + QString::fromWCharArray(stage));
        requireUnloaded(log);
        const auto before = events(log);
        session.reset();
        session.reset();
        require(events(log) == before, "repeated cleanup called released interfaces");
        require(!session.probe(parameters()), "probe accepted a failing SDK stage");
        requireUnloaded(log);
        setMode(nullptr);
        require(session.probe(parameters()),
                "session could not recover after initialization failure");
        require(!session.isInitialized(), "probe left an active session");
        requireUnloaded(log);
    }
    setMode(nullptr);
}

void testProbeAndFrames(const QString& library, const QString& log)
{
    QFile::remove(log);
    {
        AmfSession session(library);
        auto config = parameters();
        for (bool tenBit : { false, true }) {
            config.tenBit = tenBit;
            require(session.probe(config), "capability probe failed");
            require(!session.isInitialized(), "successful probe retained active state");
            requireUnloaded(log);
        }
        require(events(log).contains("nv12") && events(log).contains("p010"),
                "format selection was lost");
        require(session.initialize(config), "session activation failed");
        require(session.setSharpness(2.0), "sharpness reconfiguration failed");
        // The C fixture deliberately has no output plane: the session must reject
        // it before invoking D3D. Data/output references remain owned until reset.
        require(!session.render(reinterpret_cast<ID3D11Texture2D*>(1),
                                reinterpret_cast<ID3D11Texture2D*>(2),
                                reinterpret_cast<ID3D11DeviceContext*>(3)),
                "missing output plane was accepted");
    }
    requireUnloaded(log);
    const QStringList cleanup = { "caps.release",      "data.release",     "output.release",
                                  "surface.release",   "scaler.terminate", "scaler.release",
                                  "context.terminate", "context.release",  "unload:0" };
    const auto actual = events(log);
    require(actual.mid(actual.size() - cleanup.size()) == cleanup,
            "session destruction order changed");
}

void testIndependentSessions(const QString& library, const QString& log)
{
    QFile::remove(log);
    {
        AmfSession active(library);
        require(active.initialize(parameters()), "active session failed");
        {
            AmfSession unused(library);
        }
        {
            AmfSession probe(library);
            require(probe.probe(parameters()), "second session probe failed");
        }
        require(active.isInitialized() && active.setSharpness(2.0),
                "another session invalidated the active owner");
        require(!events(log).contains("unload:0"), "another session unloaded the active DLL");
        require(active.initialize(parameters()), "repeated activation failed");
    }
    requireUnloaded(log);
}

void testReconfigurationAndFrameFailures(const QString& library, const QString& log)
{
    for (const auto* stage : { L"flush", L"terminate", L"property", L"scalerinit" }) {
        QFile::remove(log);
        setMode(nullptr);
        AmfSession session(library);
        auto config = parameters();
        config.width = 1920;
        config.height = 1080;
        config.outputWidth = 1280;
        config.outputHeight = 720;
        require(session.initialize(config), "downscale session setup failed");
        setMode(stage);
        require(!session.setSharpness(2.0) && !session.isInitialized(),
                "failed reconfiguration retained an active session");
        requireUnloaded(log);
    }
    for (const auto* stage :
         { L"wrap", L"submit", L"query", L"nulloutput", L"flush", L"querysurface" }) {
        QFile::remove(log);
        setMode(nullptr);
        {
            AmfSession session(library);
            require(session.initialize(parameters()), "frame session setup failed");
            setMode(stage);
            require(!session.render(reinterpret_cast<ID3D11Texture2D*>(1),
                                    reinterpret_cast<ID3D11Texture2D*>(2),
                                    reinterpret_cast<ID3D11DeviceContext*>(3)),
                    "invalid frame output was accepted");
        }
        requireUnloaded(log);
    }
    setMode(nullptr);
}

void testInvalidInputs()
{
    for (const auto& name : { QStringLiteral("moonlight-missing-amf-runtime-for-test.dll"),
                              QStringLiteral("kernel32.dll") }) {
        AmfSession session(name);
        require(!session.initialize(parameters()), "invalid runtime was accepted");
        require(!session.probe(parameters()), "invalid runtime probe was accepted");
        session.reset();
    }
    AmfSession inactive;
    require(!inactive.setSharpness(2.0) && !inactive.render(nullptr, nullptr, nullptr),
            "inactive session accepted work");
    require(!inactive.initialize({}), "invalid parameters were accepted");
}
} // namespace

int main(int argc, char** argv)
{
    static_assert(!std::is_copy_constructible_v<AmfSession>);
    QCoreApplication app(argc, argv);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    QTemporaryDir directory;
    if (!directory.isValid()) {
        return 1;
    }
    const auto library = QCoreApplication::applicationDirPath() + "/amf_runtime_fixture.dll";
    const auto log = directory.filePath("lifecycle.log");
    SetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_LOG",
                            reinterpret_cast<const wchar_t*>(log.utf16()));
    testFailures(library, log);
    testProbeAndFrames(library, log);
    testIndependentSessions(library, log);
    testReconfigurationAndFrameFailures(library, log);
    testInvalidInputs();
    SetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_LOG", nullptr);
    if (app.arguments().contains(QStringLiteral("--hardware"))) {
        require(testAmfHardware(), "AMF hardware integration test failed");
    }
    std::cout << (failures ? "AMF session regression tests failed: "
                           : "AMF session regression tests passed: ")
              << failures << std::endl;
    return failures == 0 ? 0 : 1;
}
