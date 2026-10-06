#pragma once
#include <stdint.h>

// leg.h — Core 0's supervisor for one node-run leg (docs/homing.md §2.3).
//
// The node owns the stop and the motion; this owns only the WAITING. A leg
// takes seconds and the control plane answers one line per command, so the
// handler arms and returns and the polling lives in the Core 0 loop, where it
// cannot hold the plane shut. What a leg's outcome means for the machine (the
// homing session, the alarm, the datum) is the caller's: see homing.h.

// Why a leg failed. They split by WHERE TO LOOK, which is the only reason a
// failure code exists: a switch or a travel figure, a bus, or a pulser.
#define LEGFAIL_NONE     0
#define LEGFAIL_BUDGET   1   // node stopped itself, switch never asserted --
                             // a real "never reached": bad travel figure,
                             // wrong approach direction, or a dead switch
#define LEGFAIL_POLL     2   // the node stopped ANSWERING mid-leg (4 in a row).
                             // Says nothing about the axis; the bus is the
                             // suspect. The motion may well have been fine.
#define LEGFAIL_DEADLINE 3   // still pulsing past the supervisor's own timeout.
                             // The node's budget should have stopped it first,
                             // so this points at the pulser, not the switch.

// ── rotary only (ROTARY_IDX_* in include/common.h) ──────────────────────────
// A sweep always stops; what varies is what it managed to prove.
#define LEGFAIL_INDEX_ABSENT 4  // the sweep completed and found no usable
                                // feature: crossings 0 (never saw the magnet
                                // at all) or a window that reduced to nothing.
                                // Sensor, magnet, or wiring -- NOT the budget.
#define LEGFAIL_INDEX_SHAPE  5  // a feature was there but did not fit the
                                // capture buffer even at the derived
                                // decimation. The dip's shape changed; the
                                // detector is working and disbelieving it.
#define LEGFAIL_INDEX_SLIP   6  // the index repeated at intervals that disagree
                                // with each other. The measurement is sound and
                                // the MECHANISM is not: slipped belt, stalled
                                // driver, or a feature that is not
                                // once-per-revolution.

#define LEGFAIL_DUMMY        7  // a dummy leg told to fail

// How a leg ended, filled by legPoll and legDrop.
struct LegEnd {
    uint8_t node;
    bool    dummy;     // no node, no motion
    bool    rotary;    // a sweep: no switch, so no latch to record
    bool    retract;   // the node armed a retract (its pin was asserted)
    uint8_t failWhy;   // LEGFAIL_*; LEGFAIL_NONE on success
};

enum LegPoll : uint8_t {
    LEG_RUNNING,
    LEG_DONE,
    LEG_FAILED,
};

// Arm ONE leg on `node` with CMD_HOME_LEG. Returns true once the node is
// pulsing; on any refusal prints the one `err` line and returns false, with
// nothing moved.
//
// NODE-ADDRESSED: every output of a leg is node-framed -- the span, the index
// in the node's own counter, the limit latch (a switch is wired to a NODE,
// position.h). It also means a leg runs during commissioning, before any
// axis_map is committed.
//
// `expectKind` is HOMING_KIND_LIMIT or HOMING_KIND_INDEX -- which VERB the
// operator typed. The node declares its own kind in the status tail, so this
// probes it and refuses a mismatch BEFORE arming: running the wrong leg's
// semantics would drive an axis for a full budget to produce an answer that
// could never exist.
//
// `intendedRetract` is the host's own prediction of what this leg is. The node
// checks it against its own pin read and NAKs on disagreement
// (NAK_INTENT_MISMATCH, include/common.h) rather than silently running the
// wrong leg's semantics under the right leg's budget. Meaningless for
// HOMING_KIND_INDEX -- there is no pin to agree with -- and `rot_leg` passes
// false.
bool legArm(uint8_t node, uint8_t expectKind, uint8_t dir, bool intendedRetract,
            uint16_t startUs, uint16_t floorUs, uint16_t rampSteps, uint32_t maxSteps);

// A bench leg with no node and no motion: after `ms` it ends as a success, or
// as a failure with LEGFAIL_DUMMY.
void legArmDummy(bool succeed, uint32_t ms);

// Poll the running leg. LEG_DONE and LEG_FAILED release it and fill `*end`.
// Call from the Core 0 loop, only while legActive().
LegPoll legPoll(LegEnd* end);

// Release the running leg without a verdict (something else took the machine).
void legDrop(LegEnd* end);

// True while a leg is armed and not yet released.
bool legActive(void);
