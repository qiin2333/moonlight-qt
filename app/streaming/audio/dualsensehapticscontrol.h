#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

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
        report[36] = state[6];
        report[38] = state[1];
        report[39] = state[7];
        return report;
    }

    void reset() { m_Controllers = {}; }

private:
    struct Controller
    {
        std::optional<std::array<std::uint8_t, 8>> state;
        Effect extra{};
    };
    std::array<Controller, 16> m_Controllers{};
};
}
