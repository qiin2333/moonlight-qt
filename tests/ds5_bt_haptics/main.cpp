// Tests for the DualSense Bluetooth haptics report path (dualsensehapticsbt.h).

#include "streaming/audio/dualsensehapticsbt.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dualsense_haptics::bt;

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

std::vector<std::uint8_t> fromHex(const std::string& hex)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// The first 270 bytes of a report 0x34 DSX sent to a Bluetooth DualSense
// (ds5-bridge dsx_report34_capture.bin), including its CRC.
const char* const CapturedReport =
    "34809107fe30303030307cd240fefefefe000001010202030304040505050604"
    "040101fefdfcfcfcfcfcfbfcfcffff010101010000fefefdfdfefe0000010101"
    "02020203030303030301010000fffffefefdfdfcfcfbfbfbfbfcfcfdfdffff00"
    "00010101010101010101010000fefdfcfcfbfbfbfbfbfbfcfcfdfd0000020205"
    "0507070707050603030101903ffdf700007e7fff09000f000000000000000000"
    "0000000000000000000000000000000000000a070000020005242d7800000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000"
    "00000000000000000000053bb1fa";

std::uint32_t storedCrc(const std::uint8_t* report)
{
    return report[CrcOffset] | (report[CrcOffset + 1] << 8) | (report[CrcOffset + 2] << 16) |
           (static_cast<std::uint32_t>(report[CrcOffset + 3]) << 24);
}

void testCrc()
{
    const std::string text = "123456789";
    check(crc32(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()) == 0xCBF43926u,
          "CRC32 check value");

    const auto captured = fromHex(CapturedReport);
    check(captured.size() == ReportLength, "captured report length");
    check(outputReportCrc(captured.data(), CrcOffset) == storedCrc(captured.data()),
          "CRC of the captured DSX report");
}

void testBuilder()
{
    Report34Builder builder;
    std::vector<std::int8_t> audio(AudioBytesPerReport);
    for (std::size_t i = 0; i < audio.size(); i++)
        audio[i] = static_cast<std::int8_t>(static_cast<int>(i) - 63);

    std::uint8_t first[ReportLength];
    std::uint8_t second[ReportLength];
    builder.build(first, audio.data());
    builder.build(second, audio.data());

    const auto captured = fromHex(CapturedReport);
    check(first[0] == 0x34, "report id");
    for (std::size_t i = 2; i < 10; i++)
        check(first[i] == captured[i], "fixed header bytes 2-9 match DSX");
    check(first[11] == 0xD2 && first[12] == 0x40, "fixed bytes 11-12");
    check(first[1] == 0x80 && second[1] == 0xA0, "sequence advances by 0x20");
    check(static_cast<std::uint8_t>(second[10] - first[10]) == 2, "counter advances by 2");
    check(std::equal(audio.begin(), audio.end(),
                     reinterpret_cast<const std::int8_t*>(first + AudioOffset)),
          "audio at bytes 13-138");
    bool controlZero = true;
    for (std::size_t i = AudioOffset + AudioBytesPerReport; i < CrcOffset; i++)
        controlZero = controlZero && first[i] == 0;
    check(controlZero, "control block left zero");
    check(outputReportCrc(first, CrcOffset) == storedCrc(first), "builder CRC");
    check(outputReportCrc(second, CrcOffset) == storedCrc(second), "builder CRC, second report");
}

std::vector<std::int16_t> sine(double frequency, double amplitude, std::size_t frames)
{
    std::vector<std::int16_t> pcm(frames * 2);
    for (std::size_t i = 0; i < frames; i++) {
        const auto v = static_cast<std::int16_t>(std::lround(
            amplitude * std::sin(2.0 * 3.14159265358979323846 * frequency * i / 48000.0)));
        pcm[i * 2] = v;
        pcm[i * 2 + 1] = static_cast<std::int16_t>(-v);
    }
    return pcm;
}

