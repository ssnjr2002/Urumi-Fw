// machine.cpp — see machine.h.

#include "machine.h"
#include "position.h"          // homingLatched
#include "slot_map.h"
#include "bus.h"
#include "runner.h"
#include "probe.h"
#include "sequence.h"
#include "../planner/queue.h"  // plannerStopJog, plannerQueueLift
#include "../../ipc/shared_state.h"
#include "../config/machine_cfg.h"
#include "hardware/sync.h"

static uint16_t events     = 0;
static bool     latched    = false;
static bool     jobAborted = false;   // a job's abort is waiting on Core 1
static bool     abortLifted = false;  // and its lift has been queued

uint16_t machineFaults(void) {
    uint16_t f = events;
    if (busDegraded())        f |= F_BUS;
    if (machineCfgBlocking()) f |= F_CONFIG;
    if (!slotMapComplete())   f |= F_UNMAPPED;
    if (homingLatched)        f |= F_LATCHED;
    if (latched)              f |= F_ESTOP;
    return f;
}

void faultSet(uint16_t f) {
    f &= F_EVENTS;
    if (!f) return;
    events |= f;
    dutyEpoch = dutyEpoch + 1;   // an alarm resets the tool's duty
    machineTeardown(false);
}

void faultsClearEvents(void) { events = 0; }

void estopLatch(void) {
    latched        = true;
    estopSwept     = false;
    __dmb();
    estopRequested = true;
    machineTeardown(true);
}

bool estopLatched(void) { return latched; }

void estopClear(void) {
    latched        = false;
    estopRequested = false;
    estopSwept     = false;
    machinePublish();
}

void contextOpen(uint8_t ctx) {
    __dmb();
    machineContext = ctx;
    machinePublish();
}

void contextClose(uint8_t ctx) {
    if (machineContext != ctx) return;
    // Closing a level tears down the levels below it.
    sequenceAbort();
    if (runnerLive()) {
        LegEnd ends[LEG_MAX];
        runnerAbortAll(ends);
    }
    __dmb();
    machineContext = CTX_IDLE;
    machinePublish();
}

uint8_t machineStep(void) {
    if (runnerLive() || probeLegInFlight()) return STEP_LEGS;
    if (plannerActive || plannerRing.count() != 0) return STEP_RING;
    if (sequenceWaiting()) return STEP_WAIT;
    return STEP_NONE;
}

void jobAbort(void) {
    if (machineContext == CTX_JOB) jobAborted = true;
    __dmb();
    abortRequested = true;
}

void machineTeardown(bool estop) {
    LegEnd ends[LEG_MAX];
    if (runnerLive()) {
        if (estop) runnerDropAll(ends);
        else       runnerAbortAll(ends);
    }
    sequenceAbort();
    switch (machineContext) {
        case CTX_PROBE:
            probeTeardown(!estop);          // closes the context
            break;
        case CTX_HOMING:
            contextClose(CTX_HOMING);
            break;
        case CTX_JOB:
            if (estop) {
                // Core 1's sweep drops the ring.
                jobAborted = abortLifted = false;
                contextClose(CTX_JOB);
            } else {
                jobAbort();
            }
            break;
        default:
            if (!estop && machineStep() == STEP_RING) plannerStopJog();
            break;
    }
    machinePublish();
}

void machineReset(void) {
    events         = 0;
    latched        = false;
    jobAborted     = false;
    abortLifted    = false;
    estopRequested = false;
    estopSwept     = false;
    machineContext = CTX_IDLE;
    ringOwner      = RING_JOB;
    machineState   = STATE_ALARM;
}

static uint8_t stateOf(uint16_t faults) {
    if (latched) return estopSwept ? STATE_ALARM : STATE_ESTOP;
    // A sequence by name, whatever the context.
    const uint8_t seq = sequenceId();
    if (seq == SEQ_HOME || seq == SEQ_RECOVERY) return STATE_HOMING;
    if (seq == SEQ_GOTO) return STATE_JOGGING;
    if (seq == SEQ_TOOL_CHANGE) return STATE_JOB;
    // A manual context keeps its name over its raw legs.
    if (machineContext == CTX_PROBE)  return STATE_PROBING;
    if (machineContext == CTX_HOMING) return STATE_HOMING;
    // A step outranks the job context.
    const uint8_t step = machineStep();
    if (step == STEP_LEGS) return STATE_TRAVERSE;
    if (step == STEP_RING && ringOwner != RING_JOB) return STATE_JOGGING;
    if (machineContext == CTX_JOB) return STATE_JOB;
    return faults ? STATE_ALARM : STATE_IDLE;
}

uint8_t machineView(void) { return stateOf(machineFaults()); }

static bool jobPaused(void) { return machineContext == CTX_JOB && jobReason == JOB_PAUSED; }

static bool estopUnswept(void) { return latched && !estopSwept; }

// A paused job's ring is parked, not moving.
static bool stepLive(void) {
    const uint8_t step = machineStep();
    return step == STEP_LEGS || (step == STEP_RING && !jobPaused());
}

bool machineAtRest(void) {
    return !estopUnswept() && !stepLive() && !sequenceLive() &&
           (machineContext == CTX_IDLE || jobPaused());
}

bool machineBusFree(void) {
    return machineAtRest() ||
           (machineContext == CTX_HOMING && machineStep() == STEP_NONE && !sequenceLive());
}

const char* machineAdmit(uint8_t kind) {
    const uint8_t k   = K_KIND(kind);
    const uint8_t ctx = machineContext;
    if (k == K_ANY || k == K_READ) return nullptr;
    const bool busy = stepLive() || sequenceLive();
    if (ctx == CTX_JOB && jobReason == JOB_TOOL_WAIT && (kind & K_IN_TOOL)) {
        if (busy) return "busy";
        return latched ? "bad_state" : nullptr;
    }
    if ((ctx == CTX_PROBE && !(kind & K_IN_PROBE)) ||
        (ctx == CTX_HOMING && !(kind & K_IN_HOMING)))
        return busy ? "busy" : "bad_state";
    bool ok = false;
    switch (k) {
        case K_WRITE:
            ok = !estopUnswept() && !busy &&
                 (ctx == CTX_IDLE || ctx == CTX_PROBE || ctx == CTX_HOMING || jobPaused());
            break;
        case K_STEP:
            ok = !latched && !busy && ctx != CTX_JOB;
            break;
        case K_SEQUENCE:
            ok = !latched && !busy && ctx == CTX_IDLE && (machineFaults() & ~F_LATCHED) == 0;
            break;
        case K_CONTEXT:
            ok = !latched;
            break;
        case K_CLEAR:
            ok = !busy && ctx == CTX_IDLE;
            break;
    }
    if (ok) return nullptr;
    return busy ? "busy" : "bad_state";
}

// An aborted job at rest lifts once, then closes when the lift has drained.
static void abortSettle(void) {
    uint32_t s = spin_lock_blocking(plannerLock);
    const bool rest = jobAborted && !abortRequested && !plannerActive && plannerRing.count() == 0;
    spin_unlock(plannerLock, s);
    if (!rest) return;
    if (!abortLifted && plannerQueueLift(JOB_ABORTING)) { abortLifted = true; return; }
    s = spin_lock_blocking(plannerLock);
    jobAborted = abortLifted = false;
    if (machineContext == CTX_JOB) machineContext = CTX_IDLE;
    spin_unlock(plannerLock, s);
}

void machinePublish(void) {
    abortSettle();
    const uint16_t f = machineFaults();
    const uint32_t s = spin_lock_blocking(plannerLock);
    machineState = stateOf(f);
    spin_unlock(plannerLock, s);
}
