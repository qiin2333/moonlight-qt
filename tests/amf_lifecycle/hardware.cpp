#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "streaming/video/ffmpeg-renderers/amfresources.h"
#include "streaming/video/ffmpeg-renderers/amfruntime.h"
#include "public/include/components/HQScaler.h"

#include <iostream>

using Microsoft::WRL::ComPtr;
using Resources = AmfResources<AmfRuntime, amf::AMFContextPtr, amf::AMFComponentPtr,
                               amf::AMFSurfacePtr, amf::AMFDataPtr, amf::AMFCapsPtr>;

// Optional integration with the installed AMD driver; CI uses deterministic
// lifecycle tests. The D3D device deliberately outlives every AMF resource.
bool testAmfHardware()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    int adaptersTested = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == S_OK;
         ++index) {
        DXGI_ADAPTER_DESC1 desc;
        if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x1002) {
            continue;
        }

        ComPtr<ID3D11Device> device;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
            return false;
        }

        for (int cycle = 0; cycle < 5; ++cycle) {
            const auto format = cycle >= 3 ? amf::AMF_SURFACE_P010 : amf::AMF_SURFACE_NV12;
            Resources resources;
            if (!resources.runtime.load()) {
                return false;
            }
            auto* amfFactory = resources.runtime.factory();
            if (amfFactory->CreateContext(&resources.context) != AMF_OK ||
                resources.context->InitDX11(device.Get()) != AMF_OK) {
                return false;
            }
            // Real failure after partial initialization must unload cleanly.
            if (cycle == 0) {
                if (amfFactory->CreateComponent(resources.context, L"MoonlightInvalidComponent",
                                                &resources.upScaler) == AMF_OK) {
                    return false;
                }
                resources.reset();
                resources.reset();
                continue;
            }

            if (amfFactory->CreateComponent(resources.context, AMFHQScaler, &resources.upScaler) !=
                    AMF_OK ||
                resources.upScaler->GetCaps(&resources.caps) != AMF_OK || !resources.caps ||
                resources.caps->GetAccelerationType() == amf::AMF_ACCEL_NOT_SUPPORTED ||
                resources.context->AllocSurface(amf::AMF_MEMORY_DX11, format, 1280, 720,
                                                &resources.surface) != AMF_OK) {
                return false;
            }
            if (resources.upScaler->SetProperty(AMF_HQ_SCALER_OUTPUT_SIZE,
                                                AMFConstructSize(1920, 1080)) != AMF_OK ||
                resources.upScaler->SetProperty(AMF_HQ_SCALER_ENGINE_TYPE, amf::AMF_MEMORY_DX11) !=
                    AMF_OK ||
                resources.upScaler->SetProperty(AMF_HQ_SCALER_ALGORITHM,
                                                AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_0) != AMF_OK ||
                resources.upScaler->Init(format, 1280, 720) != AMF_OK) {
                return false;
            }
            if (resources.surface->QueryInterface(
                    amf::AMFData::IID(), reinterpret_cast<void**>(&resources.data)) != AMF_OK) {
                return false;
            }
            if (cycle % 2 == 1) {
                // Successful capability probe: caps are still live at reset.
                resources.reset();
                resources.reset();
            } else {
                resources.initialized = true;
                // Active-stream resources are released by the destructor.
            }
        }
        ++adaptersTested;
    }
    std::cout << "AMF hardware adapters tested: " << adaptersTested << std::endl;
    return adaptersTested > 0;
}
