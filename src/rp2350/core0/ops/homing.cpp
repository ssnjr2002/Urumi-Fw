#include <Arduino.h>
#include "homing.h"
#include "machine.h"
#include "runner.h"
#include "sequence.h"
#include "leg_effects.h"
#include "position.h"
#include "../../ipc/shared_state.h"

static uint8_t failWhy = LEGFAIL_NONE;
static uint8_t failNode;

static uint8_t heldCycle;

void homingHold(uint8_t cycle) { heldCycle = cycle; }

uint8_t homingHeld(void) { return heldCycle; }

uint8_t homingFailWhy(void) { return failWhy; }

uint8_t homingFailNode(void) { return failNode; }

// The failed leg was already released by the runner (leg_effects.h).
//
// The node's own span survives this untouched: how far the leg got before it
// stopped is the whole diagnostic, and `nodestat` still reports it alongside
// homefail= to tell a budget that genuinely ran out from one that stopped
// nowhere near its limit.
//
// A failure fails its cycle: every other running leg is aborted, node by node,
// and released without a verdict.
void homingFail(const LegEnd& e) {
    failWhy  = e.failWhy;
    failNode = e.dummy ? 0 : e.node;
    LegEnd ends[LEG_MAX];
    runnerAbortAll(ends);
    faultSet(F_HOMING);   // ends the sequence and closes the context
}

void homingAbandon(uint8_t node, uint8_t why) {
    originInvalidate(node);
    LegEnd e = {};
    e.node    = node;
    e.failWhy = why;
    homingFail(e);
}

// The armed leg has not failed yet.
static const char* armed(const char* why) {
    if (!why) failWhy = LEGFAIL_NONE;
    return why;
}

const char* homingArm(uint8_t node, uint8_t expectKind, uint8_t dir,
                      bool intendedRetract,
                      uint16_t startUs, uint16_t floorUs,
                      uint16_t rampSteps, uint32_t maxSteps,
                      bool* intentMismatch) {
    const uint8_t intent = expectKind == HOMING_KIND_INDEX ? LEG_SWEEP
                         : intendedRetract                 ? LEG_RETRACT
                                                           : LEG_SEEK;
    return armed(runnerArm(intent, node, expectKind, dir, intendedRetract,
                           startUs, floorUs, rampSteps, maxSteps, intentMismatch));
}

const char* homingArmPark(uint8_t node, int32_t target, uint16_t startUs,
                          uint16_t floorUs, uint16_t rampSteps) {
    return armed(runnerArmPark(node, target, startUs, floorUs, rampSteps));
}

const char* homingArmUnlatch(uint8_t node, uint8_t dir, uint16_t startUs,
                             uint16_t floorUs, uint16_t rampSteps, uint32_t maxSteps) {
    return armed(runnerArm(LEG_UNLATCH, node, HOMING_KIND_LIMIT, dir, true,
                           startUs, floorUs, rampSteps, maxSteps));
}

// ── the raw leg: a one-step sequence in the manual homing context ───────────

// The context stays open for the next leg, `setorigin` or `home_end`; the
// latch is published at that exit, not here.
static bool legNext(uint8_t) {
    homingReason = HOMING_WAIT;
    return false;
}

static const Sequence legSequence = { SEQ_LEG, legNext, nullptr };

// The first leg opens the context, from IDLE or with only F_LATCHED set
// (cmdLeg).
static const char* legStarted(const char* why) {
    if (why) return why;
    homingReason = HOMING_LEG;
    if (machineContext != CTX_HOMING) contextOpen(CTX_HOMING);
    sequenceStart(&legSequence);
    return nullptr;
}

const char* homingBegin(uint8_t node, uint8_t expectKind, uint8_t dir,
                        bool intendedRetract,
                        uint16_t startUs, uint16_t floorUs,
                        uint16_t rampSteps, uint32_t maxSteps,
                        bool* intentMismatch) {
    return legStarted(homingArm(node, expectKind, dir, intendedRetract, startUs,
                                floorUs, rampSteps, maxSteps, intentMismatch));
}

const char* homingParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                            uint16_t floorUs, uint16_t rampSteps) {
    return legStarted(homingArmPark(node, target, startUs, floorUs, rampSteps));
}

const char* homingDummyBegin(bool succeed, uint32_t ms) {
    return legStarted(armed(runnerArmDummy(succeed, ms)));
}

const char* homingAbort(uint8_t node) {
    LegEnd e;
    bool released;
    if (const char* why = runnerAbort(node, &e, &released)) return why;
    if (!released) return nullptr;
    // A homing leg fails its sequence; a traverse leg's step just ends.
    const uint8_t seq = sequenceId();
    if (seq != SEQ_NONE && seq != SEQ_TRAVERSE) homingFail(e);
    return nullptr;
}

bool homingWaiting(void) {
    return machineContext == CTX_HOMING && !sequenceLive();
}
