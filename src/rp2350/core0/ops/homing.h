#pragma once
#include <stdint.h>
#include "leg.h"

// homing.h — the homing session around node-run legs (docs/homing.md §2.3).
//
// The machine stays in STATE_HOMING across legs (a session); homingReason says
// whether a leg runs. `setorigin` and `home_end` close it, as do estop and
// ALARM_HOMING_FAIL. Running and judging one leg is leg.h's.

// Arm ONE LEG on `node` (legArm); the first opens the session. Prints exactly
// one reply line in every path, per the control-plane contract. Returns true if
// the command was answered at all -- which is what the handler propagates --
// not whether the leg found anything.
bool homingBegin(uint8_t node, uint8_t expectKind, uint8_t dir,
                 bool intendedRetract,
                 uint16_t startUs, uint16_t floorUs,
                 uint16_t rampSteps, uint32_t maxSteps);

// Poll a leg in progress and turn its end into the session's state. No-op
// unless a leg is armed. Call from the Core 0 loop.
void homingTick(void);

// A dummy leg (legArmDummy) in the session. Touches no latch and no origin.
// The caller gates it as a leg.
void homingDummyBegin(bool succeed, uint32_t ms);

// True while a leg runs. Lets the gates refuse a second leg without reading
// machineState, which anything may write.
bool homingActive(void);

// The session is open and no leg runs (HOMING_WAIT): the next leg, `setorigin`
// or `home_end` may follow, and the bus is free.
bool homingWaiting(void);

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
