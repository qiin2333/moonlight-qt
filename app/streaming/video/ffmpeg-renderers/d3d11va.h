#pragma once

#include "renderer.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <CGuid.h>
#include <atlbase.h>
#include "streaming/video/videoenhancement.h"
#include "public/common/AMFFactory.h"

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

class D3D11VARenderer : public IFFmpegRenderer
{
public:
    D3D11VARenderer(int decoderSelectionPass);
    virtual ~D3D11VARenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override;
    virtual bool prepareDecoderContextInGetFormat(AVCodecContext* context, AVPixelFormat pixelFormat) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO stateInfo) override;
    virtual int getRendererAttributes() override;
    virtual int getDecoderCapabilities() override;
    virtual bool needsTestFrame();
    virtual void setHdrMode(bool enabled) override;
    virtual InitFailureReason getInitFailureReason() override;

    enum PixelShaders {
        GENERIC_YUV_420,
        GENERIC_AYUV,
        GENERIC_Y410,
        _COUNT
    };

private:
    static void lockContext(void* lock_ctx);
    static void unlockContext(void* lock_ctx);

    bool setupRenderingResources();
    std::vector<DXGI_FORMAT> getVideoTextureSRVFormats();
    bool setupFrameRenderingResources(AVHWFramesContext* framesContext);
    bool setupSwapchainDependentResources();
    bool setupVideoTexture(AVHWFramesContext* framesContext); // for !m_BindDecoderOutputTextures
    bool setupTexturePoolViews(AVHWFramesContext* framesContext); // for m_BindDecoderOutputTextures
    void renderOverlay(Overlay::OverlayType type);
    bool createOverlayVertexBuffer(Overlay::OverlayType type, int width, int height, Microsoft::WRL::ComPtr<ID3D11Buffer>& newVertexBuffer);
    void bindColorConversion(bool frameChanged, AVFrame* frame);
    void bindVideoVertexBuffer(bool frameChanged, AVFrame* frame);
    void renderVideo(AVFrame* frame);
    bool createVideoProcessor(ID3D11Device* device = nullptr, ID3D11DeviceContext* context = nullptr);
    bool initializeVideoProcessor();
    bool enableAMDVideoSuperResolution(bool activate = true, bool logInfo = true, ID3D11Device* device = nullptr);
    bool enableIntelVideoSuperResolution(bool activate = true, bool logInfo = true);
    bool enableNvidiaVideoSuperResolution(bool activate = true, bool logInfo = true);
    bool enableAMDHDR(bool activate = true, bool logInfo = true);
    bool enableIntelHDR(bool activate = true, bool logInfo = true);
    bool enableNvidiaHDR(bool activate = true, bool logInfo = true);
    void prepareEnhancedOutput(AVFrame* frame);
    bool setupAmfTexture();
    bool setupEnhancedTexture();
    int getAdapterIndexByEnhancementCapabilities();
    bool checkDecoderSupport(IDXGIAdapter* adapter);
    bool createDeviceByAdapterIndex(int adapterIndex, bool* adapterNotFound = nullptr);
    bool setupSharedDevice(IDXGIAdapter1* adapter);
    bool createSharedFencePair(UINT64 initialValue,
                               ID3D11Device5* dev1, ID3D11Device5* dev2,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev1Fence,
                               Microsoft::WRL::ComPtr<ID3D11Fence>& dev2Fence);

    int m_DecoderSelectionPass;
    int m_DevicesWithFL11Support;
    int m_DevicesWithCodecSupport;

    enum class SupportedFenceType {
        None,
        NonMonitored,
        Monitored,
    };

    Microsoft::WRL::ComPtr<IDXGIFactory5> m_Factory;
    int m_AdapterIndex;
    Microsoft::WRL::ComPtr<ID3D11Device5> m_RenderDevice, m_DecodeDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_RenderDeviceContext, m_DecodeDeviceContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_RenderSharedTextureArray;
    Microsoft::WRL::ComPtr<IDXGISwapChain4> m_SwapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_VideoBlendState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_OverlayBlendState;

    SupportedFenceType m_FenceType;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeD2RFence, m_RenderD2RFence;
    UINT64 m_D2RFenceValue;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_DecodeR2DFence, m_RenderR2DFence;
    UINT64 m_R2DFenceValue;
    SDL_mutex* m_ContextLock;
    bool m_BindDecoderOutputTextures;
    bool m_UseFenceHack;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_VideoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext2> m_VideoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_VideoProcessor;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_VideoProcessorEnumerator;
    D3D11_VIDEO_PROCESSOR_CAPS m_VideoProcessorCapabilities;
    D3D11_VIDEO_PROCESSOR_STREAM m_StreamData;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_OutputView;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_InputView;
    Microsoft::WRL::ComPtr<ID3D11Resource> m_BackBufferResource;
    VideoEnhancement* m_VideoEnhancement;
    bool m_AutoStreamSuperResolution = false;

    DECODER_PARAMETERS m_DecoderParams;
    DXGI_FORMAT m_TextureFormat;
    int m_DisplayWidth;
    int m_DisplayHeight;
    int m_OutputIndex;
    AVColorTransferCharacteristic m_LastColorTrc;
    int m_LastColorSpace;
    bool m_LastFullRange;
    int m_LastEnhColorTrc;

    struct {
        int width;
        int height;
        int left;
        int top;
    } m_OutputTexture;
    D3D11_BOX m_SrcBox;

    bool m_AllowTearing;

    std::array<Microsoft::WRL::ComPtr<ID3D11PixelShader>, PixelShaders::_COUNT> m_VideoPixelShaders;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_VideoVertexBuffer;

    // Only valid if !m_BindDecoderOutputTextures
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_VideoTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_AmfTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_EnhancedTexture;

    // Only index 0 is valid if !m_BindDecoderOutputTextures
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2>> m_VideoTextureResourceViews;

    SDL_SpinLock m_OverlayLock;
    std::array<Microsoft::WRL::ComPtr<ID3D11Buffer>, Overlay::OverlayMax> m_OverlayVertexBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, Overlay::OverlayMax> m_OverlayTextures;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, Overlay::OverlayMax> m_OverlayTextureResourceViews;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_OverlayPixelShader;

    AVBufferRef* m_HwDeviceContext;

    // AMD (AMF)
    amf::AMFContextPtr m_AmfContext;
    amf::AMFSurfacePtr m_AmfSurface;
    amf::AMFDataPtr m_AmfData;
    amf::AMFComponent* m_AmfUpScaler;
    bool m_AmfInitialized = false;
    bool m_AmfUpScalerSharpness = false;
    amf::AMF_SURFACE_FORMAT m_AmfUpScalerSurfaceFormat;
};

