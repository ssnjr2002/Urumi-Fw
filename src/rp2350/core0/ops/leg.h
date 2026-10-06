#pragma once
#include <stdint.h>

// leg.h — Core 0's supervisor for node-run legs (docs/homing.md §2.3), at
// most one per node, run in parallel.
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

#define LEGFAIL_PARK         8  // a park stopped off its target: on a switch,
                                // short of it, or with the node's datum gone
#define LEGFAIL_ABORTED      9  // the operator aborted the leg (leg_abort)

// How a leg ended, filled by legPoll and legDrop.
struct LegEnd {
    uint8_t node;
    bool    dummy;     // no node, no motion
    bool    rotary;    // a sweep: no switch, so no latch to record
    bool    retract;   // the node armed a retract (its pin was asserted)
    bool    park;      // a park leg
    int32_t pos;       // a finished park: the node's counter at the final poll
    uint8_t failWhy;   // LEGFAIL_*; LEGFAIL_NONE on success
};

// Legs supervised at once: one per stepper node (six on a dual-head machine).
#define LEG_MAX 8

enum LegPoll : uint8_t {
    LEG_RUNNING,
    LEG_DONE,
    LEG_FAILED,
};

// Arm ONE seek, retract or sweep on `node` with CMD_HOME_LEG. Returns nullptr
// once the node is pulsing; on a refusal, with nothing moved, the reply text
// after `err ` (valid until the next refusal). A node already running a leg is
// `node <id> busy`; a full table is `busy`. Prints nothing.
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
// HOMING_KIND_INDEX -- there is no pin to agree with -- and `leg <n> sweep`
// passes false.
const char* legArm(uint8_t node, uint8_t expectKind, uint8_t dir, bool intendedRetract,
                   uint16_t startUs, uint16_t floorUs, uint16_t rampSteps,
                   uint32_t maxSteps);

// Arm a park leg on `node` with CMD_PARK_LEG: run to the absolute node counter
// `target`. Any node with a terminator; the caller checks the origin. Returns
// as legArm. A node already at `target` arms nothing and the first poll
// finishes the leg.
const char* legArmPark(uint8_t node, int32_t target, uint16_t startUs,
                       uint16_t floorUs, uint16_t rampSteps);

// Send CMD_LEG_ABORT to `node`. The refusal text, as legArm, if the node did
// not ack; the leg (if any) then runs on, still supervised. On an ack to a node
// running a leg, that leg is released as LEGFAIL_ABORTED into `*end` and
// `*released` is set.
const char* legAbort(uint8_t node, LegEnd* end, bool* released);

// Abort every running leg (CMD_LEG_ABORT to each node) and release it into
// `ends` (LEG_MAX entries); returns how many. An acked park ends with
// LEGFAIL_NONE and the ack's counter in `pos`: the node stays energised, so
// its datum stands. Every other end is LEGFAIL_ABORTED.
uint8_t legAbortAll(LegEnd* ends);

// A bench leg with no node and no motion: after `ms` it ends as a success, or
// as a failure with LEGFAIL_DUMMY. `busy` on a full table.
const char* legArmDummy(bool succeed, uint32_t ms);

// Poll the next due leg, in turn. LEG_DONE and LEG_FAILED release it and fill
// `*end`; LEG_RUNNING means no leg ended this call. Call from the Core 0 loop.
LegPoll legPoll(LegEnd* end);

// Release every leg without a verdict into `ends` (LEG_MAX entries); returns
// how many (something else took the machine).
uint8_t legDropAll(LegEnd* ends);

// True while `node` runs a leg.
bool legActive(uint8_t node);

// True while any leg runs.
bool legAny(void);
