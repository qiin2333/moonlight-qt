#pragma once

// DualSense HD haptics over Bluetooth.
//
// Over USB the controller exposes a four-channel audio endpoint and the haptic
// voice coils are simply channels 3 and 4. Over Bluetooth there is no audio
// endpoint; the haptics PCM travels inside HID output report 0x34 instead:
//
//   offset  size  content
//        0     1  report id 0x34
//        1     1  sequence (+0x20 per report)
//      2-9     8  fixed header 91 07 fe 30 30 30 30 30
//       10     1  frame counter (+2 per report)
//    11-12     2  fixed d2 40
//   13-138   126  63 stereo frames, signed 8-bit, interleaved L/R
//  139-265        control block (left zero: SDL drives LEDs/triggers via 0x31)
//  266-269     4  CRC32 (seed byte 0xa2, over bytes 0-265), little endian
//
// One report carries ~30 ms, so the coils run at roughly 2.1 kHz. Layout and
// pacing are taken from koshisan/ds5-bridge (REPORT_0x34.md), where they were
// reverse engineered from DSX captures and have been in daily use.
//
// Everything in this header is platform independent so it can be unit tested.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dualsense_haptics {
namespace bt {

constexpr std::uint8_t ReportId = 0x34;
// Bytes covered by the report, including the CRC. Windows pads every write to
// the collection's longest output report (547 bytes for report 0x39).
constexpr std::size_t ReportLength = 270;
constexpr std::size_t CrcOffset = 266;
constexpr std::size_t AudioOffset = 13;
constexpr std::size_t FramesPerReport = 63;
constexpr std::size_t AudioBytesPerReport = FramesPerReport * 2;

// 48 kHz input frames consumed per report (~30 ms). This is the ratio
// ds5-bridge uses in production, i.e. an output rate of ~2077 Hz.
constexpr double InputFramesPerReport = 1455.0;
constexpr double InputRate = 48000.0;

constexpr float DefaultGain = 2.0f;

inline std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t size)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; i++) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (std::size_t i = 0; i < size; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

// Standard CRC32 (as zlib.crc32) over data.
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    return ~crc32Update(0xFFFFFFFFu, data, size);
}

// CRC of a Bluetooth output report: CRC32 over the 0xa2 transaction header
// byte followed by the report bytes.
inline std::uint32_t outputReportCrc(const std::uint8_t* report, std::size_t size)
{
    const std::uint8_t seed = 0xA2;
    return ~crc32Update(crc32Update(0xFFFFFFFFu, &seed, 1), report, size);
}

class Report34Builder
{
public:
    // Write one report into out[0..ReportLength). audio holds FramesPerReport
    // interleaved stereo signed 8-bit frames.
    void build(std::uint8_t* out, const std::int8_t* audio)
    {
        std::fill_n(out, ReportLength, std::uint8_t{ 0 });
        out[0] = ReportId;
        out[1] = m_Sequence;
        static constexpr std::uint8_t header[] = { 0x91, 0x07, 0xFE, 0x30, 0x30, 0x30, 0x30, 0x30 };
        std::copy(std::begin(header), std::end(header), out + 2);
        out[10] = m_Counter;
        out[11] = 0xD2;
        out[12] = 0x40;
        for (std::size_t i = 0; i < AudioBytesPerReport; i++)
            out[AudioOffset + i] = static_cast<std::uint8_t>(audio[i]);

        const std::uint32_t crc = outputReportCrc(out, CrcOffset);
        out[CrcOffset + 0] = static_cast<std::uint8_t>(crc);
        out[CrcOffset + 1] = static_cast<std::uint8_t>(crc >> 8);
        out[CrcOffset + 2] = static_cast<std::uint8_t>(crc >> 16);
        out[CrcOffset + 3] = static_cast<std::uint8_t>(crc >> 24);

        m_Sequence = static_cast<std::uint8_t>(m_Sequence + 0x20);
        m_Counter = static_cast<std::uint8_t>(m_Counter + 2);
    }

    void reset()
    {
        m_Sequence = 0x80;
        m_Counter = 0x80;
    }

private:
    std::uint8_t m_Sequence = 0x80;
    std::uint8_t m_Counter = 0x80;
};

// 48 kHz interleaved 16-bit stereo -> ~2.1 kHz interleaved signed 8-bit stereo.
// A 4th order Butterworth low pass at 900 Hz keeps the decimation from
// aliasing, then each output frame is linearly interpolated.
class Downsampler
{
public:
    explicit Downsampler(float gain = DefaultGain) : m_Gain(gain)
    {
        // Butterworth Q values for the two second-order sections of a 4th order filter.
        designLowPass(m_Sections[0], 900.0, 0.54119610);
        designLowPass(m_Sections[1], 900.0, 1.30656296);
        reset();
    }

