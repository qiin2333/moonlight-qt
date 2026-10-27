#pragma once

#include <memory>
#include <cstddef>
#include <cstdint>

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
    void updateControl(std::uint16_t controllerNumber, const std::uint8_t* state);
    bool hasControl(std::uint16_t controllerNumber) const;
    bool submitEffect(std::uint16_t controllerNumber, const void* effect, std::size_t size);
    void setControllerTarget(int controllerNumber, int joystickInstanceId = -1);
    void reset();
    bool submit(const LI_DS5_HAPTICS_IR_FRAME_V2& frame, bool& startedNative);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
