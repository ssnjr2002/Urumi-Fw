#pragma once
#include <stdint.h>
#include "leg.h"

// leg_effects.h — what a leg's end does to the machine, by the intent it was
// armed with, and whether a step's owner still holds the machine. The runner
// calls these; the Pico implementation reaches position.h and shared state,
// and the native test fakes them.

// What the operator asked a leg to do. The node only knows its pin, so the
// intent is kept beside the leg, not read back from it.
enum LegIntent : uint8_t {
    LEG_SEEK,      // onto the switch
    LEG_RETRACT,   // off the switch
    LEG_SWEEP,     // rotary index
    LEG_PARK,      // to a node counter target, within the datum
    LEG_DUMMY,     // no node, no motion
};

// A leg ended with a verdict (legPoll): success records the latch, then the
// origin is released.
void legEffectEnd(uint8_t intent, const LegEnd& e);

// A leg ended without a verdict (abort, drop): the origin is released only.
void legEffectRelease(uint8_t intent, const LegEnd& e);

// True while the machine is still in `owner`, the state the step runs in.
bool legOwnerHolds(uint8_t owner);
