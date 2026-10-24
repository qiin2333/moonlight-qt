#include "streaming/video/ffmpeg-renderers/amfresources.h"
#include "streaming/video/ffmpeg-renderers/amfruntime.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

#include <memory>
#include <iostream>

#ifdef Q_OS_WIN
#include <windows.h>
bool testAmfHardware();
#endif

namespace {
int failures = 0;

void require(bool condition, const QString& message)
{
    if (!condition) {
        qCritical().noquote() << message;
        ++failures;
    }
}

struct State
{
    int loaderReferences = 0;
    int liveObjects = 0;
    QStringList events;
};

struct Runtime
{
    std::shared_ptr<State> state = std::make_shared<State>();
    bool loaded = false;

    void load()
    {
        if (!loaded) {
            loaded = true;
            ++state->loaderReferences;
            state->events << "load";
        }
    }

    void unload()
    {
        if (loaded) {
            loaded = false;
            if (--state->loaderReferences == 0) {
                require(state->liveObjects == 0, "DLL unloaded with live AMF interfaces");
            }
            state->events << "unload";
        }
    }
};

// Use the real SDK smart pointer so its operator& and Release behavior are
// exercised; the interfaces check that every call precedes DLL unloading.
class Interface
{
public:
    Interface(State& state, const QString& name) : m_State(state), m_Name(name)
    {
        ++m_State.liveObjects;
    }

    amf_long Acquire()
    {
        checkLoaded();
        return ++m_References;
    }

    amf_long Release()
    {
        checkLoaded();
        m_State.events << m_Name + ".release";
        const auto references = --m_References;
        if (references == 0) {
            --m_State.liveObjects;
            delete this;
        }
        return references;
    }

    void Terminate()
    {
        checkLoaded();
        m_State.events << m_Name + ".terminate";
    }

private:
    void checkLoaded()
    {
        require(m_State.loaderReferences > 0, m_Name + " called after DLL unload");
    }

