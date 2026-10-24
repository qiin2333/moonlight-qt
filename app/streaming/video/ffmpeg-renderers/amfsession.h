#pragma once

#include <QString>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

// Owns all AMF interfaces and the DLL reference that makes them callable.
// Callers can process frames, but cannot retain interfaces or unload the DLL.
class AmfSession
{
public:
    struct Parameters
    {
        ID3D11Device* device = nullptr;
        int width = 0;
        int height = 0;
        int outputWidth = 0;
        int outputHeight = 0;
        int frameRate = 60;
        bool tenBit = false;
        double sharpness = 0.30;
    };

    explicit AmfSession(const QString& libraryName = {});
    ~AmfSession();
    AmfSession(const AmfSession&) = delete;
    AmfSession& operator=(const AmfSession&) = delete;

    bool probe(const Parameters& parameters);
    bool initialize(const Parameters& parameters);
    bool isInitialized() const;
    bool setSharpness(double sharpness);
    bool render(ID3D11Texture2D* input, ID3D11Texture2D* output, ID3D11DeviceContext* context);
    void reset();

private:
    class Impl;
    std::unique_ptr<Impl> m_Impl;
};
