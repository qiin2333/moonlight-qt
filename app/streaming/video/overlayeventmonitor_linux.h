#pragma once

#include <cstdint>
#include <functional>
#include <memory>

class LinuxDisplayEventMonitor
{
public:
    using DisplayFdProvider = std::function<int()>;

    explicit LinuxDisplayEventMonitor(std::function<void()> wakeCallback,
                                      DisplayFdProvider displayFdProvider = {});
    ~LinuxDisplayEventMonitor();

    LinuxDisplayEventMonitor(const LinuxDisplayEventMonitor&) = delete;
    LinuxDisplayEventMonitor& operator=(const LinuxDisplayEventMonitor&) = delete;

    bool attach(std::uintptr_t nativeWindow = 0);
    void detach();
    bool isAttached() const;

    // Re-arm the display descriptor only after Qt has drained its native events.
    void finishEventProcessing();

private:
    struct State;

    std::function<void()> m_WakeCallback;
    DisplayFdProvider m_DisplayFdProvider;
    std::unique_ptr<State> m_State;
};
