#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "public/include/core/Factory.h"

// Exercise the SDK's published C ABI through the production C++ session. Only
// methods used by the session are implemented; no private session access is needed.
static int liveObjects;
static AMFContextVtbl contextVtbl;
static AMFComponentVtbl scalerVtbl;
static AMFCapsVtbl capsVtbl;
static AMFSurfaceVtbl surfaceVtbl;
static AMFDataVtbl dataVtbl;

static void trace(const char* event)
{
    wchar_t path[32768];
    if (!GetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_LOG", path, 32768)) {
        return;
    }
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, event, (DWORD)strlen(event), &written, NULL);
        WriteFile(file, "\n", 1, &written, NULL);
        CloseHandle(file);
    }
}

static int failing(const wchar_t* stage)
{
    wchar_t mode[64] = { 0 };
    GetEnvironmentVariableW(L"MOONLIGHT_AMF_FIXTURE_MODE", mode, 64);
    return wcscmp(mode, stage) == 0;
}

#define DEFINE_OBJECT(type, name)                                                                  \
    typedef struct                                                                                 \
    {                                                                                              \
        type base;                                                                                 \
        amf_long references;                                                                       \
        const char* label;                                                                         \
    } name##Object;                                                                                \
    static type* name##Create(const char* label)                                                   \
    {                                                                                              \
        name##Object* object = calloc(1, sizeof(*object));                                         \
        object->base.pVtbl = &name##Vtbl;                                                          \
        object->references = 1;                                                                    \
        object->label = label;                                                                     \
        ++liveObjects;                                                                             \
        return &object->base;                                                                      \
    }                                                                                              \
    static amf_long AMF_STD_CALL name##Acquire(type* self)                                         \
    {                                                                                              \
        return ++((name##Object*)self)->references;                                                \
    }                                                                                              \
    static amf_long AMF_STD_CALL name##Release(type* self)                                         \
    {                                                                                              \
        name##Object* object = (name##Object*)self;                                                \
        amf_long references = --object->references;                                                \
        if (!references) {                                                                         \
            char event[64];                                                                        \
            snprintf(event, sizeof(event), "%s.release", object->label);                           \
            trace(event);                                                                          \
            --liveObjects;                                                                         \
            free(object);                                                                          \
        }                                                                                          \
        return references;                                                                         \
    }

DEFINE_OBJECT(AMFContext, context)
DEFINE_OBJECT(AMFComponent, scaler)
DEFINE_OBJECT(AMFCaps, caps)
DEFINE_OBJECT(AMFSurface, surface)
DEFINE_OBJECT(AMFData, data)

static AMF_RESULT AMF_STD_CALL createContext(AMFFactory* self, AMFContext** result)
{
    *result = failing(L"nullcontext") ? NULL : contextCreate("context");
    return failing(L"context") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL initDX11(AMFContext* self, void* device, AMF_DX_VERSION version)
{
    trace("dx11");
    return failing(L"dx11") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL terminateContext(AMFContext* self)
{
    trace("context.terminate");
    return AMF_OK;
}

static AMF_RESULT AMF_STD_CALL createComponent(AMFFactory* self, AMFContext* context,
                                               const wchar_t* id, AMFComponent** result)
{
    *result = failing(L"nullcomponent") ? NULL : scalerCreate("scaler");
    return failing(L"component") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL getCaps(AMFComponent* self, AMFCaps** result)
{
    *result = failing(L"nullcaps") ? NULL : capsCreate("caps");
    return failing(L"caps") ? AMF_FAIL : AMF_OK;
}

static AMF_ACCELERATION_TYPE AMF_STD_CALL acceleration(AMFCaps* self)
{
    return failing(L"unsupported") ? AMF_ACCEL_NOT_SUPPORTED : AMF_ACCEL_HARDWARE;
}

static AMF_RESULT AMF_STD_CALL allocSurface(AMFContext* self, AMF_MEMORY_TYPE memory,
                                            AMF_SURFACE_FORMAT format, amf_int32 width,
                                            amf_int32 height, AMFSurface** result)
{
    trace(format == AMF_SURFACE_P010 ? "p010" : "nv12");
    *result = failing(L"nullsurface") ? NULL : surfaceCreate("surface");
    return failing(L"surface") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL setProperty(AMFComponent* self, const wchar_t* name,
                                           AMFVariantStruct value)
{
    trace("property");
    return failing(L"property") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL initScaler(AMFComponent* self, AMF_SURFACE_FORMAT format,
                                          amf_int32 width, amf_int32 height)
{
    trace("scaler.init");
    return failing(L"scalerinit") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL terminateScaler(AMFComponent* self)
{
    trace("scaler.terminate");
    return failing(L"terminate") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL flush(AMFComponent* self)
{
    trace("flush");
    return failing(L"flush") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL wrapSurface(AMFContext* self, void* texture, AMFSurface** result,
                                           AMFSurfaceObserver* observer)
{
    *result = surfaceCreate("surface");
    return failing(L"wrap") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL submit(AMFComponent* self, AMFData* input)
{
    trace("submit");
    return failing(L"submit") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL queryOutput(AMFComponent* self, AMFData** result)
{
    *result = failing(L"nulloutput") ? NULL : dataCreate("data");
    return failing(L"query") ? AMF_FAIL : AMF_OK;
}

static AMF_RESULT AMF_STD_CALL querySurface(AMFData* self, const AMFGuid* id, void** result)
{
    *result = surfaceCreate("output");
    return failing(L"querysurface") ? AMF_FAIL : AMF_OK;
}

static AMFPlane* AMF_STD_CALL getPlane(AMFSurface* self, amf_size index)
{
    trace("plane");
    return NULL; // Stop before D3D CopyResource; hardware tests cover actual copying.
}

static AMFFactoryVtbl factoryVtbl = { .CreateContext = createContext,
                                      .CreateComponent = createComponent };
static AMFFactory factory = { &factoryVtbl };

__declspec(dllexport) AMF_RESULT AMF_CDECL_CALL AMFInit(amf_uint64 version, AMFFactory** result)
{
    contextVtbl.Acquire = contextAcquire;
    contextVtbl.Release = contextRelease;
    contextVtbl.InitDX11 = initDX11;
    contextVtbl.Terminate = terminateContext;
    contextVtbl.AllocSurface = allocSurface;
    contextVtbl.CreateSurfaceFromDX11Native = wrapSurface;
    scalerVtbl.Acquire = scalerAcquire;
    scalerVtbl.Release = scalerRelease;
    scalerVtbl.SetProperty = setProperty;
    scalerVtbl.Init = initScaler;
    scalerVtbl.Terminate = terminateScaler;
    scalerVtbl.Flush = flush;
    scalerVtbl.GetCaps = getCaps;
    scalerVtbl.SubmitInput = submit;
    scalerVtbl.QueryOutput = queryOutput;
    capsVtbl.Acquire = capsAcquire;
    capsVtbl.Release = capsRelease;
    capsVtbl.GetAccelerationType = acceleration;
    surfaceVtbl.Acquire = surfaceAcquire;
    surfaceVtbl.Release = surfaceRelease;
    surfaceVtbl.GetPlaneAt = getPlane;
    dataVtbl.Acquire = dataAcquire;
    dataVtbl.Release = dataRelease;
    dataVtbl.QueryInterface = querySurface;
    trace("init");
    *result = failing(L"nullfactory") ? NULL : &factory;
    return failing(L"init") ? AMF_FAIL : AMF_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH) {
        char event[64];
        snprintf(event, sizeof(event), "unload:%d", liveObjects);
        trace(event);
    }
    return TRUE;
}
