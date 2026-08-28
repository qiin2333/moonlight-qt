#include "dualsensehapticsmac.h"

#include "dualsensehapticscalibration.h"

#include "SDL_compat.h"

#include <algorithm>
#include <mutex>

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

@interface MoonlightDualSenseHapticState : NSObject
@property(nonatomic, strong) GCController* controller;
@property(nonatomic, strong) CHHapticEngine* leftEngine;
@property(nonatomic, strong) CHHapticEngine* rightEngine;
@property(nonatomic, strong) id<CHHapticPatternPlayer> leftPlayer;
@property(nonatomic, strong) id<CHHapticPatternPlayer> rightPlayer;
@end

@implementation MoonlightDualSenseHapticState
- (void)dealloc
{
    [_controller release];
    [_leftEngine release];
    [_rightEngine release];
    [_leftPlayer release];
    [_rightPlayer release];
    [super dealloc];
}
@end

namespace {

bool isDualSenseController(GCController* controller)
{
    return controller != nil &&
           [controller.extendedGamepad isKindOfClass:[GCDualSenseGamepad class]];
}

GCController* findDualSense(std::uint16_t controllerNumber)
{
    NSMutableArray<GCController*>* fallbackControllers = [NSMutableArray array];
    for (GCController* controller in GCController.controllers) {
        if (!isDualSenseController(controller)) {
            continue;
        }

        if (controller.playerIndex != GCControllerPlayerIndexUnset &&
            static_cast<NSInteger>(controller.playerIndex) == controllerNumber) {
            return controller;
        }
        [fallbackControllers addObject:controller];
    }

    // SDL and GameController share the player index on macOS. Some controllers
    // remain unassigned until input begins, so retain a deterministic fallback
    // for the common single-controller case and for initially unassigned pads.
    if (controllerNumber < fallbackControllers.count) {
        return fallbackControllers[controllerNumber];
    }
    return nil;
}

id<CHHapticPatternPlayer> createPlayer(CHHapticEngine* engine, NSError** error)
{
    CHHapticEventParameter* intensity =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                     value:1.0f];
    CHHapticEventParameter* sharpness =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness
                                                     value:0.5f];
    CHHapticEvent* event =
        [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                     parameters:@[intensity, sharpness]
                                   relativeTime:0
                                       duration:GCHapticDurationInfinite];
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event]
                                                            parameters:@[]
                                                                 error:error];
    [intensity release];
    [sharpness release];
    [event release];
    if (pattern == nil) {
        return nil;
    }

    id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:error];
    [pattern release];
    if (player == nil || ![player startAtTime:CHHapticTimeImmediate error:error]) {
        return nil;
    }
    return player;
}

bool startHandle(GCDeviceHaptics* haptics, GCHapticsLocality locality,
                 CHHapticEngine** engineOut, id<CHHapticPatternPlayer>* playerOut)
{
    if (![haptics.supportedLocalities containsObject:locality]) {
        return false;
    }

    CHHapticEngine* engine = [haptics createEngineWithLocality:locality];
    NSError* error = nil;
    if (engine == nil || ![engine startAndReturnError:&error]) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                    "Unable to start macOS DualSense haptic engine: %s",
                    error.localizedDescription.UTF8String ?: "unknown error");
        return false;
    }

    id<CHHapticPatternPlayer> player = createPlayer(engine, &error);
    if (player == nil) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                    "Unable to create macOS DualSense haptic player: %s",
                    error.localizedDescription.UTF8String ?: "unknown error");
        [engine stopWithCompletionHandler:nil];
        return false;
    }

    *engineOut = engine;
    *playerOut = player;
    return true;
}

