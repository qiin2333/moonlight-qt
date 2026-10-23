#pragma once

#include <cstdint>

/**
 * @brief The BandwidthMeter class converts a monotonically increasing byte
 * counter into an instantaneous Mbps value.
 *
 * No smoothing by design: the overlay must reflect real rate changes (e.g.
 * the host switching to a static desktop) within one sampling window. This
 * mirrors the Android client's BandwidthMeter exactly.
 *
 * Samples closer together than minIntervalUs return the last reading without
 * touching the baseline; samples further apart than maxIntervalUs re-baseline
 * without producing a reading (the interval is too stale to be a meaningful
 * rate). A byte or timestamp counter that moves backwards (stream restart)
 * invalidates the last reading and re-baselines.
 *
 * Not thread-safe; call update() from a single thread.
 */
class BandwidthMeter
{
public:
    BandwidthMeter(std::uint64_t minIntervalUs = 250000, std::uint64_t maxIntervalUs = 5000000);

    /**
     * @brief Records a counter sample and returns the Mbps rate over the
     * interval since the previous accepted sample.
     *
     * @return The instantaneous rate in Mbps, or NaN until a valid interval
     *         is available.
     */
    double update(std::uint64_t totalBytes, std::uint64_t nowUs);

    /**
     * @brief Clears the baseline so the next update() starts a fresh window.
     */
    void reset();

private:
    const std::uint64_t m_MinIntervalUs;
    const std::uint64_t m_MaxIntervalUs;
    bool m_HasBaseline;
    std::uint64_t m_PreviousBytes;
    std::uint64_t m_PreviousTimeUs;
    double m_LastMbps;
};
