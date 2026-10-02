#pragma once

#include <memory>

#include <Limelight.h>

class DualSenseHapticsRenderer
{
public:
    enum class Mode
    {
        Physical, // Authored PCM on the controller's USB audio endpoint
        Emulated, // Analyzed IR frames
    };

    explicit DualSenseHapticsRenderer(Mode mode);
    ~DualSenseHapticsRenderer();

    DualSenseHapticsRenderer(const DualSenseHapticsRenderer&) = delete;
    DualSenseHapticsRenderer& operator=(const DualSenseHapticsRenderer&) = delete;

    static bool isAvailable();
    void submit(const LI_DS5_HAPTICS_PCM_FRAME& frame);
    void setControllerTarget(int controllerNumber);
    void reset();
    bool submit(const LI_DS5_HAPTICS_IR_FRAME_V2& frame, bool& startedNative);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
