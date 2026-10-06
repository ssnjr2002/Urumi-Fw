#pragma once
// leg.h — the node's own step pulser, which runs ONE leg (CMD_HOME_LEG,
// docs/homing.md §1.4).
//
// A leg is the base: ramp, budget, span and stop. The mode decides only what
// ends it and what its finish records. The node measures and reports; it does
// not know that legs come in pairs or what a datum is.
#include <stdint.h>
#include "stepper_state.h"

#ifdef HAS_HOMING

enum LegMode : uint8_t {
    LEG_SEEK,      // stop when the switch asserts (debounced); latch on it
    LEG_RETRACT,   // ignore the switch, run the budget out; clear the latch if clear
    LEG_SWEEP,     // run through the Hall index; stop when the window is complete
};

// True while the pulser runs. The stream path reads it from its RX ISR, so it
// is a variable, not a call.
extern volatile bool legActive;

// Validate, convert to ticks, and start the pulser. 0 when armed, else the NAK
// reason; nothing moved.
uint8_t legArm(LegMode mode, bool dir, uint16_t startUs, uint16_t floorUs,
               uint16_t rampSteps, uint32_t maxSteps);

// Stop the pulser. Idempotent; CMD_DISABLE calls it to abort a leg.
void legHalt(void);

// Loop-context half of the leg: finish a stopped one, slice the index resolve,
// publish NODE_FLAG_LEG. Call from node_loop() before the limit flag.
void legLoop(void);

// How far the last completed leg moved, in steps, signed.
int32_t legSpanSteps(void);

#endif  // HAS_HOMING
