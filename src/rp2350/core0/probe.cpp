// probe.cpp — the tool-height probe session. See probe.h and docs/tool_probe.md.
#include <Arduino.h>
#include <string.h>
#include "probe.h"
#include "position.h"
#include "cmd/axis_map.h"
#include "../ipc/shared_state.h"
#include "../ipc/core1_rpc.h"
#include "hardware/sync.h"     // __dmb

// Z takes slot 2 (SLOT_Z); the vacuum takes slot 3.
//
// Weak preference, since the verified teardown in probeBegin removes the hazard
// that would make it matter: on builds with no fourth axis, slot 3 is the one
// most likely genuinely unoccupied, and if that verification ever regressed,
// phantom steps on a rotary/aux axis are less destructive than on the gantry.
// Slot 1 would be fine.
#define PROBE_Z_SLOT    2
#define PROBE_VAC_SLOT  3

// Confirm polls and noise retries. Both are cheap, and both are only spent after
// Z has already stopped (§5.9) — which is why real statistical rejection is
// affordable here and unaffordable during the descent.
#define PROBE_CONFIRM_POLLS  8
#define PROBE_RETRY_LIMIT    3

// Ceiling on how long Core 0 waits for a posted leg before declaring the
// EMITTER, not the switch, at fault. Derived from the leg's own budget the way
// homingTimeoutMs is, plus a margin: a leg that has not come back long after its
// own worst case could not have finished, and DEADLINE says to go and look at
// Core 1 rather than at the bed.
#define PROBE_DEADLINE_CAP_MS  600000UL

static bool     claimed     = false;
static bool     legInFlight = false;
static uint8_t  zNodeId     = 0;
static uint8_t  vacNodeId   = 0;
static uint8_t  savedMap[MOTION_SLOTS];
static uint8_t  returnState = STATE_IDLE;
static uint16_t legId       = 0;
static uint32_t legDeadline = 0;

// The Z node's own counter at the last boundary. §5.8's cross-check compares
// DELTAS against it rather than absolute positions, which keeps this out of the
// origin bookkeeping entirely: what it asks is "did the node take the steps we
// emitted", and that question is answered by a difference.
static int32_t  lastNodePos = 0;
static bool     haveNodePos = false;

static uint8_t  lastCause   = PROBE_OK;
static uint8_t  lastRetries = 0;
static int32_t  lastSteps   = 0;

bool    probeActive(void)       { return claimed; }
bool    probeLegInFlight(void)  { return legInFlight; }
uint8_t probeLastCause(void)    { return lastCause; }
uint8_t probeLastRetries(void)  { return lastRetries; }
int32_t probeLastSteps(void)    { return lastSteps; }

// ── The switch, read over the bus ────────────────────────────────────────────
// Refreshes probingReason from a real read. Only ever called between legs, which
// is the only time the bus is free AND the only time the answer can change —
// nothing moves between legs. `*openOut` is left untouched on a failed read, so
// a bus fault cannot fabricate a "clear" that lets an exit through.
static bool readSwitch(bool* openOut) {
    uint8_t level = 0;
    if (rpcSwitchGet(vacNodeId, &level) != RPC_OK) return false;
    *openOut = (level != 0);
    probingReason = level ? PROBING_CONTACT : PROBING_CLEAR;
    return true;
}

// ── Teardown ─────────────────────────────────────────────────────────────────
// Disengage everything, then replay a map through the EXISTING axis_map path.
//
// Not a restore routine, deliberately. axisMapApply is "deliberately dumb, not a
// diff": it rebuilds machinePos, axes_homed and homingLatched from ENGAGE acks
// rather than from anything remembered, so it is correct even if a node reset
// mid-probe. A second binder restoring from saved state is precisely where this
// would go wrong.
//
// BEST-EFFORT. If the bus is what failed, some engages time out — but that path
// already handles it (parkForget with no answer, slotBind from acks), so a
// partial restore is the same defined degradation any axis_map produces on a
// flaky bus, not garbage.
static void probeRestore(const uint8_t* map) {
    axisMapApply(map, /*quiet=*/true);
    claimed     = false;
    legInFlight = false;
    haveNodePos = false;
}

