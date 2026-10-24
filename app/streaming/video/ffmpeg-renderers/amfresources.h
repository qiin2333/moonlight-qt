#pragma once

// The runtime must outlive every interface obtained from it, including caps and
// frame references. Keep the cleanup order in one place for probes and streams.
template <typename Runtime, typename ContextPtr, typename ComponentPtr, typename SurfacePtr,
          typename DataPtr, typename CapsPtr>
class AmfResources
{
public:
    AmfResources() = default;
    AmfResources(const AmfResources&) = delete;
    AmfResources& operator=(const AmfResources&) = delete;

    ~AmfResources() { reset(); }

    void reset()
    {
        initialized = false;
        caps = nullptr;
        data = nullptr;
        surface = nullptr;
        if (upScaler) {
            upScaler->Terminate();
            upScaler = nullptr;
        }
        if (context) {
            context->Terminate();
            context = nullptr;
        }
        runtime.unload();
    }

    Runtime runtime;
    ContextPtr context;
    ComponentPtr upScaler;
    SurfacePtr surface;
    DataPtr data;
    CapsPtr caps;
    bool initialized = false;
};
