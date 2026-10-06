#include <Arduino.h>
#include "homing.h"
#include "position.h"
#include "../../ipc/shared_state.h"

static uint8_t failWhy = LEGFAIL_NONE;
static uint8_t failNode;

bool homingActive(void) { return legAny(); }

uint8_t homingFailWhy(void) { return failWhy; }

uint8_t homingFailNode(void) { return failNode; }

// The leg is over, one way or the other. Both exits invalidate the origin, and
// that is not conservatism -- it is required. A leg moves the axis with the
// NODE's own pulser, so the node counts those steps and Core 1 does not; the
// machinePos it maintains is stale the moment the pulser runs. (`step` differs:
// it goes through the stream, so Core 1 adds the same steps and both frames stay
// consistent -- see cmdStep.) Dropping the datum makes that staleness visible as
// an un-homed axis instead of a plausible wrong number, and §3.4's closing
// `setorigin` is what re-derives it from the node's counter.
//
// The one exception is a park that finished: it moved within the datum, with
// the witness intact and the counter on target, so the origin stands and only
// machinePos is re-derived.
static void homingRelease(const LegEnd& e) {
    if (e.dummy) return;
    if (e.park && e.failWhy == LEGFAIL_NONE) originAdopt(e.node, e.pos);
    else                                      originInvalidate(e.node);
}

// homingLatched is deliberately untouched here, and it is already right in both
// failure modes: a seek that failed never reached the switch, so its bit should
// stay clear, and a retract that failed never escaped one, so its bit should
// stay set. Both are what the last successful leg left behind.
//
// The node's own span survives this untouched: how far the leg got before it
// stopped is the whole diagnostic, and `nodestat` still reports it alongside
// homefail= to tell a budget that genuinely ran out from one that stopped
// nowhere near its limit.
//
// A failure fails its cycle: every other running leg is aborted, node by node,
// and released without a verdict. legAbortAll keeps an aborted park's origin.
static void homingFail(const LegEnd& e) {
    failWhy  = e.failWhy;
    failNode = e.dummy ? 0 : e.node;
    homingRelease(e);
    LegEnd ends[LEG_MAX];
    const uint8_t n = legAbortAll(ends);
    for (uint8_t i = 0; i < n; i++) homingRelease(ends[i]);
    alarmReason  = ALARM_HOMING_FAIL;
    machineState = STATE_ALARM;
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

bool homingBegin(uint8_t node, uint8_t expectKind, uint8_t dir,
                 bool intendedRetract,
                 uint16_t startUs, uint16_t floorUs,
                 uint16_t rampSteps, uint32_t maxSteps) {
    if (!legArm(node, expectKind, dir, intendedRetract,
                startUs, floorUs, rampSteps, maxSteps)) return true;
    homingOpen();
    Serial.println("ok");
    return true;
}

bool homingParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                     uint16_t floorUs, uint16_t rampSteps) {
    if (!legArmPark(node, target, startUs, floorUs, rampSteps)) return true;
    homingOpen();
    Serial.println("ok");
    return true;
}

bool homingAbort(uint8_t node) {
    LegEnd e;
    bool released;
    if (!legAbort(node, &e, &released)) return true;
    // Answer first: the alarm is the leg's outcome, not the command's.
    Serial.println("ok");
    if (!released) return true;
    // As homingTick: if something else already owns the machine, drop quietly.
    if (machineState != STATE_HOMING) homingRelease(e);
    else                              homingFail(e);
    return true;
}

void homingDummyBegin(bool succeed, uint32_t ms) {
    if (!legArmDummy(succeed, ms)) return;
    homingOpen();
    Serial.println("ok");
}

void homingTick(void) {
    if (!legAny()) return;

    LegEnd e;
    // Something else has taken the machine -- `stop` sets ESTOP, Core 1 folds it
    // to ALARM. Drop the legs rather than fight for them: leaving them armed
    // would fire timeouts later, at a moment with nothing to do with homing.
    if (machineState != STATE_HOMING) {
        LegEnd ends[LEG_MAX];
        const uint8_t n = legDropAll(ends);
        for (uint8_t i = 0; i < n; i++) homingRelease(ends[i]);
        return;
    }

    switch (legPoll(&e)) {
        case LEG_RUNNING: return;
        case LEG_FAILED:  homingFail(e); return;
        case LEG_DONE:    break;
    }

    // The leg succeeded, so its outcome is known without re-reading the pin: a
    // seek ended ON the switch and a retract ended OFF it. Recorded against the
    // NODE, which is what the switch is wired to; position.cpp keeps the
    // slot-framed homingLatched in step and re-derives it across a rebind.
    //
    // Never for a sweep: the latch means "this axis is standing on its limit
    // switch", and a rotary axis has no switch to stand on. Setting it would put
    // the machine in ALARM/LIMIT_LATCHED after a home that succeeded.
    // Nor for a park: it finished off its switch, and the latch is unchanged.
    if (!e.dummy && !e.rotary && !e.park) nodeLatchSet(e.node, !e.retract);

    homingRelease(e);

    // The session stays open for the next leg, `setorigin` or `home_end`; the
    // latch is published at that exit, not here. If `stop` landed during the
    // poll above, the fault owns the machine and the phase is not ours to set.
    if (machineState != STATE_HOMING || legAny()) return;
    homingReason = HOMING_WAIT;
}

bool homingWaiting(void) {
    return machineState == STATE_HOMING && !legAny();
}
