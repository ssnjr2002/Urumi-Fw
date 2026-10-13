#pragma once
#include <stdint.h>
#include "leg.h"

// homing.h — homing legs and the manual homing session (docs/homing.md §2.3).
//
// homingArm and homingArmPark arm a homing leg into the live step, in no
// context; a sequence (`home`, `recovery`) uses them. A failed leg fails its
// sequence (homingFail): every other running leg is aborted and the machine
// alarms with F_HOMING. Running and judging a leg is leg.h's.
//
// The manual session: each raw leg is a one-step `leg` sequence in the homing
// context, which stays open across legs (homingReason says whether one runs)
// until `setorigin` or `home_end`, an estop or F_HOMING closes it.

// Arm a seek, retract or sweep leg (legArm) into the live step, or open one.
// Returns nullptr, or legArm's refusal text with nothing moved.
const char* homingArm(uint8_t node, uint8_t expectKind, uint8_t dir,
                      bool intendedRetract,
                      uint16_t startUs, uint16_t floorUs,
                      uint16_t rampSteps, uint32_t maxSteps,
                      bool* intentMismatch = nullptr);

// Arm a park leg (legArmPark), as homingArm. A finished park keeps the node's
// origin; the caller checks it is homed.
const char* homingArmPark(uint8_t node, int32_t target, uint16_t startUs,
                          uint16_t floorUs, uint16_t rampSteps);

// Arm an unlatch leg (off the switch, within the datum), as homingArm.
const char* homingArmUnlatch(uint8_t node, uint8_t dir, uint16_t startUs,
                             uint16_t floorUs, uint16_t rampSteps, uint32_t maxSteps);

// Start ONE raw LEG on `node` as homingArm, as a `leg` sequence; the first
// opens the session. Returns as homingArm. Prints nothing: the caller replies.
const char* homingBegin(uint8_t node, uint8_t expectKind, uint8_t dir,
                        bool intendedRetract,
                        uint16_t startUs, uint16_t floorUs,
                        uint16_t rampSteps, uint32_t maxSteps,
                        bool* intentMismatch = nullptr);

// A raw park leg in the session, as homingBegin.
const char* homingParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                            uint16_t floorUs, uint16_t rampSteps);

// leg_abort: stop `node`'s pulser. If it ran a homing leg, that leg fails as
// LEGFAIL_ABORTED, failing its sequence; any other node just acks. Returns as
// homingArm.
const char* homingAbort(uint8_t node);

// A dummy leg (legArmDummy) in the session. Touches no latch and no origin.
// The caller gates it as a leg. Returns as homingBegin.
const char* homingDummyBegin(bool succeed, uint32_t ms);

// The cycle a `home` run is in, for get's homecycle=; 0 when none runs.
void homingHold(uint8_t cycle);
uint8_t homingHeld(void);

// Fail the live sequence on a leg's end: abort the other legs, record
// homefail=/homenode= and alarm with F_HOMING.
void homingFail(const LegEnd& e);

// Fail the live sequence on `node` for a cause found outside a leg, or a
// traverse leg (LEGFAIL_*): as homingFail, and `node`'s origin is dropped.
void homingAbandon(uint8_t node, uint8_t why);

// The session is open and no leg runs (HOMING_WAIT): the next leg, `setorigin`
// or `home_end` may follow, and the bus is free.
bool homingWaiting(void);

// The node whose leg failed (0 for a dummy). Meaningful as homingFailWhy.
uint8_t homingFailNode(void);

// Why the last leg failed, LEGFAIL_*. F_HOMING says THAT one did;
// this says which fault, because they are diagnosed in completely different
// places. Meaningful only while F_HOMING is set; reset at the
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
