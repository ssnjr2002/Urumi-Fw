#include <Arduino.h>
#include "homing.h"
#include "runner.h"
#include "leg_effects.h"
#include "position.h"
#include "../../ipc/shared_state.h"

static uint8_t failWhy = LEGFAIL_NONE;
static uint8_t failNode;

static uint8_t heldCycle;

bool homingActive(void) { return runnerLive(); }

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
static void homingFail(const LegEnd& e) {
    failWhy  = e.failWhy;
    failNode = e.dummy ? 0 : e.node;
    LegEnd ends[LEG_MAX];
    runnerAbortAll(ends);
    alarmReason  = ALARM_HOMING_FAIL;
    machineState = STATE_ALARM;
}

void homingAbandon(uint8_t node, uint8_t why) {
    originInvalidate(node);
    LegEnd e = {};
    e.node    = node;
    e.failWhy = why;
    homingFail(e);
}

// The first leg opens the session. The only alarm a leg is admitted from is
// ALARM_LIMIT_LATCHED (cmdLeg), and the latch mask keeps that fact, so the
// reason is cleared here: anything found in it later is new. The exit
// re-derives it.
static void homingOpen(void) {
    failWhy      = LEGFAIL_NONE;   // this leg has not failed yet
    homingReason = HOMING_LEG;
    alarmReason  = ALARM_NONE;
    __dmb();
    machineState = STATE_HOMING;
}

const char* homingBegin(uint8_t node, uint8_t expectKind, uint8_t dir,
                        bool intendedRetract,
                        uint16_t startUs, uint16_t floorUs,
                        uint16_t rampSteps, uint32_t maxSteps,
                        bool* intentMismatch) {
    const uint8_t intent = expectKind == HOMING_KIND_INDEX ? LEG_SWEEP
                         : intendedRetract                 ? LEG_RETRACT
                                                           : LEG_SEEK;
    const char* why = runnerArm(STATE_HOMING, intent, node, expectKind, dir,
                                intendedRetract, startUs, floorUs, rampSteps,
                                maxSteps, intentMismatch);
    if (!why) homingOpen();
    return why;
}

const char* homingParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                            uint16_t floorUs, uint16_t rampSteps) {
    const char* why = runnerArmPark(STATE_HOMING, node, target, startUs, floorUs, rampSteps);
    if (!why) homingOpen();
    return why;
}

const char* homingAbort(uint8_t node) {
    LegEnd e;
    bool released;
    if (const char* why = runnerAbort(node, &e, &released)) return why;
    if (!released) return nullptr;
    // If something else already owns the machine, the release was all.
    if (machineState == STATE_HOMING) homingFail(e);
    return nullptr;
}

const char* homingDummyBegin(bool succeed, uint32_t ms) {
    const char* why = runnerArmDummy(STATE_HOMING, succeed, ms);
    if (!why) homingOpen();
    return why;
}

// The runner has applied each end's effects (latch, origin); this is the
// session's verdict on them.
void homingTick(void) {
    RunEvent ev;
    while (runnerPop(&ev)) {
        if (ev.kind == RUN_LEG_END) {
            if (ev.end.failWhy != LEGFAIL_NONE) homingFail(ev.end);
            continue;
        }
        // The session stays open for the next leg, `setorigin` or `home_end`;
        // the latch is published at that exit, not here. A step that ended by
        // an abort, or under a fault, leaves the phase to whoever owns the
        // machine.
        if (machineState == STATE_HOMING) homingReason = HOMING_WAIT;
    }
}

bool homingWaiting(void) {
    return machineState == STATE_HOMING && !runnerLive();
}
