#include "streaming/audio/dualsensehapticscalibration.h"
#include "streaming/audio/dualsensehapticsrouting.h"
#include "streaming/audio/dualsensehapticsstream.h"
#include "streaming/audio/dualsensehapticscontrol.h"

#include <algorithm>
#include <cstdint>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main()
{
    LI_DS5_HAPTICS_IR_FRAME_V2 frame = {};
    frame.lanes[0].rmsAmplitude = 0.5f;
    frame.lanes[0].lowBandRatio = 1.0f;
    frame.lanes[1].rmsAmplitude = 0.4f;
    frame.lanes[1].transientStrength = 1.0f;

    const auto active = dualsense_haptics::renderIrV2(frame);
    CHECK(active.lowFrequency > 0);
    CHECK(active.highFrequency > 0);

    const auto native = dualsense_haptics::renderIrV2Native(frame);
    CHECK(native.left.intensity > 0.0f);
    CHECK(native.right.intensity > 0.0f);
    CHECK(native.left.sharpness < native.right.sharpness);
    CHECK(native.left.intensity <= 1.0f);
    CHECK(native.right.intensity <= 1.0f);

    frame.flags = LI_DS5_HAPTICS_IR_FLAG_STREAM_END;
    const auto ended = dualsense_haptics::renderIrV2(frame);
    CHECK(ended.lowFrequency == 0);
    CHECK(ended.highFrequency == 0);
    const auto nativeEnded = dualsense_haptics::renderIrV2Native(frame);
    CHECK(nativeEnded.left.intensity == 0.0f);
    CHECK(nativeEnded.right.intensity == 0.0f);

    frame.flags = LI_DS5_HAPTICS_IR_FLAG_SILENT;
    const auto silent = dualsense_haptics::renderIrV2(frame);
    CHECK(silent.lowFrequency == 0);
    CHECK(silent.highFrequency == 0);
    const auto nativeSilent = dualsense_haptics::renderIrV2Native(frame);
    CHECK(nativeSilent.left.intensity == 0.0f);
    CHECK(nativeSilent.right.intensity == 0.0f);

    using Candidate = dualsense_haptics::LocalControllerCandidate;
    const Candidate singleDualSense[] = {{0, true}};
    CHECK(dualsense_haptics::selectUniqueLocalDualSense(singleDualSense, 1, true) == 0);
    CHECK(dualsense_haptics::selectUniqueLocalDualSense(singleDualSense, 1, false) == 0);

    const Candidate mixedControllers[] = {{0, false}, {1, true}};
    CHECK(dualsense_haptics::selectUniqueLocalDualSense(mixedControllers, 2, true) == 1);
    const Candidate mergedControllers[] = {{0, false}, {0, true}};
    CHECK(dualsense_haptics::selectUniqueLocalDualSense(mergedControllers, 2, false) == -1);

    const Candidate twoDualSense[] = {{0, true}, {1, true}};
    CHECK(dualsense_haptics::selectUniqueLocalDualSense(twoDualSense, 2, true) == -1);
    CHECK(dualsense_haptics::selectUniqueLocalDualSense(nullptr, 0, true) == -1);

    // Physical haptics only needs the DualSense itself to be unique.
    CHECK(dualsense_haptics::selectUniqueDualSense(mergedControllers, 2) == 0);
    CHECK(dualsense_haptics::selectUniqueDualSense(mixedControllers, 2) == 1);
    CHECK(dualsense_haptics::selectUniqueDualSense(twoDualSense, 2) == -1);
    const Candidate noDualSense[] = { { 0, false } };
    CHECK(dualsense_haptics::selectUniqueDualSense(noDualSense, 1) == -1);
    CHECK(dualsense_haptics::selectUniqueDualSense(nullptr, 0) == -1);

    CHECK(!dualsense_haptics::canUseNativeController(0, 1, 1));
    CHECK(dualsense_haptics::canUseNativeController(1, 1, 1));
    CHECK(!dualsense_haptics::canUseNativeController(1, -1, 1));
    CHECK(!dualsense_haptics::canUseNativeController(1, 1, 0));
    CHECK(!dualsense_haptics::canUseNativeController(1, 1, 2));
    dualsense_haptics::IrBackendLatch backendLatch;
    CHECK(backendLatch.shouldAttemptNative(1, false));
    backendLatch.useFallback(1);
    CHECK(!backendLatch.shouldAttemptNative(1, false));
    CHECK(!backendLatch.shouldAttemptNative(1, false));
    backendLatch.useFallback(0);
    CHECK(!backendLatch.shouldAttemptNative(0, false));
    CHECK(!backendLatch.shouldAttemptNative(1, true));
    CHECK(backendLatch.shouldAttemptNative(1, false));
    CHECK(!backendLatch.shouldAttemptNative(0, false));
    CHECK(!backendLatch.shouldAttemptNative(0, true));
    CHECK(backendLatch.shouldAttemptNative(0, false));
    backendLatch.useFallback(0);
    backendLatch.useFallback(1);
    backendLatch.reset();
    CHECK(backendLatch.shouldAttemptNative(0, false));
    CHECK(backendLatch.shouldAttemptNative(1, false));

    using Action = dualsense_haptics::PcmStreamTracker::Action;
    dualsense_haptics::PcmStreamTracker tracker;
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_START, 0, 10) ==
          Action::ResetAndAccept);
    CHECK(tracker.observe(0, 0, 11) == Action::Accept);

    // A second controller must not continually reset the endpoint owned by the
    // first controller, even when its stream starts while the first is active.
    CHECK(tracker.observe(0, 1, 20) == Action::Ignore);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_START, 1, 20) == Action::Ignore);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_END, 1, 21) == Action::Ignore);
    CHECK(tracker.observe(0, 0, 12) == Action::Accept);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_END, 0, 13) == Action::End);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_START, 1, 20) ==
          Action::ResetAndAccept);
    CHECK(tracker.observe(0, 0, 13) == Action::Ignore);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_END, 1, 21) == Action::End);

    // A long quiet period does not alter state: the next contiguous packet is
    // accepted without forcing another endpoint reset and prebuffer delay.
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_START, 0, 100) ==
          Action::ResetAndAccept);
    CHECK(tracker.observe(0, 0, 101) == Action::Accept);

    // Gaps retain buffered PCM; duplicates and reordered packets cannot replay it.
    CHECK(tracker.observe(0, 0, 103) == Action::Accept);
    CHECK(tracker.observe(0, 0, 103) == Action::Ignore);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_END, 0, 102) == Action::Ignore);
    CHECK(tracker.observe(0, 0, 104) == Action::Accept);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_END, 0, 105, 240) == Action::Accept);
    CHECK(tracker.observe(LI_DS5_HAPTICS_PCM_FLAG_STREAM_START | LI_DS5_HAPTICS_PCM_FLAG_STREAM_END,
                          0, 106, 240) == Action::ResetAndAccept);

    dualsense_haptics::ControlState controls;
    const std::uint8_t host[] = { 2, 4, 0x62, 0, 0, 0x10, 0x23, 1 };
    CHECK(controls.update(7, host));
    CHECK(!controls.has(0) && controls.has(7));
    CHECK(!controls.update(16, host));
    auto invalid = std::array<std::uint8_t, 8>{ 0x80, 0, 0, 0, 0, 0, 0, 0 };
    CHECK(!controls.update(7, invalid.data()));
    dualsense_haptics::ControlState::Effect led{};
    led[0] = 3;
    led[2] = 255; // Generic rumble cannot replace authoritative zero-strength V2.
    led[1] = 4;
    led[44] = 11;
    led[45] = 22;
    led[46] = 33;
    CHECK(controls.mergeEffect(7, led));
    dualsense_haptics::ControlState::Effect trigger{};
    trigger[0] = 4;
    trigger[10] = 5;
    trigger[11] = 99;
    CHECK(controls.mergeEffect(7, trigger));
    CHECK(!controls.mergeEffect(0, trigger));
    const auto effect = controls.effect(7);
    CHECK(effect[0] == 6 && effect[38] == 4 && effect[1] == 0x66);
    CHECK(effect[2] == 0 && effect[3] == 0 && effect[9] == 0x10 && effect[36] == 0x23 &&
          effect[39] == 1);
    CHECK(effect[10] == 5 && effect[11] == 99 && effect[44] == 11 && effect[46] == 33);
    controls.reset();
    CHECK(!controls.has(7));

    LI_DS5_OUTPUT_EXTENSION output{};
    output.controllerNumber = 7;
    output.validMask = 0xff;
    output.speakerVolume = 100;
    output.headphoneVolume = 127;
    output.micVolume = 64;
    output.audioRouting = 0x30;
    output.muteLed = 1;
    output.micMute = 1;
    output.playerLedMask = 0x15;
    output.lightbarBrightness = 2;
    CHECK(controls.updateOutput(output));
    CHECK(!controls.has(7)); // Audio controls alone must not take over SDL rumble.
    const std::uint8_t powerState[] = { 2, 4, 0x62, 0, 0, 0xa5, 0x23, 1 };
    CHECK(controls.update(7, powerState));
    CHECK(controls.mergeEffect(7, led));
    CHECK(controls.mergeEffect(7, trigger));
    auto audioEffect = controls.effect(7);
    CHECK(audioEffect[0] == 0xf6 && audioEffect[1] == 0x77);
    CHECK(audioEffect[4] == 127 && audioEffect[5] == 100 && audioEffect[6] == 64);
    CHECK(audioEffect[7] == 0x30 && audioEffect[8] == 1 && audioEffect[9] == 0xb5);
    CHECK(audioEffect[38] == 5 && audioEffect[42] == 2 && audioEffect[43] == 0x15);
    CHECK(audioEffect[10] == 5 && audioEffect[11] == 99);
    CHECK(audioEffect[44] == 11 && audioEffect[45] == 22 && audioEffect[46] == 33);

    output.validMask = LI_DS5_OUT_SPEAKER_VOLUME;
    output.speakerVolume = 65;
    CHECK(controls.updateOutput(output));
    audioEffect = controls.effect(7);
    CHECK(audioEffect[5] == 65 && audioEffect[4] == 127 && audioEffect[7] == 0x30);
    output.validMask = LI_DS5_OUT_MIC_MUTE;
    output.micMute = 0;
    CHECK(controls.updateOutput(output));
    CHECK(controls.effect(7)[9] == 0xa5); // Preserve the actuator power-save bits.
    output.controllerNumber = 16;
    CHECK(!controls.updateOutput(output));
    output.controllerNumber = 7;
    output.validMask = 0x100;
    CHECK(!controls.updateOutput(output));
    controls.reset();
    CHECK(controls.update(7, powerState));
    audioEffect = controls.effect(7);
    CHECK(audioEffect[0] == 2 && audioEffect[1] == 0x62 && audioEffect[38] == 4);
    CHECK(audioEffect[4] == 0 && audioEffect[5] == 0 && audioEffect[7] == 0);
    CHECK(audioEffect[9] == 0xa5 && audioEffect[42] == 0 && audioEffect[43] == 0);

    // Authored left/right PCM goes to channels 3 and 4; the headset pair stays
    // silent.
    const std::int16_t stereo[] = { 100, -200, 300, -400 };
    std::int16_t endpointFrames[8];
    std::fill_n(endpointFrames, 8, std::int16_t{ 1 });
    dualsense_haptics::writePcmChannels(endpointFrames, stereo, 2, 2,
                                        [](std::int16_t s) { return s; });
    const std::int16_t expectedFrames[] = { 0, 0, 100, -200, 0, 0, 300, -400 };
    CHECK(std::equal(endpointFrames, endpointFrames + 8, expectedFrames));

    float floatFrames[4];
    dualsense_haptics::writePcmChannels(floatFrames, stereo, 1, 2,
                                        [](std::int16_t s) { return s / 32768.0f; });
    CHECK(floatFrames[0] == 0.0f && floatFrames[1] == 0.0f);
    CHECK(floatFrames[2] == 100 / 32768.0f && floatFrames[3] == -200 / 32768.0f);

    // Four-channel packets preserve each authored speaker/haptic sample, including
    // both extrema, for every endpoint format. No channel pair is duplicated.
    const std::int16_t quad[] = { 100, -200, 300, -400, -32768, 32767, 0, 12345 };
    dualsense_haptics::writePcmChannels(endpointFrames, quad, 2, 4,
                                        [](std::int16_t s) { return s; });
    CHECK(std::equal(endpointFrames, endpointFrames + 8, quad));
    float quadFloat[8];
    dualsense_haptics::writePcmChannels(quadFloat, quad, 2, 4,
                                        [](std::int16_t s) { return s / 32768.0f; });
    std::int32_t quadInt[8];
    dualsense_haptics::writePcmChannels(
        quadInt, quad, 2, 4, [](std::int16_t s) { return static_cast<std::int32_t>(s) * 65536; });
    for (unsigned i = 0; i < 8; ++i) {
        CHECK(quadFloat[i] == quad[i] / 32768.0f);
        CHECK(quadInt[i] == static_cast<std::int32_t>(quad[i]) * 65536);
    }

    // Prebuffering stops at 15 ms, or earlier when the endpoint holds less.
    CHECK(!dualsense_haptics::isPrebufferFull(480, 2400));
    CHECK(dualsense_haptics::isPrebufferFull(720, 2400));
    CHECK(dualsense_haptics::isPrebufferFull(480, 480));

    // Start before a packet would overflow the endpoint's frames or packets.
    CHECK(!dualsense_haptics::mustStartBeforePrebuffering(0, 0, 480, 240, 0));
    CHECK(!dualsense_haptics::mustStartBeforePrebuffering(240, 1, 240, 480, 0));
    CHECK(dualsense_haptics::mustStartBeforePrebuffering(240, 1, 241, 480, 0));
    CHECK(!dualsense_haptics::mustStartBeforePrebuffering(31, 31, 1, 2400, 32));
    CHECK(dualsense_haptics::mustStartBeforePrebuffering(32, 32, 1, 2400, 32));
    return 0;
}