    State& m_State;
    QString m_Name;
    amf_long m_References = 1;
};

using Ptr = amf::AMFInterfacePtr_T<Interface>;
using Resources = AmfResources<Runtime, Ptr, Ptr, Ptr, Ptr, Ptr>;

void populate(Resources& resources, int stage)
{
    resources.runtime.load();
    auto& state = *resources.runtime.state;
    Ptr* interfaceSlots[] = { std::addressof(resources.context), std::addressof(resources.upScaler),
                              std::addressof(resources.caps), std::addressof(resources.surface),
                              std::addressof(resources.data) };
    const char* names[] = { "context", "scaler", "caps", "surface", "data" };
    for (int i = 0; i < stage; ++i) {
        Interface** slot = &(*interfaceSlots[i]);
        *slot = new Interface(state, QString::fromLatin1(names[i]));
    }
}

void testPartialInitialization()
{
    // Failure after runtime load, context/component creation, GetCaps,
    // AllocSurface, or after obtaining an output frame.
    for (int stage = 0; stage <= 5; ++stage) {
        Resources resources;
        populate(resources, stage);
        resources.reset();
        auto& state = *resources.runtime.state;
        require(state.liveObjects == 0, QString("failure stage %1 leaked interfaces").arg(stage));
        require(state.loaderReferences == 0, "failure leaked a runtime reference");
        require(!resources.initialized, "failure retained initialized state");
        const auto events = state.events;
        resources.reset();
        require(state.events == events, "cleanup was not idempotent");
    }
}

void testProbeAndActiveDestruction()
{
    for (bool activate : { false, true }) {
        std::shared_ptr<State> state;
        {
            Resources resources;
            populate(resources, 5);
            state = resources.runtime.state;
            if (activate) {
                resources.initialized = true;
            } else {
                resources.reset();
                require(state->liveObjects == 0, "successful probe retained interfaces");
                // Multiple adapters are probed before activating a stream.
                populate(resources, 5);
                resources.reset();
                populate(resources, 5);
                resources.initialized = true;
            }
        }
        require(state->liveObjects == 0 && state->loaderReferences == 0,
                "destructor leaked AMF state");
        const QStringList cleanup = { "caps.release",     "data.release",   "surface.release",
                                      "scaler.terminate", "scaler.release", "context.terminate",
                                      "context.release",  "unload" };
        require(state->events.mid(state->events.size() - cleanup.size()) == cleanup,
                "AMF interfaces were destroyed in the wrong order");
    }
}

void testIndependentRenderers()
{
    Resources active;
    populate(active, 5);
    auto& state = *active.runtime.state;
    {
        Resources unused;
        unused.runtime.state = active.runtime.state;
    }
    require(state.loaderReferences == 1, "unused renderer unloaded active runtime");
    {
        Resources probe;
        probe.runtime.state = active.runtime.state;
        populate(probe, 5);
    }
    require(state.loaderReferences == 1 && state.liveObjects == 5,
            "probe destroyed another renderer's runtime or objects");
    active.reset();
}

void testRuntimeLoadFailures()
{
    AmfRuntime missing(QStringLiteral("moonlight-missing-amf-runtime-for-test.dll"));
    require(!missing.load() && !missing.factory(), "missing DLL was accepted");
    missing.unload();
    missing.unload();
#ifdef Q_OS_WIN
    AmfRuntime noEntryPoint(QStringLiteral("kernel32.dll"));
    require(!noEntryPoint.load() && !noEntryPoint.factory(), "DLL without AMFInit was accepted");
    require(!noEntryPoint.load(), "missing-entry-point retry retained invalid factory");
    noEntryPoint.unload();
#endif
}

#ifdef Q_OS_WIN
void testRuntimeOwnership()
{
    const auto fileName = QCoreApplication::applicationDirPath() + "/amf_runtime_fixture.dll";
    const auto loaded = [] {
        return GetModuleHandleW(L"amf_runtime_fixture.dll") != nullptr;
    };
    for (const auto* mode : { L"fail", L"null" }) {
        SetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_MODE", mode);
        AmfRuntime runtime(fileName);
        require(!runtime.load() && !runtime.factory(), "invalid AMFInit result was accepted");
        require(!loaded(), "failed AMFInit leaked its DLL reference");
        require(!runtime.load() && !loaded(), "AMFInit failure retry leaked its DLL reference");
    }
    SetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_MODE", nullptr);
    {
        AmfRuntime active(fileName);
        require(active.load() && active.factory(), "fixture runtime failed to load");
        require(active.load(), "repeated runtime load failed");
        {
            AmfRuntime unused(fileName);
        }
        require(loaded(), "unused runtime unloaded another owner's DLL");
        {
            AmfRuntime probe(fileName);
            require(probe.load(), "second runtime owner failed to load");
        }
        require(loaded(), "probe unloaded another owner's DLL");
        if (active.factory()) {
            require(QString::fromWCharArray(active.factory()->GetCacheFolder()) == "fixture",
                    "surviving runtime factory is invalid");
        }
        active.unload();
        require(!loaded(), "repeated load acquired an extra DLL reference");
        require(active.load(), "runtime reload after cleanup failed");
    }
    require(!loaded(), "runtime destructor leaked its DLL reference");
}
#endif
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testPartialInitialization();
    testProbeAndActiveDestruction();
    testIndependentRenderers();
    testRuntimeLoadFailures();
#ifdef Q_OS_WIN
    testRuntimeOwnership();
    if (app.arguments().contains(QStringLiteral("--hardware"))) {
        require(testAmfHardware(), "AMF hardware integration test failed");
    }
#endif
    if (failures == 0) {
        std::cout << "AMF lifecycle regression tests passed" << std::endl;
    } else {
        std::cerr << failures << " AMF lifecycle checks failed" << std::endl;
    }
    return failures == 0 ? 0 : 1;
}
