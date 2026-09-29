// queue.cpp — plannerQueueLine: push under plannerLock, replan without it,
// commit under it again. A commit refused because Core 1 claimed or released a
// block meanwhile is replanned and retried.

#include <Arduino.h>
#include "queue.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"

// Junction deviation, mm: how far a corner may be rounded at junction speed.
static constexpr float kDeviation = 0.02f;

static void replanAndCommit() {
    for (;;) {
        plannerRing.replan();
        const uint32_t s = spin_lock_blocking(plannerLock);
        const bool ok = plannerRing.commit();
        spin_unlock(plannerLock, s);
        if (ok) return;
    }
}

PlannerQueueResult plannerQueueLine(float x, float y, float feed) {
    const uint8_t st = machineState;
    const bool running = st == STATE_RUNNING && runningReason == RUNNING_PLANNER;
    if ((st != STATE_IDLE && !running) || pauseRequested || abortRequested)
        return PQ_BAD_STATE;
    if (!machineCfgValid()) return PQ_NO_CONFIG;

    const MachineCfg& cfg = machineCfg();
    const CfgAxis* axes[2] = {&cfg.x, &cfg.y};
    planner::AxisLimits limits;
    for (int i = 0; i < 2; i++) {
        if (axes[i]->maxFeed <= 0 || axes[i]->maxAccel <= 0) return PQ_NO_LIMITS;
        limits.max_feed[i]  = axes[i]->maxFeed;
        limits.max_accel[i] = axes[i]->maxAccel;
    }

    const uint32_t s = spin_lock_blocking(plannerLock);
    if (!plannerActive && plannerRing.count() == 0) {
        plannerSpm[0] = cfg.x.stepsPerUnit;
        plannerSpm[1] = cfg.y.stepsPerUnit;
        const planner::Vec2 at{machinePos[0] / plannerSpm[0], machinePos[1] / plannerSpm[1]};
        plannerRing.reset(at);
        plannerExec.reset(at);
    }
    const bool pushed = plannerRing.push({x, y}, feed, limits, kDeviation);
    spin_unlock(plannerLock, s);
    if (!pushed) return PQ_FULL;

    replanAndCommit();
    return PQ_OK;
}

int plannerQueueDepth() { return plannerRing.count(); }
