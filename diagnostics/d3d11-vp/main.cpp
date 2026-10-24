#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

struct ScaleConfig
{
    int width;
    int height;
    int outputWidth;
    int outputHeight;
    bool tenBit;
};

namespace {
bool probeVideoProcessorScaling(ID3D11Device* device, ID3D11DeviceContext* context,
                                const ScaleConfig& config, ID3D11Texture2D* source)
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

int main()
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::cerr << "CreateDXGIFactory1 failed\n";
        return 1;
    }
    int checked = 0;
    bool passed = true;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == S_OK;
         ++index) {
        DXGI_ADAPTER_DESC1 description = {};
        if (FAILED(adapter->GetDesc1(&description)) || description.VendorId != 0x1002 ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &device, nullptr, &context))) {
            std::cerr << "Adapter " << index << ": D3D11 device creation failed\n";
            passed = false;
            continue;
        }
        ++checked;
        for (bool downscale : { false, true }) {
            for (bool tenBit : { false, true }) {
                ScaleConfig config = { downscale ? 1920 : 1280, downscale ? 1080 : 720,
                                       downscale ? 1280 : 1920, downscale ? 720 : 1080, tenBit };
                D3D11_TEXTURE2D_DESC description = {};
                description.Width = config.width;
                description.Height = config.height;
                description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
                description.Format = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
                description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                const UINT pitch = config.width * (tenBit ? 2 : 1);
                std::vector<unsigned char> pixels(pitch * config.height * 3 / 2, 0);
                D3D11_SUBRESOURCE_DATA initial = { pixels.data(), pitch, 0 };
                ComPtr<ID3D11Texture2D> input;
                const bool supported =
                    SUCCEEDED(device->CreateTexture2D(&description, &initial, &input)) &&
                    probeVideoProcessorScaling(device.Get(), context.Get(), config, input.Get());
                std::cout << "Adapter " << index << ": " << (tenBit ? "P010" : "NV12") << ' '
                          << config.width << 'x' << config.height << " -> " << config.outputWidth
                          << 'x' << config.outputHeight << (supported ? " PASS\n" : " FAIL\n");
                passed = passed && supported;
            }
        }
    }
    if (checked == 0) {
        std::cerr << "No usable AMD D3D11 adapter found\n";
        return 2;
    }
    return passed ? 0 : 1;
}
