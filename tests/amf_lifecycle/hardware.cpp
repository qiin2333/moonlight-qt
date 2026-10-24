#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "streaming/video/ffmpeg-renderers/amfsession.h"

#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
bool testVideoProcessorFallback(ID3D11Device* device, ID3D11DeviceContext* context,
                                const AmfSession::Parameters& config, ID3D11Texture2D* source)
{
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext1> videoContext;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&videoDevice))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&videoContext)))) {
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = content.OutputFrameRate = { 60, 1 };
    content.InputWidth = config.width;
    content.InputHeight = config.height;
    content.OutputWidth = config.outputWidth;
    content.OutputHeight = config.outputHeight;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    if (FAILED(videoDevice->CreateVideoProcessorEnumerator(&content, &enumerator)) ||
        FAILED(videoDevice->CreateVideoProcessor(enumerator.Get(), 0, &processor))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    ComPtr<ID3D11Texture2D> input;
    ComPtr<ID3D11Texture2D> output;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &input))) {
        return false;
    }
    desc.Width = config.outputWidth;
    desc.Height = config.outputHeight;
    desc.Format = config.tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &output))) {
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11VideoProcessorInputView> inputView;
    ComPtr<ID3D11VideoProcessorOutputView> outputView;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
    outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateShaderResourceView(output.Get(), nullptr, &srv)) ||
        FAILED(videoDevice->CreateVideoProcessorInputView(input.Get(), enumerator.Get(), &inputDesc,
                                                          &inputView)) ||
        FAILED(videoDevice->CreateVideoProcessorOutputView(output.Get(), enumerator.Get(),
                                                           &outputDesc, &outputView))) {
        return false;
    }
    D3D11_BOX sourceBox = { 0, 0, 0, UINT(config.width), UINT(config.height), 1 };
    context->CopySubresourceRegion(input.Get(), 0, 0, 0, 0, source, 0, &sourceBox);
    RECT target = { 0, 0, config.outputWidth, config.outputHeight };
    videoContext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &target);
    videoContext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &target);
    videoContext->VideoProcessorSetStreamColorSpace1(processor.Get(), 0,
                                                     DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    videoContext->VideoProcessorSetOutputColorSpace1(processor.Get(),
                                                     DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    return SUCCEEDED(
        videoContext->VideoProcessorBlt(processor.Get(), outputView.Get(), 0, 1, &stream));
}
} // namespace

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
                // SDK sharpness is restricted to [0, 2]. Force a real reconfiguration
                // failure, then verify a fresh source-sized VP input and RGBA output.
                if (session.setSharpness(-1.0) || session.isInitialized() ||
                    !testVideoProcessorFallback(device.Get(), context.Get(), config, input.Get())) {
                    std::cerr << "AMF failure / VP fallback failed, downscale=" << downscale
                              << " tenBit=" << tenBit << std::endl;
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
