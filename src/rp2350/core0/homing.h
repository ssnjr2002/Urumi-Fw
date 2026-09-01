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
// `intendedRetract` is the host's own prediction of what this leg is: true for
// a leg the plan expects to end already on the switch (§3.4's legs 2 and 4),
// false for one it expects to start clear (legs 1 and 3). The node checks it
// against its own pin read and NAKs on disagreement (NAK_INTENT_MISMATCH,
// include/common.h) rather than silently running the wrong leg's semantics
// under the right leg's budget.
bool homingBegin(uint8_t node, uint8_t dir, bool intendedRetract,
                 uint16_t startUs, uint16_t floorUs,
                 uint16_t rampSteps, uint32_t maxSteps);

// Poll a home in progress. No-op unless STATE_HOMING and a home is claimed.
// Call from the Core 0 loop.
void homingTick(void);

// True while this module holds a home. Lets the gates refuse a second `home`
// without reading machineState, which anything may write.
bool homingActive(void);

// The latch mask itself lives in position.h, beside the datum it mirrors: a
// limit switch belongs to a NODE, so the truth is node-framed and the per-slot
// view is re-derived on every bind. Homing writes it via nodeLatchSet() and
// reads the derived `homingLatched` here.
//
// Named for homing rather than `axes_latched` because the invariant is
// specifically about the four-leg sequence: a switch pressed by a crash mid-job
// does NOT set a bit, and nothing here pretends otherwise. The host's preflight
// owns that case (docs/homing.md §6.6).

// Leave a completed leg in the right state: ALARM/LIMIT_LATCHED while any axis
// is still standing on a switch, IDLE once none is.
//
// DERIVED, never assigned, and that is the whole point. Legs 1 and 3 end
// latched and legs 2 and 4 end clear, so a sequence that wrote the state
// directly at each leg would be correct only until two axes homed with
// interleaved legs -- then clearing one switch would report IDLE with the
// other still down. Recomputing from the mask cannot produce that.
void resumeOrHold(void);
