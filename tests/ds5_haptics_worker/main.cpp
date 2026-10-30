// Compile the production worker with fake endpoint/SDL boundaries, not a second pump.
#define SDL_MAIN_HANDLED
#include "SDL_compat.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace {
enum class WriteResult;
struct Packet;
class MockEndpoint
{
public:
    bool threadInit() { return true; }
    void threadCleanup() {}
    bool open()
    {
        m_Open = true;
        return true;
    }
    void close()
    {
        reset();
        m_Open = false;
    }
    bool isOpen() const { return m_Open; }
    bool isStarted() const { return m_Started; }
    unsigned bufferFrames() const { return 2400; }
    unsigned maxQueuedPackets() const { return 0; }
    bool start();
    void reset();
    WriteResult tryWrite(const Packet& packet);

private:
    bool m_Open = false, m_Started = false;
};

struct Event
{
    char kind;
    int joystick = -1;
    std::array<std::uint8_t, 47> effect{};
};
struct Device
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Event> events;
    unsigned frames = 0, attempts = 0, failures = 0;
    bool block = false, entered = false, resume = false;
} device;

SDL_GameController* mockController(SDL_JoystickID id)
{
    return id < 0 ? nullptr : reinterpret_cast<SDL_GameController*>(uintptr_t(id + 1));
}
SDL_bool attached(SDL_GameController*)
{
    return SDL_TRUE;
}
SDL_GameControllerType controllerType(SDL_GameController*)
{
    return SDL_CONTROLLER_TYPE_PS5;
}
void lockJoysticks() {}
void unlockJoysticks() {}
int rumble(SDL_GameController*, Uint16, Uint16, Uint32)
{
    return 0;
}
int sendEffect(SDL_GameController* pad, const void* bytes, int size)
{
    std::unique_lock lock(device.mutex);
    ++device.attempts;
    const bool fail = device.failures != 0;
    if (fail)
        --device.failures;
    if (device.block) {
        device.entered = true;
        device.changed.notify_all();
        if (!device.changed.wait_for(lock, 3s, [] { return device.resume; }))
            throw std::runtime_error("blocked HID not released");
    }
    if (fail) {
        device.changed.notify_all();
        return -1;
    }
    if (size != 47)
        throw std::runtime_error("wrong effect size");
    Event event{ 'H', int(reinterpret_cast<uintptr_t>(pad)) - 1 };
    std::copy_n(static_cast<const std::uint8_t*>(bytes), size, event.effect.begin());
    device.events.push_back(event);
    device.changed.notify_all();
    return 0;
}
}

#define HAVE_PHYSICAL_DS5_HAPTICS
#define MOONLIGHT_DS5_TEST_ENDPOINT MockEndpoint
#define SDL_GameControllerFromInstanceID mockController
#define SDL_GameControllerGetAttached attached
#define SDL_GameControllerGetType controllerType
#define SDL_LockJoysticks lockJoysticks
#define SDL_UnlockJoysticks unlockJoysticks
#define SDL_GameControllerRumble rumble
#define SDL_GameControllerSendEffect sendEffect
#include "streaming/audio/dualsensehaptics.cpp"

namespace {
bool MockEndpoint::start()
{
    std::lock_guard lock(device.mutex);
    m_Started = true;
    device.events.push_back({ 'S' });
    device.changed.notify_all();
    return true;
}
void MockEndpoint::reset()
{
    std::lock_guard lock(device.mutex);
    m_Started = false;
    device.events.push_back({ 'R' });
    device.changed.notify_all();
}
WriteResult MockEndpoint::tryWrite(const Packet& packet)
{
    std::lock_guard lock(device.mutex);
    if (packet.pcm.size() != packet.frameCount * 4u)
        throw std::runtime_error("PCM payload changed");
    device.frames += packet.frameCount;
    device.events.push_back({ 'P' });
    device.changed.notify_all();
    return WriteResult::Ok;
}

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c))                                                                                  \
            throw std::runtime_error("check failed at line " + std::to_string(__LINE__));          \
    } while (false)
