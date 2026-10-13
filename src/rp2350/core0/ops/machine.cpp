// machine.cpp — see machine.h.

#include "machine.h"
#include "position.h"          // homingLatched
#include "slot_map.h"
#include "bus.h"
#include "runner.h"
#include "probe.h"
#include "../planner/queue.h"  // plannerStopJog
#include "../../ipc/shared_state.h"
#include "../config/machine_cfg.h"
#include "hardware/sync.h"

static uint16_t events     = 0;
static bool     latched    = false;
static bool     jobAborted = false;   // a job's abort is waiting on Core 1

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
    __dmb();
    machineContext = CTX_IDLE;
    machinePublish();
}

uint8_t machineStep(void) {
    if (runnerLive() || probeLegInFlight()) return STEP_LEGS;
    if (plannerActive || plannerRing.count() != 0) return STEP_RING;
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
                jobAborted = false;
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
    estopRequested = false;
    estopSwept     = false;
    machineContext = CTX_IDLE;
    ringOwner      = RING_JOB;
    machineState   = STATE_ALARM;
}

static uint8_t stateOf(uint16_t faults) {
    if (latched) return estopSwept ? STATE_ALARM : STATE_ESTOP;
    switch (machineContext) {
        case CTX_JOB:    return STATE_JOB;
        case CTX_PROBE:  return STATE_PROBING;
        case CTX_HOMING: return STATE_HOMING;
        default:         break;
    }
    const uint8_t step = machineStep();
    if (step == STEP_LEGS) return STATE_TRAVERSE;
    if (step == STEP_RING && ringOwner != RING_JOB) return STATE_JOGGING;
    return faults ? STATE_ALARM : STATE_IDLE;
}

void machinePublish(void) {
    const uint16_t f = machineFaults();
    const uint32_t s = spin_lock_blocking(plannerLock);
    if (jobAborted && !abortRequested) {
        jobAborted = false;
        if (machineContext == CTX_JOB) machineContext = CTX_IDLE;
    }
    machineState = stateOf(f);
    spin_unlock(plannerLock, s);
}
