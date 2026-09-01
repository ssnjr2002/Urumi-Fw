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

// Why the last home failed. ALARM_HOMING_FAIL says THAT one did; this says
// which of three unrelated faults it was, because they are diagnosed in
// completely different places and the host was otherwise reduced to guessing
// (it assumed BUDGET and printed the planned max_steps, which reads as an
// accusation against the switch even when the leg died 190k steps short of it).
#define HOMEFAIL_NONE     0
#define HOMEFAIL_BUDGET   1   // node stopped itself, switch never asserted --
                              // a real "never reached": bad travel figure,
                              // wrong approach direction, or a dead switch
#define HOMEFAIL_POLL     2   // the node stopped ANSWERING mid-leg (4 in a row).
                              // Says nothing about the axis; the bus is the
                              // suspect. The motion may well have been fine.
#define HOMEFAIL_DEADLINE 3   // still pulsing past the supervisor's own timeout.
                              // The node's budget should have stopped it first,
                              // so this points at the pulser, not the switch.

// True while this module holds a home. Lets the gates refuse a second `home`
// without reading machineState, which anything may write.
bool homingActive(void);

// Why the last home failed, HOMEFAIL_*. Meaningful only while alarmReason is
// ALARM_HOMING_FAIL; reset at the next arm.
uint8_t homingFailWhy(void);

// How far the last COMPLETED leg moved, in the node's own steps. False if no leg
// has finished since boot, or if the last one failed -- see homing.cpp for why a
// failed leg's distance is deliberately not offered.
//
// Reports the endpoints, not a distance: `home` cannot know which leg of a
// sequence it is running, so the caller is the one that knows this was the seek
// and that its span is the frame. Signed subtraction is theirs to do, and the
// sign is worth having -- it catches an approach direction that ran the wrong
// way. This is the MEASUREMENT; deciding it means max travel is the operator's
// call, and depends on them having started at the far end (docs/homing.md §7).
// `wasSeek` distinguishes the two kinds of leg, and without it the number is
// anonymous: after a full four-leg home this reports leg 4's 5 mm park retract,
// which looks exactly like a frame measurement to anyone reading it by hand.
// Only a SEEK measures anything the host did not already command -- a retract
// travels precisely the max_steps it was given, so its span is an echo.
bool homingLastSpan(uint8_t* node, int32_t* from, int32_t* to, bool* wasSeek);

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