template <class Predicate> void await(Predicate predicate)
{
    std::unique_lock lock(device.mutex);
    CHECK(device.changed.wait_for(lock, 2s, predicate));
}
void clear()
{
    std::lock_guard lock(device.mutex);
    device.events.clear();
    device.frames = device.attempts = device.failures = 0;
    device.block = device.entered = device.resume = false;
}
std::vector<Event> snapshot()
{
    std::lock_guard lock(device.mutex);
    return device.events;
}
LI_DS5_HAPTICS_PCM_FRAME frame(std::uint8_t flags, unsigned sequence = 0)
{
    static const std::array<std::uint8_t, 960> samples = [] {
        std::array<std::uint8_t, 960> p{};
        p.fill(0x31);
        return p;
    }();
    LI_DS5_HAPTICS_PCM_FRAME pcm{};
    pcm.flags = flags;
    pcm.sequenceNumber = sequence;
    pcm.sampleRate = 48000;
    pcm.channelCount = 2;
    pcm.bitsPerSample = 16;
    pcm.frameCount = 240;
    pcm.pcmData = samples.data();
    pcm.pcmDataLength = static_cast<std::uint32_t>(samples.size());
    return pcm;
}

void blockedControl()
{
    clear();
    auto renderer =
        std::make_unique<DualSenseHapticsRenderer>(DualSenseHapticsRenderer::Mode::Physical);
    renderer->setControllerTarget(0, 7);
    {
        std::lock_guard lock(device.mutex);
        device.block = true;
    }
    const std::uint8_t native[] = { 0, 0, 0x62, 0, 0, 0, 0, 1 };
    renderer->updateControl(0, native);
    await([] { return device.entered; });
    auto receive = std::async(std::launch::async, [&] {
        const std::uint8_t v2[] = { 2, 4, 0x62, 0, 0, 0x10, 0x23, 1 };
        renderer->updateControl(0, v2);
        dualsense_haptics::ControlState::Effect led{};
        led[1] = 4;
        led[44] = 99;
        CHECK(renderer->submitEffect(0, led.data(), led.size()));
        renderer->submit(frame(3));
    });
    const auto accepted = receive.wait_for(100ms) == std::future_status::ready;
    {
        std::lock_guard lock(device.mutex);
        device.resume = true;
        device.changed.notify_all();
    }
    receive.get();
    CHECK(accepted);
    await([] { return device.frames == 240; });
    auto events = snapshot();
    const auto pcm =
        std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == 'P'; });
    CHECK(pcm != events.end() && pcm != events.begin());
    const auto previous = std::find_if(std::make_reverse_iterator(pcm), events.rend(),
                                       [](const auto& e) { return e.kind == 'H'; });
    CHECK(previous != events.rend());
    const auto& beforePcm = *previous;
    CHECK(beforePcm.kind == 'H' && beforePcm.joystick == 7);
    CHECK(beforePcm.effect[0] == 2 && beforePcm.effect[38] == 4 && beforePcm.effect[2] == 0);
    CHECK(beforePcm.effect[1] == 0x66 && beforePcm.effect[9] == 0x10 &&
          beforePcm.effect[36] == 0x23);
    CHECK(beforePcm.effect[39] == 1 && beforePcm.effect[44] == 99);
    renderer->setControllerTarget(0, 8);
    await([] {
        return !device.events.empty() && device.events.back().kind == 'H' &&
               device.events.back().joystick == 8;
    });
    renderer->reset();
    CHECK(!renderer->hasControl(0));
    await([] { return device.events.back().kind == 'H' && device.events.back().effect[0] == 15; });
    {
        std::lock_guard lock(device.mutex);
        device.resume = device.entered = false;
    }
    renderer->updateControl(0, native);
    await([] { return device.entered; });
    auto stopping =
        std::async(std::launch::async, [owned = std::move(renderer)]() mutable { owned.reset(); });
    const auto waited = stopping.wait_for(50ms) == std::future_status::timeout;
    {
        std::lock_guard lock(device.mutex);
        device.resume = true;
        device.changed.notify_all();
    }
    stopping.get();
    CHECK(waited); // Stop cannot finish while a HID callback still owns the controller.
    const auto final = snapshot().back();
    CHECK(final.kind == 'H' && final.joystick == 8 && final.effect[0] == 15 &&
          final.effect[2] == 0);
    CHECK(final.effect[10] == 5 && final.effect[21] == 5);
}

