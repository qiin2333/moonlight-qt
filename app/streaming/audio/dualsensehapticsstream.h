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

    Action observe(std::uint8_t flags, std::uint16_t controllerNumber,
                   std::uint32_t sequenceNumber)
    {
        if (flags & LI_DS5_HAPTICS_PCM_FLAG_STREAM_END) {
            if (!m_Active || controllerNumber != m_ControllerNumber) {
                return Action::Ignore;
            }

            m_Active = false;
            return Action::End;
        }

        if (m_Active && controllerNumber != m_ControllerNumber) {
            return Action::Ignore;
        }

        const bool streamStart = flags & LI_DS5_HAPTICS_PCM_FLAG_STREAM_START;
        const bool needsReset =
            streamStart ||
            (flags & LI_DS5_HAPTICS_PCM_FLAG_DISCONTINUITY) ||
            (m_Active && controllerNumber == m_ControllerNumber &&
             sequenceNumber != m_ExpectedSequence);

        m_Active = true;
        m_ControllerNumber = controllerNumber;
        m_ExpectedSequence = sequenceNumber + 1;
        return needsReset ? Action::ResetAndAccept : Action::Accept;
    }

    void reset()
    {
        m_Active = false;
    }

private:
    bool m_Active = false;
    std::uint16_t m_ControllerNumber = 0;
    std::uint32_t m_ExpectedSequence = 0;
};

// The DualSense exposes a single four-channel USB audio endpoint: channels 1
// and 2 drive the headset jack, channels 3 and 4 drive the two haptic voice
// coils. We render silence to the headset pair and the authored PCM to the
// haptics pair.
constexpr std::uint32_t EndpointChannelCount = 4;
constexpr std::uint32_t HapticsChannelOffset = 2;

constexpr std::uint32_t PrebufferFrames = 720; // 15 ms at 48 kHz

// Expand interleaved 16-bit stereo into endpoint frames: silence on the headset
// pair, authored PCM on the haptics pair.
template <typename Sample, typename Convert>
void spreadToHapticsChannels(Sample* out, const std::int16_t* in, std::uint16_t frameCount,
                             Convert convert)
{
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