// A failed leg tears the session down ITSELF, then alarms.
//
// The reason is not the datum — that usually survives (see
// probeCauseVoidsDatum). It is `unalarm`: without a restore, clearing the alarm
// would return the machine to IDLE with a probe binding live, three axes holding
// no slots and a vacuum in slot 3. That is the silent-axis-drop condition. The
// invariant is that ALARM is never entered with a probe binding live, the same
// shape as the estop sweep's guarantee that once you observe ALARM, everything
// on the bus is already parked.
//
// This does its blocking bus work inside the supervisor tick rather than a
// command handler, which homing.h otherwise warns against. Deliberate asymmetry:
// a failed probe is already a stop-everything event with nothing streaming, so
// an unresponsive control plane during a fault is tolerable in a way it would
// not be on the happy path — which is exactly why the happy-path exit is a
// command.
// An estop arrives here as a leg result, but it is not this module's verdict to
// publish. Core 1's estop path owns the transition -- it flushes, sweeps the bus
// and sets ALARM_ESTOP -- and that outranks a probe-fail reason, because the
// recovery ladders genuinely differ: a probe failure de-energises nothing and is
// cleared by `unalarm`, while an estop de-energises the whole bus, makes every
// axis back-drivable and needs a re-home. So the session is still torn down (the
// invariant is that ALARM is never entered with a probe binding live) but the
// reason and the state are left alone.
static void probeFail(uint8_t cause) {
    // Capture the reason BEFORE restoring, and do not let the restore overwrite
    // it. Otherwise a POLL failure whose restore also fails reports as a config
    // problem, and the diagnostically useful fact — the vacuum stopped answering
    // — is gone.
    lastCause = cause;

    // Only the causes that lost track of steps void the datum. Most probe
    // failures do not, and position.cpp already draws that line correctly by
    // keying origin invalidation on ESTOP / SOFT_LIMIT rather than on ALARM
    // generally — so a probe-fail alarm must not use either of those reasons.
    if (probeCauseVoidsDatum(cause)) originInvalidate(zNodeId);

    probeRestore(savedMap);

    if (cause == PROBE_ESTOP) return;   // Core 1 owns this transition

    alarmReason = ALARM_PROBE_FAIL;
    __dmb();
    machineState = STATE_ALARM;
}

// ── probe_map ────────────────────────────────────────────────────────────────
bool probeBegin(uint8_t zNode, uint8_t vacNode) {
    if (claimed) { Serial.println("err busy"); return true; }
    if (zNode == vacNode) { Serial.println("err dup"); return true; }

    // A de-energised Z accepts every leg and cannot turn, so the terminator is
    // never reached, the budget burns out, and the result reads as a broken
    // switch rather than a motor nobody turned on. legCommon refuses a home for
    // this reason; the same reason applies here and the same mask answers it.
    if (!(nodeEnabled & (1u << zNode))) { Serial.println("err not_enabled"); return true; }

    for (uint8_t i = 0; i < MOTION_SLOTS; i++) savedMap[i] = slotNodeAt(i);
    returnState = machineState;        // IDLE or PAUSED — a mid-job tool swap
                                       // must land back in PAUSED, and neither
                                       // exit route can infer where it started

    // Disengage every bound node, and verify types out of the acks. Each ENGAGE
    // ack's first status byte is node_type(), so the pass that has to happen
    // anyway proves the id called a stepper is a stepper and the id called a
    // vacuum is a vacuum — free.
    bool zSeen = false, vacSeen = false;
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        const uint8_t n = savedMap[i];
        if (n == SLOT_NONE) continue;
        NodeStatus st;
        if (rpcNodeStatus(CMD_ENGAGE, n, SLOT_NONE, &st) != RPC_OK) {
            Serial.printf("err node %d no_ack\n", n);
            return true;               // map untouched: nothing has been rebound
        }
        if (st.hasStepperTail) parkRecord(n, st.pos); else parkForget(n);
        if (n == zNode)   { zSeen   = true; if (st.type != NODE_TYPE_STEPPER) {
            Serial.printf("err node %d not_stepper\n", n); return true; } }
        if (n == vacNode) { vacSeen = true; if (st.type != NODE_TYPE_VACUUM) {
            Serial.printf("err node %d not_vacuum\n", n); return true; } }
    }

    // The vacuum is normally NOT in the committed map — it holds no motion slot
    // — so its type is verified by its own engage below instead.
    (void)zSeen;

    NodeStatus zst, vst;
    if (rpcNodeStatus(CMD_ENGAGE, zNode, PROBE_Z_SLOT, &zst) != RPC_OK ||
        zst.type != NODE_TYPE_STEPPER) {
        Serial.printf("err node %d engage\n", zNode);
        axisMapApply(savedMap, /*quiet=*/true);
        return true;
    }
    if (rpcNodeStatus(CMD_ENGAGE, vacNode, PROBE_VAC_SLOT, &vst) != RPC_OK ||
        vst.type != NODE_TYPE_VACUUM) {
        Serial.printf("err node %d engage\n", vacNode);
        axisMapApply(savedMap, /*quiet=*/true);
        return true;
    }
    (void)vacSeen;

    slotBind(PROBE_Z_SLOT, zNode, &zst);
    // The vacuum is deliberately NOT slotBind()'d. slotBind writes machinePos,
    // axes_homed and homingLatched for the slot, and the vacuum has no stepper
    // tail — the slot would land at position 0 with the datum cleared while
    // reconcileValidity still projected nodeEnabled through the map, so the host
    // would see slot 3 as an enabled, unhomed axis at zero. The probe emitter
    // takes the vacuum's slot as a number, not from the map.
    for (uint8_t i = 0; i < MOTION_SLOTS; i++)
        if (i != PROBE_Z_SLOT) slotUnbind(i);

    zNodeId     = zNode;
    vacNodeId   = vacNode;
    claimed     = true;
    legInFlight = false;
    lastCause   = PROBE_OK;
    lastRetries = 0;
    lastSteps   = 0;
    haveNodePos = zst.hasStepperTail;
    lastNodePos = zst.pos;

    bool open = false;
    if (!readSwitch(&open)) {
        Serial.printf("err node %d no_switch\n", vacNode);
        probeRestore(savedMap);
        return true;
    }

    __dmb();
    machineState = STATE_PROBING;
    Serial.printf("ok probing z=%d vac=%d switch=%d\n", zNode, vacNode, open ? 1 : 0);
    return true;
}

