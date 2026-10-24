#include "amfsession.h"

#include <d3d11.h>
#include <QLibrary>

#include "public/include/core/Factory.h"
#include "public/include/components/HQScaler.h"

class AmfSession::Impl
{
public:
    explicit Impl(const QString& libraryName)
        : m_Library(libraryName.isEmpty() ? QString::fromWCharArray(AMF_DLL_NAME) : libraryName),
          m_UseDefaultLibrary(libraryName.isEmpty())
    {
    }

    ~Impl() { reset(); }

    void reset()
    {
        m_Initialized = false;
        m_Caps = nullptr;
        m_Data = nullptr;
        m_OutputSurface = nullptr;
        m_Surface = nullptr;
        if (m_UpScaler) {
            m_UpScaler->Terminate();
            m_UpScaler = nullptr;
        }
        if (m_Context) {
            m_Context->Terminate();
            m_Context = nullptr;
        }
        m_Factory = nullptr;
        m_Parameters = {};
        if (m_Library.isLoaded()) {
            m_Library.unload();
        }
    }

    bool initialize(const Parameters& parameters)
    {
        m_Parameters = parameters;
        if (!parameters.device || parameters.width <= 0 || parameters.height <= 0 ||
            parameters.outputWidth <= 0 || parameters.outputHeight <= 0 ||
            parameters.frameRate <= 0) {
            return false;
        }
        if (m_UseDefaultLibrary) {
            m_Library.setFileName(QString::fromWCharArray(AMF_DLL_NAME));
        }
        if (!m_Library.load()) {
            if (!m_UseDefaultLibrary) {
                return false;
            }
            // Preserve the SDK helper's AMF Lite fallback.
            m_Library.setFileName(QString::fromWCharArray(AMFLITE_DLL_NAME));
            if (!m_Library.load()) {
                return false;
            }
        }
        const auto init = reinterpret_cast<AMFInit_Fn>(m_Library.resolve(AMF_INIT_FUNCTION_NAME));
        if (!init || init(AMF_FULL_VERSION, &m_Factory) != AMF_OK || !m_Factory) {
            return false;
        }

        if (m_Factory->CreateContext(&m_Context) != AMF_OK || !m_Context ||
            m_Context->InitDX11(parameters.device) != AMF_OK ||
            m_Factory->CreateComponent(m_Context, AMFHQScaler, &m_UpScaler) != AMF_OK ||
            !m_UpScaler || m_UpScaler->GetCaps(&m_Caps) != AMF_OK || !m_Caps ||
            m_Caps->GetAccelerationType() == amf::AMF_ACCEL_NOT_SUPPORTED ||
            m_Context->AllocSurface(amf::AMF_MEMORY_DX11, format(), parameters.width,
                                    parameters.height, &m_Surface) != AMF_OK ||
            !m_Surface) {
            return false;
        }

        if (m_UpScaler->SetProperty(
                AMF_HQ_SCALER_OUTPUT_SIZE,
                AMFConstructSize(parameters.outputWidth, parameters.outputHeight)) != AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_ENGINE_TYPE, amf::AMF_MEMORY_DX11) != AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_ALGORITHM, AMF_HQ_SCALER_ALGORITHM_VIDEOSR1_0) !=
                AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_KEEP_ASPECT_RATIO, true) != AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_FILL, true) != AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_FILL_COLOR, AMFConstructColor(0, 0, 0, 255)) !=
                AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_SHARPNESS, parameters.sharpness) != AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_FRAME_RATE, parameters.frameRate) != AMF_OK ||
            m_UpScaler->Init(format(), parameters.width, parameters.height) != AMF_OK) {
            return false;
        }
        m_Initialized = true;
        return true;
    }

    bool isInitialized() const { return m_Initialized; }

    bool setSharpness(double sharpness)
    {
        if (!m_Initialized) {
            return false;
        }
        m_Data = nullptr;
        m_OutputSurface = nullptr;
        m_Surface = nullptr;
        if (m_UpScaler->Flush() != AMF_OK || m_UpScaler->Terminate() != AMF_OK ||
            m_UpScaler->SetProperty(AMF_HQ_SCALER_SHARPNESS, sharpness) != AMF_OK ||
            m_UpScaler->Init(format(), m_Parameters.width, m_Parameters.height) != AMF_OK) {
            reset();
            return false;
        }
        m_Parameters.sharpness = sharpness;
        return true;
    }

    bool render(ID3D11Texture2D* input, ID3D11Texture2D* output, ID3D11DeviceContext* context)
    {
        if (!m_Initialized || !input || !output || !context) {
            return false;
        }
        m_Data = nullptr;
        m_OutputSurface = nullptr;
        if (m_Context->CreateSurfaceFromDX11Native(input, &m_Surface, nullptr) != AMF_OK ||
            !m_Surface || m_UpScaler->SubmitInput(m_Surface) != AMF_OK) {
            return false;
        }
        const auto result = m_UpScaler->QueryOutput(&m_Data);
        const auto flushResult = m_UpScaler->Flush();
        if (result != AMF_OK || !m_Data || flushResult != AMF_OK ||
            m_Data->QueryInterface(amf::AMFSurface::IID(),
                                   reinterpret_cast<void**>(&m_OutputSurface)) != AMF_OK ||
            !m_OutputSurface) {
            return false;
        }
        auto* plane = m_OutputSurface->GetPlaneAt(0);
        auto* texture = plane ? static_cast<ID3D11Texture2D*>(plane->GetNative()) : nullptr;
        if (!texture) {
            return false;
        }
        context->CopyResource(output, texture);
        return true;
    }

private:
    amf::AMF_SURFACE_FORMAT format() const
    {
        return m_Parameters.tenBit ? amf::AMF_SURFACE_P010 : amf::AMF_SURFACE_NV12;
    }

    QLibrary m_Library;
    bool m_UseDefaultLibrary;
    amf::AMFFactory* m_Factory = nullptr;
    amf::AMFContextPtr m_Context;
    amf::AMFComponentPtr m_UpScaler;
    amf::AMFSurfacePtr m_Surface;
    amf::AMFSurfacePtr m_OutputSurface;
    amf::AMFDataPtr m_Data;
    amf::AMFCapsPtr m_Caps;
    Parameters m_Parameters;
    bool m_Initialized = false;
};

AmfSession::AmfSession(const QString& libraryName) : m_Impl(std::make_unique<Impl>(libraryName)) {}
AmfSession::~AmfSession() = default;

bool AmfSession::initialize(const Parameters& parameters)
{
    reset();
    if (!m_Impl->initialize(parameters)) {
        reset();
        return false;
    }
    return true;
}

bool AmfSession::probe(const Parameters& parameters)
{
    const bool supported = initialize(parameters);
    reset();
    return supported;
}

bool AmfSession::isInitialized() const
{
    return m_Impl->isInitialized();
}
bool AmfSession::setSharpness(double sharpness)
{
    return m_Impl->setSharpness(sharpness);
}
bool AmfSession::render(ID3D11Texture2D* input, ID3D11Texture2D* output,
                        ID3D11DeviceContext* context)
{
    return m_Impl->render(input, output, context);
}
void AmfSession::reset()
{
    m_Impl->reset();
}
