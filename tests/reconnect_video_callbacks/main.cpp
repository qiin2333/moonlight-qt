// Reproduces an in-session reconnect with a pull renderer: the first
// LiStartConnection() runs moonlight-common-c's real fixupMissingCallbacks()
// on the session's callbacks, and the second connection must still satisfy
// LiStartConnection()'s "no submitDecodeUnit with CAPABILITY_PULL_RENDERER"
// precondition.

#include "streaming/videocallbacks.h"

#include <cstdio>

extern "C" void fixupMissingCallbacks(PDECODER_RENDERER_CALLBACKS* drCallbacks,
                                      PAUDIO_RENDERER_CALLBACKS* arCallbacks,
                                      PCONNECTION_LISTENER_CALLBACKS* clCallbacks);

static int realSubmit(PDECODE_UNIT)
{
    return DR_OK;
}

static bool startConnectionAccepts(const DECODER_RENDERER_CALLBACKS& callbacks)
{
    // Same check as LiStartConnection()
    return !((callbacks.capabilities & CAPABILITY_PULL_RENDERER) && callbacks.submitDecodeUnit);
}

static void simulateLiStartConnection(DECODER_RENDERER_CALLBACKS* callbacks)
{
    AUDIO_RENDERER_CALLBACKS audio = {};
    CONNECTION_LISTENER_CALLBACKS listener = {};
    PDECODER_RENDERER_CALLBACKS dr = callbacks;
    PAUDIO_RENDERER_CALLBACKS ar = &audio;
    PCONNECTION_LISTENER_CALLBACKS cl = &listener;
    fixupMissingCallbacks(&dr, &ar, &cl);
}

int main()
{
    int failures = 0;

    DECODER_RENDERER_CALLBACKS pull = {};
    pull.capabilities = CAPABILITY_PULL_RENDERER;
    for (int connection = 1; connection <= 3; connection++) {
        prepareVideoCallbacksForConnection(&pull);
        if (!startConnectionAccepts(pull)) {
            std::printf("FAIL: pull renderer rejected on connection %d\n", connection);
            failures++;
        }
        simulateLiStartConnection(&pull);
    }

    // A push renderer keeps its real callback
    DECODER_RENDERER_CALLBACKS push = {};
    push.submitDecodeUnit = realSubmit;
    simulateLiStartConnection(&push);
    prepareVideoCallbacksForConnection(&push);
    if (push.submitDecodeUnit != realSubmit) {
        std::printf("FAIL: push renderer lost its submitDecodeUnit\n");
        failures++;
    }

    if (failures == 0) {
        std::printf("Reconnect video callback tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