// ── probe_leg ────────────────────────────────────────────────────────────────
bool probeArmLeg(uint8_t dir, uint16_t startUs, uint16_t ceilUs,
                 uint16_t rampSteps, uint8_t pollDiv, uint32_t maxSteps,
                 uint16_t deadlineUs, uint8_t intent) {
    if (!claimed)     { Serial.println("err not_probing"); return true; }
    if (legInFlight)  { Serial.println("err busy");        return true; }
    if (!(nodeEnabled & (1u << zNodeId))) { Serial.println("err not_enabled"); return true; }

    // §5.8's cross-check, at the leg boundary — one of the only two windows
    // where the bus is free. A mismatch means steps were refused, and the limit
    // gate refuses SILENTLY, so everything downstream would be measuring a
    // fiction. It is checked here rather than after the fact because the cheapest
    // moment to stop is before the next leg drives the axis.
    NodeStatus st;
    if (rpcNodeStatus(CMD_NODE_STATUS, zNodeId, 0, &st) != RPC_OK) {
        Serial.println("err node no_ack"); return true;
    }
    if (haveNodePos && st.hasStepperTail && st.pos != lastNodePos) {
        Serial.println("err pos_mismatch");
        probeFail(PROBE_POS_MISMATCH);
        return true;
    }

    // The intent check runs HERE, not on the node. CMD_HOME_LEG pushes it to the
    // node because only the node can see its own limit pin; the Pico can read
    // this switch itself between legs, so it checks before arming. Same purpose
    // — catch a sequencing error before it drives an axis for a full budget —
    // implemented on the side that can see.
    //
    // Not made redundant by probingReason, even though the two read the same
    // pin: the reason reports what the machine HAS, `intent` is what the host
    // BELIEVES, and catching a host that disagrees with the machine is the whole
    // value of the check. A check derived from the machine's own reading could
    // never disagree with it.
    bool open = false;
    if (!readSwitch(&open)) { Serial.println("err node no_switch"); return true; }
    if (open != (intent != 0)) {
        // Which way is DOWN is not knowable here -- `dir` is a wire bit and the
        // host owns the geometry -- so ALREADY_OPEN is derived from the intent
        // rather than from the direction. A leg that declared "I expect the
        // switch closed" and found it open IS §5.10's already-open case: the
        // switch has failed, or Z is parked on the bed, and either way running
        // the leg would drive the tool further into it.
        //
        // The mirror case (declared open, found closed) is an ordinary
        // sequencing error on the host's side: nothing is pressed into anything,
        // so it is refused without tearing the session down.
        if (intent == 0) {
            probeFail(PROBE_ALREADY_OPEN);
            Serial.println("err already_open");
        } else {
            Serial.printf("err intent switch=%d\n", open ? 1 : 0);
        }
        return true;
    }

    ProbeLegReq rq;
    rq.zSlot        = PROBE_Z_SLOT;
    rq.vacSlot      = PROBE_VAC_SLOT;
    rq.vacNode      = vacNodeId;
    rq.dir          = dir;
    rq.startUs      = startUs;
    rq.ceilUs       = ceilUs;
    rq.rampSteps    = rampSteps;
    rq.pollDiv      = pollDiv;
    rq.maxSteps     = maxSteps;
    rq.deadlineUs   = deadlineUs;
    rq.confirmPolls = PROBE_CONFIRM_POLLS;
    rq.retryLimit   = PROBE_RETRY_LIMIT;

    if (!rpcProbeLegPost(&rq, &legId)) { Serial.println("err busy"); return true; }

    // Worst case: every step at its slowest interval, plus a full poll deadline
    // for each poll, plus a margin. Generous on purpose — this bound exists to
    // catch a wedged emitter, not to police a slow bus, and a leg that merely
    // ran slowly must not be killed by it.
    const uint64_t stepUs = (uint64_t)maxSteps * (startUs > ceilUs ? startUs : ceilUs);
    const uint64_t pollUs = (uint64_t)(maxSteps / pollDiv) * deadlineUs;
    uint64_t ms = ((stepUs + pollUs) * 12 / 10) / 1000 + 1000;
    if (ms > PROBE_DEADLINE_CAP_MS) ms = PROBE_DEADLINE_CAP_MS;
    legDeadline = millis() + (uint32_t)ms;

    legInFlight   = true;
    probingReason = PROBING_LEG;
    Serial.println("ok");
    return true;
}

