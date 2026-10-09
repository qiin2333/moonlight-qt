#pragma once

#include <cstdint>

namespace GamepadSensorRate {
using PeriodMs = uint16_t;

constexpr PeriodMs reportPeriodMs(uint16_t rateHz)
{
    // Zero disables reporting. Round up so the integer millisecond scheduler
    // cannot send events faster than the requested rate.
    return rateHz ? static_cast<PeriodMs>((1000u + rateHz - 1u) / rateHz) : 0;
}
}
