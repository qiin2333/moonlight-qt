#include "streaming/input/gamepadsensorrate.h"

#include <cstdio>
#include <limits>

int main()
{
    using GamepadSensorRate::reportPeriodMs;
    static_assert(std::numeric_limits<GamepadSensorRate::PeriodMs>::max() >= 1000,
                  "sensor period storage must accommodate a 1 Hz report rate");
    const struct
    {
        uint16_t rateHz;
        uint16_t periodMs;
    } cases[] = { { 0, 0 },   { 1, 1000 }, { 2, 500 },  { 3, 334 },  { 4, 250 },
                  { 60, 17 }, { 200, 5 },  { 1000, 1 }, { 1001, 1 }, { UINT16_MAX, 1 } };
    for (const auto& test : cases) {
        if (reportPeriodMs(test.rateHz) != test.periodMs) {
            std::fprintf(stderr, "incorrect period for %u Hz\n", test.rateHz);
            return 1;
        }
    }

    // Every enabled rate must survive storage and respect its upper bound,
    // including rates below 4 Hz and rates above the millisecond resolution.
    for (uint32_t rateHz = 1; rateHz <= UINT16_MAX; ++rateHz) {
        const GamepadSensorRate::PeriodMs periodMs = reportPeriodMs(static_cast<uint16_t>(rateHz));
        if (periodMs == 0 || periodMs > 1000 || static_cast<uint32_t>(periodMs) * rateHz < 1000) {
            std::fprintf(stderr, "period exceeds rate limit for %u Hz\n", rateHz);
            return 1;
        }
    }
    std::puts("sensor report period tests passed");
    return 0;
}