// ── probe_end / axis_map-as-exit ─────────────────────────────────────────────
bool probeExit(const uint8_t* newMap) {
    if (!claimed)    { Serial.println("err not_probing"); return true; }
    if (legInFlight) { Serial.println("err busy");        return true; }

    // Re-read rather than trusting probingReason. The reason is refreshed at
    // every boundary and nothing moves between legs, so the two agree — but this
    // is the gate that keeps a tool off the bed, and a gate should read the pin
    // it is protecting.
    bool open = false;
    if (!readSwitch(&open)) { Serial.println("err node no_switch"); return true; }
    if (open) {
        Serial.println("err not_cleared");
        lastCause = PROBE_NOT_CLEARED;
        return true;                    // session stays open: a retract can fix it
    }

    probeRestore(newMap ? newMap : savedMap);

    // Back to wherever the session started. A jog during pause returns to PAUSED
    // because the job is still suspended, and a mid-job tool swap is that case
    // exactly — so the session cannot simply land in IDLE.
    __dmb();
    machineState = (returnState == STATE_PAUSED) ? STATE_PAUSED : STATE_IDLE;
    Serial.println("ok");
    return true;
}

// ── supervisor ───────────────────────────────────────────────────────────────
void probeTick(void) {
    if (!legInFlight) return;

    RpcReply rep;
    if (rpcPoll(&rep)) {
        legInFlight = false;
        ProbeLegOut res;
        if (rep.id != legId || rep.result != RPC_OK || !rpcProbeLegDecode(&rep, &res)) {
            probeFail(PROBE_DEADLINE);   // Core 1 answered something we cannot
            return;                      // read: the emitter, not the switch
        }
        lastCause   = res.cause;
        lastRetries = res.retries;
        lastSteps   = res.steps;

        // Refresh the node counter baseline from what the leg actually emitted,
        // so the NEXT boundary's cross-check asks about the next leg only.
        lastNodePos += res.steps;

        if (res.cause != PROBE_OK) { probeFail(res.cause); return; }

        // Success is not a terminal state. A leg boundary leaves the machine in
        // STATE_PROBING with the reason back to CLEAR or CONTACT — that
        // transition IS the leg-done signal the session state took away.
        bool open = false;
        if (!readSwitch(&open)) { probeFail(PROBE_POLL); return; }
        return;
    }

    if ((int32_t)(millis() - legDeadline) >= 0) {
        legInFlight = false;
        probeFail(PROBE_DEADLINE);
    }
}
