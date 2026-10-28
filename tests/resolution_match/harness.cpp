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
void SDL_GetWindowSizeInPixels(void*, int* width, int* height)
{
    *width = windowSize.width();
    *height = windowSize.height();
}
#define SDL_VERSION_ATLEAST(a, b, c) 1
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

void prepare(Session& session)
{
    ticks = 1000;
    sends = resetEvents = 0;
    sendResult = 0;
    queueFails = destroyedUnderLock = false;
    windowSize = QSize(1600, 900);
    session.preferences.resolutionMatchWindow = true;
    session.m_ResolutionMatch.hostSupported = true;
    session.m_ResolutionMatch.hostDisplaySize = QSize(1280, 720);
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
        session.handleHostResolutionReport(1600, 900);
        ok &= require(sends == 0 && session.m_ResolutionMatch.deadline == 0 && resetEvents == 0,
                      "unsupported hosts must never send, schedule or reset a decoder");
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
        prepare(session);
        session.m_ResolutionMatch.hostDisplaySize = QSize(1920, 1080);
        session.scheduleWindowResolutionMatch();
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 0 && session.m_ResolutionMatch.deferred,
                      "non-1:1 host display must defer rather than send");
        session.handleHostResolutionReport(1280, 720);
        ticks += 400;
        session.processWindowResolutionMatch();
        ok &= require(sends == 1, "a 1:1 display report must retry the deferred resize");
    }
    {
        Session session;
        prepare(session);
        session.preferences.resolutionMatchWindow = false;
        session.m_ResolutionMatch.hostDisplaySize = QSize();
        session.handleHostResolutionReport(1280, 720);
        ok &= require(resetEvents == 0 && session.m_VideoDecoder,
                      "first connection report only records display size");
        queueFails = true;
        session.handleHostResolutionReport(1600, 900);
        ok &= require(resetEvents == 0 && session.m_VideoDecoder &&
                          session.m_ActiveVideoWidth == 1280,
                      "failed reset enqueue must preserve the decoder and dimensions");
        queueFails = false;
        session.handleHostResolutionReport(1600, 900);
        ok &= require(resetEvents == 1 && !session.m_VideoDecoder && destroyedUnderLock &&
                          session.m_ActiveVideoWidth == 1600 && session.m_ActiveVideoHeight == 900,
                      "host-initiated changes must reset under lock even with matching off");
    }
    {
        Session session;
        prepare(session);
        session.m_ActiveVideoWidth = 1600;
        session.m_ActiveVideoHeight = 900;
        session.m_ResolutionMatch.hostDisplaySize = QSize(1600, 900);
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
