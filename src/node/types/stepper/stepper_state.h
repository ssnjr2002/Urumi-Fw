#pragma once
// stepper_state.h — state shared between the stream path (stepper.cpp) and the
// leg pulser (leg.cpp). Private to the stepper type. Plain globals rather than
// accessors: both ISRs read them, and a call into another file would make the
// ISR save every call-clobbered register.
#include <stdint.h>
#include "board.h"
#include "stepper/stepper.h"

// The node's one step counter: the stream and the pulser both count into it.
extern volatile int32_t absolutePosition;
// Last level driven on DIR, by either path.
extern bool currentDir;

#ifdef HAS_LIMIT_SWITCH
// The limit gate (stepper.cpp). A leg writes the latch: a seek that stopped on
// its switch sets it, a retract that got clear clears it.
extern volatile uint32_t limitBytesAsserted;
extern volatile uint32_t limitRunBase;
extern volatile bool     limitLatched;
#endif
