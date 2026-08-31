#pragma once
#include <stdint.h>

// homing.h — Core 0's supervisor for a node-run home (docs/homing.md §2.3).
//
// The node owns the stop and the motion; this owns only the WAITING. It exists
// because the control plane's contract is one reply line per command and a home
// takes seconds: a handler that blocked until the pulser stopped would freeze
// `getstate`, `stop` and every abort for the whole seek, on a command that is
// driving an axis into a hard stop.
//
// So `home` arms and returns, the machine sits in STATE_HOMING, and the polling
// lives out here in the Core 0 loop where it cannot hold the plane shut.

// Arm a home on `node` and enter STATE_HOMING. Prints exactly one reply line in
// every path, per the control-plane contract. Returns true if the command was
// answered at all -- which is what the handler propagates -- not whether the
// axis found its switch.
bool homingBegin(uint8_t node, uint8_t dir, uint16_t startUs, uint16_t floorUs,
                 uint16_t rampSteps, uint32_t maxSteps);

// Poll a home in progress. No-op unless STATE_HOMING and a home is claimed.
// Call from the Core 0 loop.
void homingTick(void);

// True while this module holds a home. Lets the gates refuse a second `home`
// without reading machineState, which anything may write.
bool homingActive(void);
