#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dualsense_haptics {

struct LocalControllerCandidate
{
    int logicalNumber;
    bool dualSense;
};

inline int selectUniqueLocalDualSense(const LocalControllerCandidate* controllers,
                                     std::size_t controllerCount,
                                     bool multiController)
{
    if (controllers == nullptr || controllerCount == 0 ||
        (!multiController && controllerCount != 1)) {
        return -1;
    }

    int selected = -1;
    for (std::size_t i = 0; i < controllerCount; i++) {
        if (!controllers[i].dualSense) {
            continue;
        }
        if (selected >= 0) {
            return -1;
        }
        selected = controllers[i].logicalNumber;
    }
    return selected;
}

inline bool canUseNativeController(std::uint16_t controllerNumber,
                                   int selectedLocalController,
                                   std::size_t nativeControllerCount)
{
    return selectedLocalController >= 0 &&
           controllerNumber == static_cast<std::uint16_t>(selectedLocalController) &&
           nativeControllerCount == 1;
}

inline bool canKeepNativeState(std::uint16_t controllerNumber,
                               int selectedLocalController,
                               std::size_t nativeControllerCount,
                               bool sameNativeController)
{
    return sameNativeController &&
           canUseNativeController(controllerNumber,
                                  selectedLocalController,
                                  nativeControllerCount);
}

class IrBackendLatch
{
public:
    bool shouldAttemptNative(std::uint16_t controllerNumber, bool streamEnd)
    {
        if (streamEnd) {
            m_FallbackControllers.erase(controllerNumber);
            return false;
        }
        return m_FallbackControllers.find(controllerNumber) == m_FallbackControllers.end();
    }

    void useFallback(std::uint16_t controllerNumber)
    {
        m_FallbackControllers.insert(controllerNumber);
    }

    void reset()
    {
        m_FallbackControllers.clear();
    }

private:
    std::unordered_set<std::uint16_t> m_FallbackControllers;
};

// IR updates arrive every 5 ms over an unreliable channel. Bound the lifetime
// of Core Haptics' infinite players in case the final silent/end frame is lost.
class NativeStateLeaseTracker
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit NativeStateLeaseTracker(
        Clock::duration timeout = std::chrono::milliseconds(250)) :
        m_Timeout(timeout)
    {
    }

    void renew(std::uint16_t controllerNumber, TimePoint now = Clock::now())
    {
        m_Deadlines[controllerNumber] = now + m_Timeout;
    }

    void remove(std::uint16_t controllerNumber)
    {
        m_Deadlines.erase(controllerNumber);
    }

    void clear()
    {
        m_Deadlines.clear();
    }

    std::optional<TimePoint> nextDeadline() const
    {
        if (m_Deadlines.empty()) {
            return std::nullopt;
        }
        return std::min_element(
            m_Deadlines.begin(), m_Deadlines.end(),
            [](const auto& left, const auto& right) {
                return left.second < right.second;
            })->second;
    }

    std::vector<std::uint16_t> takeExpired(TimePoint now = Clock::now())
    {
        std::vector<std::uint16_t> expired;
        for (auto it = m_Deadlines.begin(); it != m_Deadlines.end();) {
            if (it->second <= now) {
                expired.push_back(it->first);
                it = m_Deadlines.erase(it);
            }
            else {
                ++it;
            }
        }
        return expired;
    }

private:
    Clock::duration m_Timeout;
    std::unordered_map<std::uint16_t, TimePoint> m_Deadlines;
};

} // namespace dualsense_haptics
