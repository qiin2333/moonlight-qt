#pragma once

#include <QLibrary>

#include "public/include/core/Factory.h"

// Each renderer owns one loader reference. In particular, destroying a renderer
// that never used AMF must not release another renderer's runtime reference.
class AmfRuntime
{
public:
    explicit AmfRuntime(const QString& libraryName = QString::fromWCharArray(AMF_DLL_NAME))
        : m_Library(libraryName)
    {
    }

    ~AmfRuntime() { unload(); }

    bool load()
    {
        if (m_Factory) {
            return true;
        }

        if (!m_Library.load()) {
            // Preserve the SDK helper's AMF Lite fallback.
            if (m_Library.fileName() != QString::fromWCharArray(AMF_DLL_NAME)) {
                return false;
            }
            m_Library.setFileName(QString::fromWCharArray(AMFLITE_DLL_NAME));
            if (!m_Library.load()) {
                return false;
            }
        }

        auto init = reinterpret_cast<AMFInit_Fn>(m_Library.resolve(AMF_INIT_FUNCTION_NAME));
        if (!init || init(AMF_FULL_VERSION, &m_Factory) != AMF_OK || !m_Factory) {
            unload();
            return false;
        }

        return true;
    }

    void unload()
    {
        m_Factory = nullptr;
        if (m_Library.isLoaded()) {
            m_Library.unload();
        }
    }

    amf::AMFFactory* factory() const { return m_Factory; }

private:
    QLibrary m_Library;
    amf::AMFFactory* m_Factory = nullptr;
};
