#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

#include <Limelight.h>

namespace dualsense_haptics {

// SDL's PS5 effect payload excludes the transport report id and CRC. All access
// is under the renderer queue mutex; this class never performs device I/O.
class ControlState
{
public:
    using Effect = std::array<std::uint8_t, 47>;

    bool has(unsigned controller) const
    {
        return controller < m_Controllers.size() && m_Controllers[controller].state.has_value();
    }

    bool update(unsigned controller, const std::uint8_t* state)
    {
        if (controller >= m_Controllers.size() || state == nullptr || (state[0] & ~3) ||
            (state[1] & ~4) || (state[2] & ~0x62) || (state[7] & ~1))
            return false;
        std::array<std::uint8_t, 8> snapshot;
        std::copy_n(state, snapshot.size(), snapshot.begin());
        m_Controllers[controller].state = snapshot;
        return true;
    }

    // common-c has validated the field values. Preserve fields omitted by a
    // partial update. Raw HID reports also select the haptics mode, so these
    // fields are only sent once has() confirms the host's actuator state.
    bool updateOutput(const LI_DS5_OUTPUT_EXTENSION& output)
    {
        constexpr std::uint32_t KnownOutputMask = 0xff;
        if (output.controllerNumber >= m_Controllers.size() || output.validMask == 0 ||
            (output.validMask & ~KnownOutputMask) != 0)
            return false;

        auto& entry = m_Controllers[output.controllerNumber];
        auto& report = entry.extra;
        entry.outputMask |= output.validMask;
        const auto set = [&](std::uint32_t mask, unsigned flagByte, std::uint8_t flag,
                             unsigned valueByte, std::uint8_t value) {
            if (output.validMask & mask) {
                report[flagByte] |= flag;
                report[valueByte] = value;
            }
        };
        set(LI_DS5_OUT_SPEAKER_VOLUME, 0, 0x20, 5, output.speakerVolume);
        set(LI_DS5_OUT_HEADPHONE_VOLUME, 0, 0x10, 4, output.headphoneVolume);
        set(LI_DS5_OUT_MIC_VOLUME, 0, 0x40, 6, output.micVolume);
        set(LI_DS5_OUT_AUDIO_ROUTING, 0, 0x80, 7, output.audioRouting);
        set(LI_DS5_OUT_MUTE_LED, 1, 0x01, 8, output.muteLed);
        set(LI_DS5_OUT_PLAYER_LEDS, 1, 0x10, 43, output.playerLedMask);
        set(LI_DS5_OUT_LIGHTBAR_BRIGHTNESS, 38, 0x01, 42, output.lightbarBrightness);
        if (output.validMask & LI_DS5_OUT_MIC_MUTE) {
            report[1] |= 0x02;
            report[9] = output.micMute ? 0x10 : 0;
        }
        return true;
    }

    bool mergeEffect(unsigned controller, const Effect& effect)
    {
        if (!has(controller))
            return false;
        auto& extra = m_Controllers[controller].extra;
        for (unsigned side = 0; side < 2; ++side) {
            const auto flag = 4 << side;
            if (effect[0] & flag) {
                extra[0] |= flag;
                std::copy_n(effect.begin() + 10 + side * 11, 11, extra.begin() + 10 + side * 11);
            }
        }
        if (effect[1] & 4) {
            extra[1] |= 4;
            std::copy_n(effect.begin() + 44, 3, extra.begin() + 44);
        }
        return true;
    }

    Effect effect(unsigned controller) const
    {
        const auto& entry = m_Controllers[controller];
        auto report = entry.extra;
        const auto& state = *entry.state;
        report[0] |= state[0];
        report[1] |= state[2];
        report[2] = state[3];
        report[3] = state[4];
        report[9] = state[5];
        if (entry.outputMask & LI_DS5_OUT_MIC_MUTE)
            report[9] = (report[9] & ~0x10) | (entry.extra[9] & 0x10);
        report[36] = state[6];
        report[38] |= state[1];
        report[39] = state[7];
        return report;
    }

    void reset() { m_Controllers = {}; }

private:
    struct Controller
    {
        std::optional<std::array<std::uint8_t, 8>> state;
        Effect extra{};
        std::uint32_t outputMask = 0;
    };
    std::array<Controller, 16> m_Controllers{};
};
}
