#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "streaming/video/ffmpeg-renderers/amfsession.h"

#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

// Optional integration through the same public session API as the renderer.
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
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, nullptr, &context))) {
            return false;
        }
        for (bool downscale : { false, true }) {
            for (bool tenBit : { false, true }) {
                AmfSession::Parameters config;
                config.device = device.Get();
                config.width = downscale ? 1920 : 1280;
                config.height = downscale ? 1080 : 720;
                config.outputWidth = downscale ? 1280 : 1920;
                config.outputHeight = downscale ? 720 : 1080;
                config.tenBit = tenBit;
                AmfSession session;
                if (!session.probe(config) || session.isInitialized() ||
                    !session.initialize(config)) {
                    std::cerr << "hardware probe/activation failed" << std::endl;
                    return false;
                }
                D3D11_TEXTURE2D_DESC texture = {};
                texture.Width = config.width;
                texture.Height = config.height;
                texture.MipLevels = 1;
                texture.ArraySize = 1;
                texture.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
                texture.SampleDesc.Count = 1;
                texture.Usage = D3D11_USAGE_DEFAULT;
                texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                const UINT pitch = config.width * (tenBit ? 2 : 1);
                std::vector<unsigned char> pixels(pitch * config.height * 3 / 2, 0);
                D3D11_SUBRESOURCE_DATA initial = { pixels.data(), pitch, 0 };
                ComPtr<ID3D11Texture2D> input;
                ComPtr<ID3D11Texture2D> output;
                if (FAILED(device->CreateTexture2D(&texture, &initial, &input))) {
                    return false;
                }
                texture.Width = config.outputWidth;
                texture.Height = config.outputHeight;
                if (FAILED(device->CreateTexture2D(&texture, nullptr, &output))) {
                    return false;
                }
                for (double sharpness : { 2.0, 0.30 }) {
                    {
                        AmfSession probe;
                        if (!probe.probe(config)) {
                            return false;
                        }
                    }
                    if (!session.setSharpness(sharpness) ||
                        !session.render(input.Get(), output.Get(), context.Get())) {
                        std::cerr << "hardware reconfiguration/frame copy failed, tenBit=" << tenBit
                                  << std::endl;
                        return false;
                    }
                }
                // SDK sharpness is restricted to [0, 2]. Verify failure cleanup
                // through the production session API, without a separate VP pipeline.
                if (session.setSharpness(-1.0) || session.isInitialized()) {
                    std::cerr << "AMF reconfiguration failure cleanup failed, downscale="
                              << downscale << " tenBit=" << tenBit << std::endl;
                    return false;
                }
                session.reset();
                session.reset();
            }
        }
        ++adaptersTested;
    }
    std::cout << "AMF hardware adapters tested: " << adaptersTested << std::endl;
    return adaptersTested > 0;
}
