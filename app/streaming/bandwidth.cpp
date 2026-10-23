#include "bandwidth.h"

#include <cmath>

BandwidthMeter::BandwidthMeter(std::uint64_t minIntervalUs, std::uint64_t maxIntervalUs)
    : m_MinIntervalUs(minIntervalUs), m_MaxIntervalUs(maxIntervalUs), m_HasBaseline(false),
      m_PreviousBytes(0), m_PreviousTimeUs(0), m_LastMbps(std::nan(""))
{
}

void BandwidthMeter::reset()
{
    m_HasBaseline = false;
    m_PreviousBytes = 0;
    m_PreviousTimeUs = 0;
    m_LastMbps = std::nan("");
}

double BandwidthMeter::update(std::uint64_t totalBytes, std::uint64_t nowUs)
{
    if (!m_HasBaseline || totalBytes < m_PreviousBytes || nowUs < m_PreviousTimeUs) {
        // A counter that moved backwards (stream restart) invalidates the
        // last reading; either way, re-establish the baseline.
        if (m_HasBaseline && (totalBytes < m_PreviousBytes || nowUs < m_PreviousTimeUs)) {
            m_LastMbps = std::nan("");
        }
        m_PreviousBytes = totalBytes;
        m_PreviousTimeUs = nowUs;
        m_HasBaseline = true;
        return m_LastMbps;
    }

    std::uint64_t intervalUs = nowUs - m_PreviousTimeUs;
    if (intervalUs < m_MinIntervalUs) {
        return m_LastMbps;
    }
    if (intervalUs > m_MaxIntervalUs) {
        // The interval is too stale to be a meaningful rate; re-baseline
        // without producing a reading.
        m_PreviousBytes = totalBytes;
        m_PreviousTimeUs = nowUs;
        return m_LastMbps;
    }

    // bytes * 8 bits/byte / seconds / 1e6 bits/Mb reduces to bytes * 8 / us
    m_LastMbps = (double)(totalBytes - m_PreviousBytes) * 8.0 / (double)intervalUs;
    m_PreviousBytes = totalBytes;
    m_PreviousTimeUs = nowUs;
    return m_LastMbps;
}
