#include <cstdint>
#include <initializer_list>
#include <iostream>

using Uint32 = uint32_t;

struct QSize
{
    int w = -1;
    int h = -1;
    QSize() = default;
    QSize(int width, int height) : w(width), h(height) {}
    int width() const { return w; }
    int height() const { return h; }
    bool isValid() const { return w >= 0 && h >= 0; }
    QSize transposed() const { return QSize(h, w); }
    bool operator==(const QSize& other) const { return w == other.w && h == other.h; }
};

struct SDL_Event
{
    int type = 0;
};

constexpr int SDL_LOG_CATEGORY_APPLICATION = 0;
constexpr int SDL_RENDER_DEVICE_RESET = 1;
constexpr int RESOLUTION_CHANGE_ERR_UNSUPPORTED = -2;
#define SDL_TICKS_PASSED(a, b) (static_cast<int32_t>((b) - (a)) <= 0)

Uint32 ticks = 1000;
QSize windowSize(1600, 900);
QSize lastRequest;
int sends = 0;
int sendResult = 0;
int resetEvents = 0;
bool queueFails = false;
bool decoderLocked = false;
bool destroyedUnderLock = false;

Uint32 SDL_GetTicks()
{
    return ticks;
}
#ifdef HARNESS_SDL_BEFORE_2_26
// A scaled display: SDL's window size is logical, half the pixel size
#define SDL_VERSION_ATLEAST(a, b, c) 0
void SDL_GetWindowSize(void*, int* width, int* height)
{
    *width = windowSize.width() / 2;
    *height = windowSize.height() / 2;
}
void SDL_GL_GetDrawableSize(void*, int* width, int* height)
{
    *width = windowSize.width();
    *height = windowSize.height();
}
#else
#define SDL_VERSION_ATLEAST(a, b, c) 1
void SDL_GetWindowSizeInPixels(void*, int* width, int* height)
{
    *width = windowSize.width();
    *height = windowSize.height();
}
#endif
void SDL_LogInfo(int, const char*, ...) {}
void SDL_LogWarn(int, const char*, ...) {}
int LiRequestResolutionChange(int width, int height)
{
    ++sends;
    lastRequest = QSize(width, height);
    return sendResult;
}
int SDL_PushEvent(SDL_Event* event)
{
    if (queueFails) {
        return -1;
    }
    resetEvents += event->type == SDL_RENDER_DEVICE_RESET;
    return 1;
}
const char* SDL_GetError()
{
    return "test queue failure";
}
void SDL_LockMutex(void*)
{
    decoderLocked = true;
}
void SDL_UnlockMutex(void*)
{
    decoderLocked = false;
}

struct Decoder
{
    ~Decoder() { destroyedUnderLock = decoderLocked; }
};

class Session
{
public:
    struct Preferences
    {
        bool resolutionMatchWindow = false;
    } preferences;
    Preferences* m_Preferences = &preferences;
    struct Config
    {
        int width = 1280;
        int height = 720;
    } m_StreamConfig;
    int m_ActiveVideoWidth = 1280;
    int m_ActiveVideoHeight = 720;
    void* m_Window = nullptr;
    void* m_DecoderLock = nullptr;
    Decoder* m_VideoDecoder = new Decoder;
    bool fullscreen = false;
    bool minimized = false;
    // RESOLUTION_STATE
    ~Session() { delete m_VideoDecoder; }
    bool isStreamingWindowFullscreen() const { return fullscreen; }
    bool isStreamingWindowMinimized() const { return minimized; }
    void scheduleWindowResolutionMatch();
    void processWindowResolutionMatch();
    void handleHostResolutionReport(int width, int height);
};

// SESSION_METHODS

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

// Starts a connection whose host reported a 1:1 display at stream start, unless told otherwise
void prepare(Session& session, bool hostStartsStreaming1To1 = true)
{
    ticks = 1000;
    sendResult = 0;
    queueFails = destroyedUnderLock = false;
    windowSize = QSize(1600, 900);
    session.preferences.resolutionMatchWindow = true;
    session.m_ResolutionMatch.hostSupported = true;
    if (hostStartsStreaming1To1) {
        session.handleHostResolutionReport(1280, 720);
    }
    sends = resetEvents = 0;
}

