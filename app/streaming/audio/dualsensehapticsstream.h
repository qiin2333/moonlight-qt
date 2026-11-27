#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <Limelight.h>

namespace dualsense_haptics {

class PcmStreamTracker
{
public:
    enum class Action
    {
        Accept,
        ResetAndAccept,
        End,
        Ignore,
    };

    Action observe(std::uint8_t flags, std::uint16_t controllerNumber, std::uint32_t sequenceNumber,
                   std::uint16_t frameCount = 0)
    {
        if (m_Active && controllerNumber != m_ControllerNumber) {
            return Action::Ignore;
        }

        const bool streamStart = flags & LI_DS5_HAPTICS_PCM_FLAG_STREAM_START;
        const bool needsReset = streamStart || (flags & LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY);
        if (!needsReset && m_HasSequence && controllerNumber == m_ControllerNumber &&
            static_cast<std::int32_t>(sequenceNumber - m_ExpectedSequence) < 0) {
            return Action::Ignore;
        }
        const bool end = flags & LI_DS5_HAPTICS_PCM_FLAG_STREAM_END;
        if (end && frameCount == 0 && (!m_Active || controllerNumber != m_ControllerNumber)) {
            return Action::Ignore;
        }

        m_Active = !end;
        m_HasSequence = true;
        m_ControllerNumber = controllerNumber;
        m_ExpectedSequence = sequenceNumber + 1;
        if (end && frameCount == 0)
            return Action::End;
        // Unreliable PCM gaps do not discard queued samples or restart the endpoint.
        return needsReset ? Action::ResetAndAccept : Action::Accept;
    }

    void reset()
    {
        m_Active = false;
        m_HasSequence = false;
    }

private:
    bool m_Active = false;
    bool m_HasSequence = false;
    std::uint16_t m_ControllerNumber = 0;
    std::uint32_t m_ExpectedSequence = 0;
};

// The DualSense exposes one four-channel USB endpoint. The first pair carries
// speaker/headphone audio; the second pair drives the haptic voice coils.
constexpr std::uint32_t EndpointChannelCount = 4;
constexpr std::uint32_t HapticsChannelOffset = 2;

constexpr std::uint32_t PrebufferFrames = 720; // 15 ms at 48 kHz

// Older hosts send only the two haptic channels. pad-audio hosts send all four
// channels in endpoint order, keeping speaker audio and haptics in the same frame.
template <typename Sample, typename Convert>
void writePcmChannels(Sample* out, const std::int16_t* in, std::uint16_t frameCount,
                      std::uint8_t channelCount, Convert convert)
{
    if (channelCount == EndpointChannelCount) {
        std::transform(in, in + static_cast<std::size_t>(frameCount) * EndpointChannelCount, out,
                       convert);
        return;
    }

    std::fill_n(out, static_cast<std::size_t>(frameCount) * EndpointChannelCount, Sample{});
    for (std::uint16_t i = 0; i < frameCount; i++) {
        out[i * EndpointChannelCount + HapticsChannelOffset] = convert(in[i * 2]);
        out[i * EndpointChannelCount + HapticsChannelOffset + 1] = convert(in[i * 2 + 1]);
    }
}

// Whether a prebuffer must start the endpoint before taking another packet.
// Before it starts, the endpoint takes at most bufferFrames frames and, when
// maxPackets is not 0, at most maxPackets packets.
inline bool mustStartBeforePrebuffering(std::uint32_t prebufferedFrames,
                                        std::size_t prebufferedPackets, std::uint32_t nextFrames,
                                        std::uint32_t bufferFrames, std::uint32_t maxPackets)
{
    return prebufferedPackets != 0 && (prebufferedFrames + nextFrames > bufferFrames ||
                                       (maxPackets != 0 && prebufferedPackets >= maxPackets));
}

inline bool isPrebufferFull(std::uint32_t prebufferedFrames, std::uint32_t bufferFrames)
{
    return prebufferedFrames >= std::min(PrebufferFrames, bufferFrames);
}
}
