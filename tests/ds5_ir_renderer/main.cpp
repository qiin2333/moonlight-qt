#include "streaming/audio/dualsensehapticscalibration.h"

#include <cassert>

int main()
{
    LI_DS5_HAPTICS_IR_FRAME_V2 frame = {};
    frame.lanes[0].rmsAmplitude = 0.5f;
    frame.lanes[0].lowBandRatio = 1.0f;
    frame.lanes[1].rmsAmplitude = 0.4f;
    frame.lanes[1].transientStrength = 1.0f;

    const auto active = dualsense_haptics::renderIrV2(frame);
    assert(active.lowFrequency > 0);
    assert(active.highFrequency > 0);

    frame.flags = LI_DS5_HAPTICS_IR_FLAG_STREAM_END;
    const auto ended = dualsense_haptics::renderIrV2(frame);
    assert(ended.lowFrequency == 0);
    assert(ended.highFrequency == 0);
    return 0;
}
