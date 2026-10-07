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

// The state, config and limit checks every push shares; fills `limits`. A jog
// joins JOGGING, a record a running planner job.
static PlannerQueueResult admit(planner::AxisLimits& limits, bool jog) {
    const uint8_t st = machineState;
    const bool running = jog ? st == STATE_JOGGING
                             : st == STATE_RUNNING && runningReason == RUNNING_PLANNER;
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

// Under plannerLock: an empty, idle ring starts again from machinePos, owned by
// `reason`'s kind. True if it did.
static bool resetIfIdle(uint8_t reason) {
    if (plannerActive || plannerRing.count() != 0) return false;
    joggingReason = reason;
    // Signed, so planner mm are machine mm on an invertDir axis too.
    const MachineCfg& cfg = machineCfg();
    plannerSpm[0] = cfg.x.invertDir ? -cfg.x.stepsPerUnit : cfg.x.stepsPerUnit;
    plannerSpm[1] = cfg.y.invertDir ? -cfg.y.stepsPerUnit : cfg.y.stepsPerUnit;
    const planner::Vec2 at{machinePos[0] / plannerSpm[0], machinePos[1] / plannerSpm[1]};
    plannerRing.reset(at);
    plannerExec.reset(at);
    return true;
}

PlannerQueueResult plannerQueueLine(float x, float y, float feed, uint8_t reason) {
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, true);
    if (r != PQ_OK) return r;

    const uint32_t s = spin_lock_blocking(plannerLock);
    resetIfIdle(reason);
    const bool mine = joggingReason == reason;
    const bool pushed = mine && plannerRing.pushLine({x, y}, feed, limits, kDeviation);
    spin_unlock(plannerLock, s);
    if (!mine) return PQ_BAD_STATE;
    if (!pushed) return PQ_FULL;

    replanAndCommit();
    return PQ_OK;
}

PlannerQueueResult plannerQueueBezier(float x1, float y1, float x2, float y2,
                                      float x3, float y3, float feed) {
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, true);
    if (r != PQ_OK) return r;

    uint32_t s = spin_lock_blocking(plannerLock);
    resetIfIdle(JOGGING_STEP);
    const bool mine = joggingReason == JOGGING_STEP;
    const planner::Vec2 p0 = plannerRing.end();
    spin_unlock(plannerLock, s);
    if (!mine) return PQ_BAD_STATE;

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

// ── streamed records ─────────────────────────────────────────────────────────

static float cutFeed = 0, travelFeed = 0;
static bool inContour = false;

void plannerSetFeed(float cut, float travel) {
    cutFeed = cut;
    travelFeed = travel;
}

void plannerEndContour() { inContour = false; }

PlannerQueueResult plannerQueueRecord(planner::Bezier& b, bool start, bool end) {
    if (!(cutFeed > 0 && travelFeed > 0)) return PQ_NO_FEED;
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, false);
    if (r != PQ_OK) return r;
    if (start == inContour) return PQ_BAD_CURVE;   // START inside a contour, or none outside
    if (planner::checkBezier(b) != planner::BezierError::None) return PQ_BAD_CURVE;

    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool reset = resetIfIdle(JOGGING_NONE);
    const planner::Vec2 e = plannerRing.end();
    const float dx = b.p[0].x - e.x, dy = b.p[0].y - e.y;
    PlannerQueueResult out = PQ_OK;
    if (joggingReason != JOGGING_NONE) {
        out = PQ_BAD_STATE;                         // a jog owns the ring
    } else if (dx == 0 && dy == 0) {
        if (plannerRing.full()) out = PQ_FULL;
    } else if (!start) {
        // An idle reset restarts the ring at machinePos, which rounds p3 to a
        // step; a contour continuing after a drain is within one step of it.
        const float step = 1 / fminf(fabsf(plannerSpm[0]), fabsf(plannerSpm[1]));
        if (reset && fabsf(dx) <= step && fabsf(dy) <= step) b.p[0] = e;
        else out = PQ_BAD_CURVE;
        if (out == PQ_OK && plannerRing.full()) out = PQ_FULL;
    } else if (plannerRing.count() > planner::Planner::kSize - 2) {
        out = PQ_FULL;                              // travel and curve go in together
    } else {
        plannerRing.pushLine(b.p[0], travelFeed, limits, kDeviation);
    }
    if (out == PQ_OK) plannerRing.pushBezier(b, cutFeed, limits, kDeviation);
    spin_unlock(plannerLock, s);
    if (out != PQ_OK) return out;

    inContour = !end;
    replanAndCommit();
    return PQ_OK;
}

bool plannerJogFrom(float* x, float* y, uint8_t reason) {
    const MachineCfg& cfg = machineCfg();
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool idle = !plannerActive && plannerRing.count() == 0;
    const bool ok = idle || joggingReason == reason;
    if (idle) {
        *x = machinePos[0] / (cfg.x.invertDir ? -cfg.x.stepsPerUnit : cfg.x.stepsPerUnit);
        *y = machinePos[1] / (cfg.y.invertDir ? -cfg.y.stepsPerUnit : cfg.y.stepsPerUnit);
    } else if (ok) {
        const planner::Vec2 e = plannerRing.end();
        *x = e.x;
        *y = e.y;
    }
    spin_unlock(plannerLock, s);
    return ok;
}

void plannerStopJog() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    if (plannerActive) {
        abortRequested = true;     // Core 1 brakes, discards the ring, lands IDLE
    } else if (plannerRing.count() != 0) {
        // Core 1 has not taken the ring yet: drop it here, where an abort could
        // be consumed as "nothing to stop" and leave the ring to run.
        const planner::Vec2 at{machinePos[0] / plannerSpm[0], machinePos[1] / plannerSpm[1]};
        plannerRing.reset(at);
        plannerExec.reset(at);
    }
    spin_unlock(plannerLock, s);
}

int plannerQueueDepth() { return plannerRing.count(); }