MoonlightDualSenseHapticState* createState(GCController* controller)
{
    GCDeviceHaptics* haptics = controller.haptics;
    if (haptics == nil) {
        return nil;
    }

    MoonlightDualSenseHapticState* state = [[MoonlightDualSenseHapticState alloc] init];
    state.controller = controller;
    CHHapticEngine* leftEngine = nil;
    CHHapticEngine* rightEngine = nil;
    id<CHHapticPatternPlayer> leftPlayer = nil;
    id<CHHapticPatternPlayer> rightPlayer = nil;
    if (!startHandle(haptics, GCHapticsLocalityLeftHandle,
                     &leftEngine, &leftPlayer)) {
        [state release];
        return nil;
    }
    if (!startHandle(haptics, GCHapticsLocalityRightHandle,
                     &rightEngine, &rightPlayer)) {
        NSError* error = nil;
        [leftPlayer stopAtTime:CHHapticTimeImmediate error:&error];
        [leftEngine stopWithCompletionHandler:nil];
        [state release];
        return nil;
    }
    state.leftEngine = leftEngine;
    state.rightEngine = rightEngine;
    state.leftPlayer = leftPlayer;
    state.rightPlayer = rightPlayer;

    SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO,
                "macOS native DualSense haptics ready for player %d",
                static_cast<int>(controller.playerIndex));
    return [state autorelease];
}

bool updatePlayer(id<CHHapticPatternPlayer> player,
                  const dualsense_haptics::NativeHapticLaneOutput& output,
                  NSError** error)
{
    CHHapticDynamicParameter* intensity =
        [[CHHapticDynamicParameter alloc]
            initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                           value:std::clamp(output.intensity, 0.0f, 1.0f)
                    relativeTime:0];
    CHHapticDynamicParameter* sharpness =
        [[CHHapticDynamicParameter alloc]
            initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl
                           value:std::clamp(output.sharpness, 0.0f, 1.0f) - 0.5f
                    relativeTime:0];
    const bool updated = [player sendParameters:@[intensity, sharpness]
                                         atTime:CHHapticTimeImmediate
                                          error:error];
    [intensity release];
    [sharpness release];
    return updated;
}

void stopState(MoonlightDualSenseHapticState* state)
{
    NSError* error = nil;
    [state.leftPlayer stopAtTime:CHHapticTimeImmediate error:&error];
    error = nil;
    [state.rightPlayer stopAtTime:CHHapticTimeImmediate error:&error];
    [state.leftEngine stopWithCompletionHandler:nil];
    [state.rightEngine stopWithCompletionHandler:nil];
}

} // namespace

struct MacDualSenseHapticsRenderer::Impl
{
    std::mutex mutex;
    NSMutableDictionary<NSNumber*, MoonlightDualSenseHapticState*>* states =
        [[NSMutableDictionary alloc] init];

    ~Impl()
    {
        for (MoonlightDualSenseHapticState* state in states.allValues) {
            stopState(state);
        }
        [states release];
    }

    bool submit(const LI_DS5_HAPTICS_IR_FRAME_V2& frame)
    {
        std::lock_guard lock(mutex);
        NSNumber* key = @(frame.controllerNumber);
        MoonlightDualSenseHapticState* state = states[key];

        GCController* controller = findDualSense(frame.controllerNumber);
        if (state != nil && state.controller != controller) {
            stopState(state);
            [states removeObjectForKey:key];
            state = nil;
        }

        if (frame.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END) {
            if (state != nil) {
                stopState(state);
                [states removeObjectForKey:key];
                return true;
            }
            return false;
        }

        if (state == nil) {
            if (controller == nil) {
                return false;
            }
            state = createState(controller);
            if (state == nil) {
                return false;
            }
            states[key] = state;
        }

        const auto output = dualsense_haptics::renderIrV2Native(frame);
        NSError* error = nil;
        if (!updatePlayer(state.leftPlayer, output.left, &error) ||
            !updatePlayer(state.rightPlayer, output.right, &error)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                        "Unable to update macOS DualSense haptics: %s",
                        error.localizedDescription.UTF8String ?: "unknown error");
            stopState(state);
            [states removeObjectForKey:key];
            return false;
        }
        return true;
    }
};

MacDualSenseHapticsRenderer::MacDualSenseHapticsRenderer() :
    m_Impl(std::make_unique<Impl>())
{
}

MacDualSenseHapticsRenderer::~MacDualSenseHapticsRenderer() = default;

bool MacDualSenseHapticsRenderer::submit(const LI_DS5_HAPTICS_IR_FRAME_V2& frame)
{
    @autoreleasepool {
        return m_Impl->submit(frame);
    }
}
