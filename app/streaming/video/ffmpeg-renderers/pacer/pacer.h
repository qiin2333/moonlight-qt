#pragma once

#include "../../decoder.h"
#include "../renderer.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};

// VRR frame timing scheduler for stable frame submission
class VrrFrameScheduler {
public:
    VrrFrameScheduler(int targetFps);
    ~VrrFrameScheduler();
    
    // Calculate optimal frame submission timing
    void scheduleFrame();
    
    // Wait until the optimal time to submit the next frame
    void waitForOptimalSubmissionTime();
    
    // Update timing statistics and adjust scheduling
    void recordFrameSubmission();
    
    // Reset timing state (e.g., after decoder recreation)
    void reset();
    
private:
    void updateTimingStatistics();
    void adjustSchedulingParameters();
    
    int m_TargetFps;
    Uint64 m_TargetFrameIntervalNs;  // Target frame interval in nanoseconds
    Uint64 m_LastFrameTimeNs;        // Last frame submission time
    Uint64 m_NextFrameTimeNs;        // Next optimal frame submission time
    
    // Timing statistics for adaptive adjustment
    QQueue<Uint64> m_FrameTimeHistory;  // Recent frame submission times
    double m_AverageFrameInterval;       // Rolling average frame interval
    double m_FrameIntervalVariance;      // Frame timing variance
    
    // Adaptive parameters
    double m_TimingAdjustmentFactor;     // Factor for timing corrections
    int m_HistorySize;                   // Number of frames to track for statistics
    
    mutable QMutex m_TimingMutex;
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncMillis);

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);
    
    // VRR-specific methods
    void submitFrameForVrr(AVFrame* frame);
    void scheduleVrrFrame(AVFrame* frame);

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;
    
    // VRR timing control
    VrrFrameScheduler* m_VrrScheduler;
    bool m_VrrModeEnabled;
};
