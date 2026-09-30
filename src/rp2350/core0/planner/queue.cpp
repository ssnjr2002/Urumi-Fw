// queue.cpp — plannerQueueLine / plannerQueueBezier: push under plannerLock,
// replan without it, commit under it again. A commit refused because Core 1
// claimed or released a block meanwhile, or came too close to the running
// block's horizon, is replanned and retried with Core 1's clock read again.

#include <Arduino.h>
#include "queue.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"
#include "planner/bezier.h"

// Junction deviation, mm: how far a corner may be rounded at junction speed.
static constexpr float kDeviation = 0.02f;

static void replanAndCommit() {
    for (;;) {
        plannerRing.replan(plannerExec.clock());
        const uint32_t s = spin_lock_blocking(plannerLock);
        const bool ok = plannerRing.commit(plannerExec.clock());
        spin_unlock(plannerLock, s);
        if (ok) return;
    }
}

// The state, config and limit checks both moves share; fills `limits`.
static PlannerQueueResult admit(planner::AxisLimits& limits) {
    const uint8_t st = machineState;
    const bool running = st == STATE_RUNNING && runningReason == RUNNING_PLANNER;
    if ((st != STATE_IDLE && !running) || pauseRequested || abortRequested)
        return PQ_BAD_STATE;
    if (!machineCfgValid()) return PQ_NO_CONFIG;

    const MachineCfg& cfg = machineCfg();
    const CfgAxis* axes[2] = {&cfg.x, &cfg.y};
    for (int i = 0; i < 2; i++) {
        if (axes[i]->maxFeed <= 0 || axes[i]->maxAccel <= 0) return PQ_NO_LIMITS;
        limits.max_feed[i]  = axes[i]->maxFeed;
        limits.max_accel[i] = axes[i]->maxAccel;
    }
    return PQ_OK;
}

// Under plannerLock: an empty, idle ring starts again from machinePos.
static void resetIfIdle() {
    if (plannerActive || plannerRing.count() != 0) return;
    const MachineCfg& cfg = machineCfg();
    plannerSpm[0] = cfg.x.stepsPerUnit;
    plannerSpm[1] = cfg.y.stepsPerUnit;
    const planner::Vec2 at{machinePos[0] / plannerSpm[0], machinePos[1] / plannerSpm[1]};
    plannerRing.reset(at);
    plannerExec.reset(at);
}

PlannerQueueResult plannerQueueLine(float x, float y, float feed) {
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits);
    if (r != PQ_OK) return r;

    const uint32_t s = spin_lock_blocking(plannerLock);
    resetIfIdle();
    const bool pushed = plannerRing.push({x, y}, feed, limits, kDeviation);
    spin_unlock(plannerLock, s);
    if (!pushed) return PQ_FULL;

    replanAndCommit();
    return PQ_OK;
}

PlannerQueueResult plannerQueueBezier(float x1, float y1, float x2, float y2,
                                      float x3, float y3, float feed) {
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits);
    if (r != PQ_OK) return r;

    uint32_t s = spin_lock_blocking(plannerLock);
    resetIfIdle();
    const planner::Vec2 p0 = plannerRing.end();
    spin_unlock(plannerLock, s);

    // The analysis (~0.6 ms) runs outside the lock. Only Core 0 pushes, so the
    // ring's end is still p0 unless an abort or cancel emptied it meanwhile.
    planner::Bezier b;
    if (planner::analyzeBezier(p0, {x1, y1}, {x2, y2}, {x3, y3}, b) != planner::BezierError::None)
        return PQ_BAD_CURVE;

    s = spin_lock_blocking(plannerLock);
    const planner::Vec2 e = plannerRing.end();
    const bool moved = e.x != p0.x || e.y != p0.y;
    const bool pushed = !moved && plannerRing.pushBezier(b, feed, limits, kDeviation);
    spin_unlock(plannerLock, s);
    if (moved) return PQ_BAD_STATE;
    if (!pushed) return PQ_FULL;

    replanAndCommit();
    return PQ_OK;
}

int plannerQueueDepth() { return plannerRing.count(); }