void shortEffects()
{
    for (int kind = 0; kind < 3; ++kind) {
        clear();
        DualSenseHapticsRenderer renderer(DualSenseHapticsRenderer::Mode::Physical);
        renderer.setControllerTarget(0, 7);
        renderer.submit(frame(kind == 0 ? 3 : 1));
        if (kind == 1) {
            auto end = frame(2, 1);
            end.frameCount = 0;
            end.pcmDataLength = 0;
            renderer.submit(end);
        }
        // kind 2 deliberately omits END, exercising idle prebuffer flushing.
        await([] {
            return std::any_of(device.events.begin(), device.events.end(),
                               [](const auto& e) { return e.kind == 'S'; });
        });
        const auto events = snapshot();
        auto first =
            std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == 'P'; });
        CHECK(first != events.end());
        CHECK(std::none_of(first, events.end(), [](const auto& e) { return e.kind == 'R'; }));
        CHECK(!renderer.hasControl(0)); // Old hosts keep the SDL compatibility path.
    }
}

void failedControlRetriesBeforePcm()
{
    clear();
    DualSenseHapticsRenderer renderer(DualSenseHapticsRenderer::Mode::Physical);
    renderer.setControllerTarget(0, 7);
    {
        std::lock_guard lock(device.mutex);
        device.block = true;
    }
    const std::uint8_t state[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    renderer.updateControl(0, state);
    await([] { return device.entered; });
    // Hold the first successful write so the next iteration sees BOTH state and
    // PCM before its control send fails. No further input drives recovery.
    renderer.updateControl(0, state);
    renderer.submit(frame(3));
    {
        std::lock_guard lock(device.mutex);
        device.failures = 1;
        device.resume = true;
        device.changed.notify_all();
    }
    await([] { return device.frames == 240; });
    const auto events = snapshot();
    auto pcm =
        std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == 'P'; });
    CHECK(pcm != events.end());
    CHECK(std::count_if(events.begin(), pcm, [](const auto& e) { return e.kind == 'H'; }) == 2);
}

void failedStopRetriesWithoutResettingAudio()
{
    clear();
    DualSenseHapticsRenderer renderer(DualSenseHapticsRenderer::Mode::Physical);
    renderer.setControllerTarget(0, 7);
    const std::uint8_t active[] = { 3, 0, 0, 33, 44, 0, 0, 0 };
    renderer.updateControl(0, active);
    renderer.submit(frame(3));
    await([] { return device.frames == 240; });
    const auto beforeStop = snapshot().size();
    {
        std::lock_guard lock(device.mutex);
        device.failures = 1;
    }
    const std::uint8_t stop[] = { 3, 0, 0, 0, 0, 0, 0, 0 };
    renderer.updateControl(0, stop);
    await([] { return device.attempts == 3; });
    const auto events = snapshot();
    CHECK(events.back().kind == 'H' && events.back().effect[0] == 3);
    CHECK(events.back().effect[2] == 0 && events.back().effect[3] == 0);
    CHECK(std::none_of(events.begin() + beforeStop, events.end(),
                       [](const auto& e) { return e.kind == 'R'; }));
    CHECK(
        renderer.hasControl(0)); // A failed native write does not release ownership to SDL rumble.
}

void resetCancelsControlRetry()
{
    clear();
    DualSenseHapticsRenderer renderer(DualSenseHapticsRenderer::Mode::Physical);
    renderer.setControllerTarget(0, 7);
    {
        std::lock_guard lock(device.mutex);
        device.failures = 100;
        device.block = true;
    }
    const std::uint8_t state[8] = {};
    renderer.updateControl(0, state);
    await([] { return device.entered; });
    renderer.submit(frame(3));
    renderer.reset();
    auto fresh = frame(3);
    fresh.frameCount = 120;
    fresh.pcmDataLength = 480;
    renderer.submit(fresh);
    {
        std::lock_guard lock(device.mutex);
        device.resume = true;
        device.changed.notify_all();
    }
    await([] { return device.frames == 120; });
    CHECK(!renderer.hasControl(0));
    {
        std::lock_guard lock(device.mutex);
        CHECK(device.attempts == 1);
    }
}
}

int main()
{
    try {
        blockedControl();
        shortEffects();
        failedControlRetriesBeforePcm();
        failedStopRetriesWithoutResettingAudio();
        resetCancelsControlRetry();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout
        << "DS5 worker ordering, nonblocking acceptance, routing, release and PCM tails passed\n";
    return 0;
}