int main()
{
    bool ok = true;
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        session.m_ResolutionMatch.deadline = 1;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deadline == 0,
                      "disabled preference with a pending deadline must send nothing");
    }
    {
        Session session;
        prepare(session);
        session.m_ResolutionMatch.hostSupported = false;
        session.m_ResolutionMatch.deadline = 1;
        session.processWindowResolutionMatch();
        session.scheduleWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deadline == 0,
                      "unsupported hosts must never send or schedule a request");
        session.handleHostResolutionReport(1600, 900);
        ok &= require(resetEvents == 1 && session.m_ActiveVideoWidth == 1600 &&
                          session.m_ActiveVideoHeight == 900,
                      "an unsupported host's own display change must still reset the decoder");
    }
    {
        Session session;
        prepare(session);
        session.scheduleWindowResolutionMatch();
        ticks = 1399;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0, "resize must debounce for 400 ms");
        session.scheduleWindowResolutionMatch();
        ticks = 1799;
        windowSize = QSize(1601, 901);
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(1600, 900),
                      "settled resize must send the latest even pixel size");
        windowSize = QSize(1920, 1080);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "an unanswered request must prevent another send");
        ticks += 5000;
        session.processWindowResolutionMatch();
        ok &= require(sends == 2 && lastRequest == windowSize, "request timeout must unblock");
        session.scheduleWindowResolutionMatch();
        ticks += 5500;
        session.processWindowResolutionMatch();
        ok &= require(sends == 2, "an ignored request must not be sent repeatedly");
    }
    {
        Session session;
        prepare(session);
        sendResult = RESOLUTION_CHANGE_ERR_UNSUPPORTED;
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && !session.m_ResolutionMatch.hostSupported,
                      "unsupported response must disable subsequent requests");
    }
    {
        Session session;
        prepare(session, false);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deferred,
                      "a resize before the host's stream-start report must defer");
        session.handleHostResolutionReport(1280, 720);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "a 1:1 stream-start report must retry the deferred resize");
    }
    {
        // The host keeps its 1/2 stream-start scale, so its stream never follows its display
        Session session;
        prepare(session, false);
        session.handleHostResolutionReport(2560, 1440);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        session.handleHostResolutionReport(1280, 720);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0, "a scaled stream must never request a resolution");
        session.handleHostResolutionReport(1920, 1080);
        ok &= require(resetEvents == 0 && session.m_ActiveVideoWidth == 1280 &&
                          session.m_ActiveVideoHeight == 720,
                      "a scaled stream's display reports must not change the stream resolution");
    }
    {
        Session session;
        prepare(session, false);
        session.handleHostResolutionReport(720, 1280);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(resetEvents == 0 && sends == 1,
                      "a rotated 1:1 stream-start report must allow requests without a reset");
    }
    {
        Session session;
        prepare(session, false);
        session.preferences.resolutionMatchWindow = false;
        session.handleHostResolutionReport(1280, 720);
        ok &= require(resetEvents == 0 && session.m_VideoDecoder,
                      "first connection report only records display size");
        queueFails = true;
        session.handleHostResolutionReport(1600, 900);
        ok &= require(resetEvents == 0 && !session.m_VideoDecoder && destroyedUnderLock &&
                          session.m_ActiveVideoWidth == 1600 && session.m_ActiveVideoHeight == 900,
                      "host-initiated changes must replace the decoder under lock even with "
                      "matching off");
        queueFails = false;
        session.processWindowResolutionMatch();
        ok &= require(resetEvents == 1,
                      "the main loop must retry a decoder reset that couldn't be queued");
        session.processWindowResolutionMatch();
        ok &= require(resetEvents == 1, "a queued decoder reset must not be repeated");
    }
    {
        Session session;
        prepare(session);
        session.handleHostResolutionReport(1600, 900);
        session.fullscreen = true;
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1 && lastRequest == QSize(1280, 720),
                      "fullscreen must return to the configured resolution");
    }
    for (QSize invalid : { QSize(255, 900), QSize(1600, 16386) }) {
        Session session;
        prepare(session);
        windowSize = invalid;
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0, "invalid dimensions must never send");
    }
    return ok ? 0 : 1;
}
