#pragma once

#include <memory>

#include <QtGlobal>

#include <Limelight.h>

// Platforms where the authored PCM can actually be rendered to a controller:
// WASAPI on Windows, CoreAudio on macOS. Elsewhere the renderer is a stub and
// only the analyzed IR haptics path is offered.
#if defined(Q_OS_WIN32) || defined(Q_OS_MACOS)
#define HAVE_PHYSICAL_DS5_HAPTICS
#endif

class DualSenseHapticsRenderer
{
public:
    DualSenseHapticsRenderer();
    ~DualSenseHapticsRenderer();

    DualSenseHapticsRenderer(const DualSenseHapticsRenderer&) = delete;
    DualSenseHapticsRenderer& operator=(const DualSenseHapticsRenderer&) = delete;

    static bool isAvailable();
    void submit(const LI_DS5_HAPTICS_PCM_FRAME& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
