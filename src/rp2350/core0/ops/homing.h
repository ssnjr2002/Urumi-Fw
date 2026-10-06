#pragma once
#include <stdint.h>
#include "leg.h"

// homing.h — the homing session around node-run legs (docs/homing.md §2.3).
//
// The machine stays in STATE_HOMING across legs (a session); homingReason says
// whether any leg runs. Legs on different nodes run in parallel; a failure
// aborts every other running leg (the cycle) and alarms. `setorigin` and
// `home_end` close it, as do estop and ALARM_HOMING_FAIL. Running and judging
// a leg is leg.h's.

// Arm ONE LEG on `node` (legArm); the first opens the session. Returns nullptr,
// or legArm's refusal text with nothing moved. Prints nothing: the caller
// replies.
const char* homingBegin(uint8_t node, uint8_t expectKind, uint8_t dir,
                        bool intendedRetract,
                        uint16_t startUs, uint16_t floorUs,
                        uint16_t rampSteps, uint32_t maxSteps);

// Arm a park leg (legArmPark) in the session, as homingBegin. A finished park
// keeps the node's origin; the caller checks it is homed.
const char* homingParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                            uint16_t floorUs, uint16_t rampSteps);

// leg_abort: stop `node`'s pulser. If it ran a leg, that leg fails as
// LEGFAIL_ABORTED, failing its cycle; any other node just acks. Returns as
// homingBegin.
const char* homingAbort(uint8_t node);

// Poll the legs in progress and turn each end into the session's state. No-op
// unless a leg is armed. Call from the Core 0 loop.
void homingTick(void);

// A dummy leg (legArmDummy) in the session. Touches no latch and no origin.
// The caller gates it as a leg. Returns as homingBegin.
const char* homingDummyBegin(bool succeed, uint32_t ms);

// True while any leg runs. Lets the gates refuse `setorigin` and `home_end`
// without reading machineState, which anything may write.
bool homingActive(void);

// The session is open and no leg runs (HOMING_WAIT): the next leg, `setorigin`
// or `home_end` may follow, and the bus is free.
bool homingWaiting(void);

// The node whose leg failed (0 for a dummy). Meaningful as homingFailWhy.
uint8_t homingFailNode(void);

// Why the last leg failed, LEGFAIL_*. ALARM_HOMING_FAIL says THAT one did;
// this says which fault, because they are diagnosed in completely different
// places. Meaningful only while alarmReason is ALARM_HOMING_FAIL; reset at the
// next arm.
uint8_t homingFailWhy(void);

// The latch mask itself lives in position.h, beside the datum it mirrors: a
// limit switch belongs to a NODE, so the truth is node-framed and the per-slot
// view is re-derived on every bind. Homing writes it via nodeLatchSet() and
// reads the derived `homingLatched` here.
//
// Named for homing rather than `axes_latched` because the invariant is
// specifically about the four-leg sequence: a switch pressed by a crash mid-job
// does NOT set a bit, and nothing here pretends otherwise. The host's preflight
// owns that case (docs/homing.md §6.6).