    void setGain(float gain) { m_Gain = gain; }

    void reset()
    {
        for (auto& section : m_Sections)
            section.z = {};
        m_Previous = {};
        m_NextOutput = 0.0;
    }

    void process(const std::int16_t* in, std::size_t frames, std::vector<std::int8_t>& out)
    {
        constexpr double step = InputFramesPerReport / FramesPerReport;
        for (std::size_t i = 0; i < frames; i++) {
            std::array<double, 2> current;
            for (int ch = 0; ch < 2; ch++) {
                double v = in[i * 2 + ch];
                for (auto& section : m_Sections)
                    v = section.run(ch, v);
                current[ch] = v;
            }
            // m_NextOutput is the next output position relative to the
            // previous input sample (0) and the current one (1).
            while (m_NextOutput <= 1.0) {
                const double t = m_NextOutput;
                for (int ch = 0; ch < 2; ch++)
                    out.push_back(quantize(m_Previous[ch] + (current[ch] - m_Previous[ch]) * t));
                m_NextOutput += step;
            }
            m_NextOutput -= 1.0;
            m_Previous = current;
        }
    }

private:
    struct Section
    {
        double b0, b1, b2, a1, a2;
        std::array<std::array<double, 2>, 2> z; // [channel][state]

        double run(int ch, double x)
        {
            // Transposed direct form II.
            const double y = b0 * x + z[ch][0];
            z[ch][0] = b1 * x - a1 * y + z[ch][1];
            z[ch][1] = b2 * x - a2 * y;
            return y;
        }
    };

    static void designLowPass(Section& s, double cutoff, double q)
    {
        const double w0 = 2.0 * 3.14159265358979323846 * cutoff / InputRate;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double cosw0 = std::cos(w0);
        const double a0 = 1.0 + alpha;
        s.b0 = (1.0 - cosw0) / 2.0 / a0;
        s.b1 = (1.0 - cosw0) / a0;
        s.b2 = s.b0;
        s.a1 = -2.0 * cosw0 / a0;
        s.a2 = (1.0 - alpha) / a0;
        s.z = {};
    }

    std::int8_t quantize(double sample) const
    {
        const double scaled = std::round(sample * m_Gain / 256.0);
        return static_cast<std::int8_t>(std::clamp(scaled, -128.0, 127.0));
    }

    std::array<Section, 2> m_Sections{};
    std::array<double, 2> m_Previous{};
    double m_NextOutput = 0.0;
    float m_Gain;
};

// Collects downsampled frames and hands out complete reports.
class ReportAssembler
{
public:
    explicit ReportAssembler(float gain = DefaultGain) : m_Downsampler(gain) {}

    void setGain(float gain) { m_Downsampler.setGain(gain); }

    void reset()
    {
        m_Downsampler.reset();
        m_Builder.reset();
        m_Pending.clear();
    }

    // Feed 48 kHz interleaved 16-bit stereo, then call send(report) for every
    // completed report. Stops and returns false as soon as send() fails.
    template <typename Send> bool push(const std::int16_t* pcm, std::size_t frames, Send send)
    {
        m_Downsampler.process(pcm, frames, m_Pending);
        std::size_t consumed = 0;
        bool ok = true;
        while (ok && m_Pending.size() - consumed >= AudioBytesPerReport) {
            m_Builder.build(m_Report.data(), m_Pending.data() + consumed);
            consumed += AudioBytesPerReport;
            ok = send(m_Report.data());
        }
        m_Pending.erase(m_Pending.begin(), m_Pending.begin() + consumed);
        return ok;
    }

    // End of a stream: pad the partial report with silence and send it, so short
    // effects and stream tails are not left waiting for samples that never come.
    template <typename Send> bool flush(Send send)
    {
        if (m_Pending.empty())
            return true;
        m_Pending.resize(AudioBytesPerReport, 0);
        m_Builder.build(m_Report.data(), m_Pending.data());
        m_Pending.clear();
        return send(m_Report.data());
    }

    std::size_t pendingFrames() const { return m_Pending.size() / 2; }

private:
    Downsampler m_Downsampler;
    Report34Builder m_Builder;
    std::vector<std::int8_t> m_Pending;
    std::array<std::uint8_t, ReportLength> m_Report{};
};

} // namespace bt
} // namespace dualsense_haptics