// Peak of one channel, skipping the filter's start-up.
int peak(const std::vector<std::int8_t>& out, int channel)
{
    int p = 0;
    for (std::size_t i = out.size() / 4 * 2 + channel; i < out.size(); i += 2)
        p = std::max(p, std::abs(static_cast<int>(out[i])));
    return p;
}

void testDownsampler()
{
    // Rate: InputFramesPerReport in -> FramesPerReport out.
    {
        Downsampler ds(1.0f);
        std::vector<std::int8_t> out;
        const auto pcm = sine(100, 0, 14550);
        ds.process(pcm.data(), 14550, out);
        const long frames = static_cast<long>(out.size() / 2);
        check(std::labs(frames - 630) <= 1, "63 output frames per 1455 input frames");
    }
    // Chunking must not change the output.
    {
        const auto pcm = sine(250, 12000, 9000);
        Downsampler whole(2.0f), pieces(2.0f);
        std::vector<std::int8_t> a, b;
        whole.process(pcm.data(), 9000, a);
        for (std::size_t offset = 0; offset < 9000;) {
            const std::size_t n = std::min<std::size_t>(9000 - offset, 1 + offset % 487);
            pieces.process(pcm.data() + offset * 2, n, b);
            offset += n;
        }
        check(a == b, "output independent of packet sizes");
    }
    // Pass band: a 200 Hz tone keeps its level (12000 * 2 / 256 ~= 94), stereo intact.
    {
        Downsampler ds(2.0f);
        std::vector<std::int8_t> out;
        const auto pcm = sine(200, 12000, 48000);
        ds.process(pcm.data(), 48000, out);
        const int left = peak(out, 0), right = peak(out, 1);
        check(left >= 85 && left <= 100, "200 Hz passes at full level (left)");
        check(right >= 85 && right <= 100, "200 Hz passes at full level (right)");
    }
    // Stop band: a 3 kHz tone would alias; it must be strongly attenuated.
    {
        Downsampler ds(2.0f);
        std::vector<std::int8_t> out;
        const auto pcm = sine(3000, 12000, 48000);
        ds.process(pcm.data(), 48000, out);
        check(peak(out, 0) <= 4, "3 kHz is filtered out");
    }
    // Clipping instead of wrap-around.
    {
        Downsampler ds(4.0f);
        std::vector<std::int8_t> out;
        const auto pcm = sine(100, 32000, 48000);
        ds.process(pcm.data(), 48000, out);
        check(peak(out, 0) == 128 || peak(out, 0) == 127, "loud input clips");
    }
}

void testAssembler()
{
    ReportAssembler assembler;
    std::vector<std::vector<std::uint8_t>> reports;
    const auto pcm = sine(150, 8000, 1455 * 3 + 100);
    std::size_t offset = 0;
    while (offset < 1455 * 3 + 100) {
        const std::size_t n = std::min<std::size_t>(240, 1455 * 3 + 100 - offset);
        assembler.push(pcm.data() + offset * 2, n, [&](const std::uint8_t* report) {
            reports.emplace_back(report, report + ReportLength);
            return true;
        });
        offset += n;
    }
    check(reports.size() == 3, "three reports from three reports' worth of PCM");
    for (const auto& r : reports)
        check(outputReportCrc(r.data(), CrcOffset) == storedCrc(r.data()), "assembled report CRC");
    check(reports.size() == 3 && reports[1][1] == static_cast<std::uint8_t>(reports[0][1] + 0x20),
          "assembled reports are sequenced");

    // A failing send stops delivery.
    int calls = 0;
    ReportAssembler failing;
    const bool ok = failing.push(pcm.data(), 1455 * 3, [&](const std::uint8_t*) {
        calls++;
        return false;
    });
    check(!ok && calls == 1, "send failure stops the push");
}

} // namespace

int main()
{
    testCrc();
    testBuilder();
    testDownsampler();
    testAssembler();
    if (failures == 0)
        std::printf("ds5_bt_haptics: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
