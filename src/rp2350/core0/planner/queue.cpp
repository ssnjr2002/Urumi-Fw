// queue.cpp — plannerQueueLine / plannerQueueBezier: push under plannerLock,
// replan without it, commit under it again. A commit refused because Core 1
// claimed or released a block meanwhile, or came too close to the running
// block's horizon, is replanned and retried with Core 1's clock read again.

#include <Arduino.h>
#include "queue.h"
#include "../config/machine_cfg.h"
#include "../ops/frames.h"
#include "../ops/mesh.h"
#include "../ops/position.h"
#include "../ops/machine.h"
#include "../../ipc/shared_state.h"
#include "planner/bezier.h"
#include "planner/mesh.h"

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
// joins JOGGING, a record a job that is buffering or running.
static PlannerQueueResult admit(planner::AxisLimits& limits, bool jog) {
    const uint8_t st = machineView();
    const uint8_t why = jobReason;
    // A tool change's line and the operator's jogs run inside the job.
    const bool tool = st == STATE_JOB && (why == JOB_TOOL_CHANGE || why == JOB_TOOL_WAIT);
    const bool joins = jog ? st == STATE_JOGGING || tool
                           : st == STATE_JOB && (why == JOB_BUFFERING || why == JOB_RUNNING);
    if ((st != STATE_IDLE && !joins) || pauseRequested || abortRequested)
        return PQ_BAD_STATE;
    if (!machineCfgValid()) return PQ_NO_CONFIG;

    // Z and A stay 0 on a slot no head binds; a move there is refused.
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) {
        const CfgAxis* a = framesAxis(k);
        if (!a) continue;
        limits.max_feed[k]  = a->maxFeed;
        limits.max_accel[k] = a->maxAccel;
    }
    if (!(limits.max_feed[0] > 0 && limits.max_accel[0] > 0 &&
          limits.max_feed[1] > 0 && limits.max_accel[1] > 0)) return PQ_NO_LIMITS;
    return PQ_OK;
}

// Signed steps per unit of slot k, 0 if no head binds it.
static float spmOf(uint8_t k) {
    const CfgAxis* a = framesAxis(k);
    if (!a) return 0;
    return a->invertDir ? -a->stepsPerUnit : a->stepsPerUnit;
}

// machinePos in planner units, Z flat (less the latched mesh). A splits into
// heading and turns in double, so a far-turned A starts exact.
static planner::Pos posFromSteps() {
    planner::Pos p;
    float* const c[3] = {&p.x, &p.y, &p.z};
    for (uint8_t k = 0; k < 3; k++) {
        const float spm = spmOf(k);
        *c[k] = spm != 0 ? machinePos[k] / spm : 0;
    }
    p.z -= meshLatchedOffset(p.x, p.y);
    const float spmA = spmOf(SLOT_A);
    if (spmA != 0) {
        const double deg = (double)machinePos[3] / spmA;
        const double turns = floor(deg / 360.0);
        p.turns = (int32_t)turns;
        p.a = (float)(deg - turns * 360.0);
        if (p.a >= 360.0f) { p.a -= 360.0f; p.turns++; }
    }
    return p;
}

// Under plannerLock: an empty, idle ring starts again from machinePos, owned by
// `reason`'s kind. True if it did.
static bool resetIfIdle(uint8_t reason) {
    if (plannerActive || plannerRing.count() != 0) return false;
    ringOwner = reason;
    // Signed, so planner units are machine units on an invertDir axis too.
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) plannerSpm[k] = spmOf(k);
    plannerTurnQ32 = (int64_t)llround((double)plannerSpm[3] * 360.0 * 4294967296.0);
    meshLatch();
    const planner::Pos at = posFromSteps();
    plannerRing.reset(at);
    plannerExec.reset(at);
    return true;
}

// ── the bed mesh along XY blocks ─────────────────────────────────────────────
// Under plannerLock, with the mesh latched for the ring (resetIfIdle). Along an
// XY block the actual Z is the ring's flat Z plus the offset: outside Z's soft
// range it is refused, checked from the motors' Z as a jog's is; otherwise the
// block is capped so Z keeps up. The walks are short (half the grid spacing a
// sample), so they stay under the lock.

static planner::Vec2 tipOffset() { return {plannerMesh.tipX, plannerMesh.tipY}; }

// The actual Z at the ring's end and the soft-range check of offset `off` from it.
static bool zInRange(float off) {
    const planner::Pos e = plannerRing.end();
    float left;
    return !framesCheckMove(SLOT_Z, e.z + meshLatchedOffset(e.x, e.y), e.z + off, &left);
}

static PlannerQueueResult meshFit(const planner::MeshWalk& w, const planner::AxisLimits& limits,
                                  planner::PathCap& cap) {
    if (!zInRange(w.lo) || !zInRange(w.hi)) return PQ_SOFT_LIMIT;
    if (limits.max_feed[SLOT_Z] > 0 && limits.max_accel[SLOT_Z] > 0)
        cap = planner::meshCap(w, limits.max_feed[SLOT_Z], limits.max_accel[SLOT_Z]);
    return PQ_OK;
}

static PlannerQueueResult meshLine(planner::Vec2 to, const planner::AxisLimits& limits,
                                   planner::PathCap& cap) {
    if (!plannerMesh.mesh) return PQ_OK;
    const planner::MeshWalk w = planner::meshWalkLine(*plannerMesh.mesh, plannerRing.end().xy(),
                                                      to, tipOffset(), plannerMesh.ref);
    return meshFit(w, limits, cap);
}

static PlannerQueueResult meshBezier(const planner::Bezier& b, const planner::AxisLimits& limits,
                                     planner::PathCap& cap) {
    if (!plannerMesh.mesh) return PQ_OK;
    const planner::MeshWalk w = planner::meshWalkBezier(*plannerMesh.mesh, b, tipOffset(),
                                                        plannerMesh.ref);
    return meshFit(w, limits, cap);
}

// The point toward `to` where the actual Z last stays in range: sampled at the
// walk's step, then the crossing bisected to 0.01 mm. The ring's end if Z
// leaves at once.
static planner::Vec2 meshClip(planner::Vec2 to) {
    const planner::Mesh& m = *plannerMesh.mesh;
    const planner::Vec2 p0 = plannerRing.end().xy();
    const float ex = to.x - p0.x, ey = to.y - p0.y;
    const float len = sqrtf(ex * ex + ey * ey);
    auto at = [&](float f) { return planner::Vec2{p0.x + ex * f, p0.y + ey * f}; };
    auto fits = [&](float f) { const planner::Vec2 p = at(f); return zInRange(meshLatchedOffset(p.x, p.y)); };
    const int n = (int)ceilf(len / (0.5f * fminf(m.dx, m.dy)));
    float ok = 0, bad = 0;
    for (int i = 1; i <= n; i++) {
        const float f = (float)i / (float)n;
        if (!fits(f)) { bad = f; break; }
        ok = f;
    }
    if (bad == 0) return to;
    while ((bad - ok) * len > 0.01f) {
        const float mid = 0.5f * (ok + bad);
        if (fits(mid)) ok = mid;
        else bad = mid;
    }
    return at(ok);
}

PlannerQueueResult plannerQueueLine(float x, float y, float feed, uint8_t reason) {
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, true);
    if (r != PQ_OK) return r;

    const uint32_t s = spin_lock_blocking(plannerLock);
    resetIfIdle(reason);
    const bool mine = ringOwner == reason;
    planner::Vec2 to = {x, y};
    planner::PathCap cap;
    PlannerQueueResult fit = mine ? meshLine(to, limits, cap) : PQ_OK;
    if (fit == PQ_SOFT_LIMIT && reason == RING_JOG_CONT) {
        to = meshClip(to);
        cap = planner::PathCap();
        const planner::Pos e = plannerRing.end();
        fit = to.x == e.x && to.y == e.y ? PQ_SOFT_LIMIT : meshLine(to, limits, cap);
    }
    const bool pushed = mine && fit == PQ_OK && plannerRing.pushLine(to, feed, limits, kDeviation, cap);
    spin_unlock(plannerLock, s);
    if (!mine) return PQ_BAD_STATE;
    if (fit != PQ_OK) return fit;
    if (!pushed) return PQ_FULL;

    replanAndCommit();
    return PQ_OK;
}

PlannerQueueResult plannerQueueAxis(uint8_t k, float d, float feed, uint8_t reason) {
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, true);
    if (r != PQ_OK) return r;
    if (k != SLOT_Z && k != SLOT_A) return PQ_BAD_STATE;
    if (!(limits.max_feed[k] > 0 && limits.max_accel[k] > 0)) return PQ_NO_LIMITS;

    const uint32_t s = spin_lock_blocking(plannerLock);
    resetIfIdle(reason);
    const bool mine = ringOwner == reason;
    planner::Pos to = plannerRing.end();
    if (k == SLOT_Z) to.z += d;
    else to.addA(d);
    const bool pushed = mine && plannerRing.pushMove(to, feed, limits, kDeviation);
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
    resetIfIdle(RING_JOG_STEP);
    const bool mine = ringOwner == RING_JOG_STEP;
    const planner::Vec2 p0 = plannerRing.end().xy();
    spin_unlock(plannerLock, s);
    if (!mine) return PQ_BAD_STATE;

    // The analysis (~0.6 ms) runs outside the lock. Only Core 0 pushes, so the
    // ring's end is still p0 unless an abort or cancel emptied it meanwhile.
    planner::Bezier b;
    if (planner::analyzeBezier(p0, {x1, y1}, {x2, y2}, {x3, y3}, b) != planner::BezierError::None)
        return PQ_BAD_CURVE;

    s = spin_lock_blocking(plannerLock);
    const planner::Pos e = plannerRing.end();
    const bool moved = e.x != p0.x || e.y != p0.y;
    planner::PathCap cap;
    const PlannerQueueResult fit = moved ? PQ_OK : meshBezier(b, limits, cap);
    const bool pushed = !moved && fit == PQ_OK && plannerRing.pushBezier(b, feed, limits, kDeviation, cap);
    spin_unlock(plannerLock, s);
    if (moved) return PQ_BAD_STATE;
    if (fit != PQ_OK) return fit;
    if (!pushed) return PQ_FULL;

    replanAndCommit();
    return PQ_OK;
}

// ── streamed records ─────────────────────────────────────────────────────────

// The `feed` command's override of the config's feeds; 0 = none.
static float cutFeed = 0, travelFeed = 0;
static float layerFeed = 0;     // the open layer's cut feed
static bool inContour = false;
// The record the next BEZIER is, and whether it is the first since a LAYER_HDR.
static uint16_t curLayer = 0;
static uint32_t nextRecord = 0;
static bool firstRecord = false;
// mm of the first record a resume skips.
static float resumeTrim = 0;

// How far before where a paused job stopped it resumes cutting, mm.
static constexpr float kResumeMargin = 2.0f;

void plannerSetFeed(float cut, float travel) {
    cutFeed = cut;
    travelFeed = travel;
}

float plannerMachineFeed() {
    if (cutFeed > 0) return cutFeed;
    return machineCfgValid() ? machineCfg().cutFallback : 0;
}

static float travelSpeed() {
    if (travelFeed > 0) return travelFeed;
    return machineCfgValid() ? machineCfg().travelDefault : 0;
}

void plannerEndContour() { inContour = false; }

static bool paused() { return machineContext == CTX_JOB && jobReason == JOB_PAUSED; }

bool plannerResumePoint(uint16_t* layer, uint32_t* record) {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool ok = paused();
    if (ok) {
        // An empty ring at the pause: the job goes on at the next record.
        *layer  = jobResume.known ? jobResume.layer  : curLayer;
        *record = jobResume.known ? jobResume.record : nextRecord;
    }
    spin_unlock(plannerLock, s);
    return ok;
}

static bool resumesAt(uint16_t layer, uint32_t record) {
    uint16_t l;
    uint32_t r;
    return plannerResumePoint(&l, &r) && l == layer && r == record;
}

PlannerQueueResult plannerLayerCheck(float cut, uint16_t layer, uint32_t record) {
    if (!(cut > 0 && travelSpeed() > 0)) return PQ_NO_FEED;
    if (paused()) {
        if (!machineCfgValid()) return PQ_NO_CONFIG;
        return resumesAt(layer, record) ? PQ_OK : PQ_NO_RESUME;
    }
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, false);
    if (r != PQ_OK) return r;
    return inContour ? PQ_BAD_CURVE : PQ_OK;
}

PlannerQueueResult plannerOpenLayer(float cut, uint16_t layer, uint32_t record) {
    const PlannerQueueResult r = plannerLayerCheck(cut, layer, record);
    if (r != PQ_OK) return r;
    const bool resume = paused();

    const uint32_t s = spin_lock_blocking(plannerLock);
    PlannerQueueResult out = PQ_OK;
    if (resume) {
        // Paused, the ring is empty and idle: the first record resets it, from
        // wherever the head is now.
        if (!paused()) {
            out = PQ_BAD_STATE;
        } else {
            resumeTrim = jobResume.known ? fmaxf(0.0f, jobResume.s - kResumeMargin) : 0;
            jobResume.known = false;
            inContour = false;
            __dmb();
            jobReason = JOB_BUFFERING;
        }
    } else if (machineContext != CTX_JOB) {
        // From IDLE: no jog runs, so the ring is empty and becomes the job's.
        if (resetIfIdle(RING_JOB)) {
            jobReason = JOB_BUFFERING;
            __dmb();
            machineContext = CTX_JOB;
        } else {
            out = PQ_BAD_STATE;
        }
    }
    if (out == PQ_OK) {
        layerFeed = cut;
        curLayer = layer;
        nextRecord = record;
        firstRecord = true;
    }
    spin_unlock(plannerLock, s);
    return out;
}

PlannerQueueResult plannerHoldForTool(bool* fromPaused) {
    *fromPaused = false;
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool idle = !plannerActive && plannerRing.count() == 0;
    const uint8_t why = jobReason;
    PlannerQueueResult out = PQ_OK;
    if (paused()) {
        *fromPaused = true;
        jobReason = JOB_TOOL_CHANGE;
    } else if (machineContext != CTX_JOB) {
        if (resetIfIdle(RING_JOB)) {
            jobReason = JOB_TOOL_CHANGE;
            __dmb();
            machineContext = CTX_JOB;
        } else {
            out = PQ_BAD_STATE;
        }
    } else if (why == JOB_BUFFERING && idle) {
        jobReason = JOB_TOOL_CHANGE;
    } else if (why == JOB_BUFFERING || why == JOB_RUNNING || why == JOB_HOLDING) {
        toolChangePending = true;
    } else {
        out = PQ_BAD_STATE;
    }
    spin_unlock(plannerLock, s);
    return out;
}

bool plannerJobSwap(uint8_t from, uint8_t to) {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool ok = machineContext == CTX_JOB && jobReason == from &&
                    !plannerActive && plannerRing.count() == 0;
    if (ok) {
        ringOwner = RING_JOB;
        __dmb();
        jobReason = to;
    }
    spin_unlock(plannerLock, s);
    return ok;
}

PlannerQueueResult plannerQueueRecord(planner::Bezier& b, bool start, bool end) {
    if (machineContext != CTX_JOB) return PQ_NO_JOB;
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, false);
    if (r != PQ_OK) return r;
    if (firstRecord) start = true;
    if (start == inContour) return PQ_BAD_CURVE;   // START inside a contour, or none outside
    if (planner::checkBezier(b) != planner::BezierError::None) return PQ_BAD_CURVE;

    // A resumed record starts at its trim, travelled to as a START's p0.
    const float from = firstRecord ? resumeTrim : 0;
    const planner::Vec2 p0 = from > 0 ? planner::bezierPoint(b, planner::bezierT(b, from)) : b.p[0];

    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool reset = resetIfIdle(RING_JOB);
    const planner::Pos e = plannerRing.end();
    const float dx = p0.x - e.x, dy = p0.y - e.y;
    PlannerQueueResult out = PQ_OK;
    bool travel = false;
    if (ringOwner != RING_JOB) {
        out = PQ_BAD_STATE;                         // a jog owns the ring
    } else if (dx == 0 && dy == 0) {
        if (plannerRing.full()) out = PQ_FULL;
    } else if (!start) {
        // An idle reset restarts the ring at machinePos, which rounds p3 to a
        // step; a contour continuing after a drain is within one step of it.
        const float step = 1 / fminf(fabsf(plannerSpm[0]), fabsf(plannerSpm[1]));
        if (reset && fabsf(dx) <= step && fabsf(dy) <= step) b.p[0] = e.xy();
        else out = PQ_BAD_CURVE;
        if (out == PQ_OK && plannerRing.full()) out = PQ_FULL;
    } else if (plannerRing.count() > planner::Planner::kSize - 2) {
        out = PQ_FULL;                              // travel and curve go in together
    } else {
        travel = true;
    }
    // Both walked before either goes in; the curve keeps the travel's flat Z.
    planner::PathCap travelCap, curveCap;
    if (out == PQ_OK && travel) out = meshLine(p0, limits, travelCap);
    if (out == PQ_OK) out = meshBezier(b, limits, curveCap);
    if (out == PQ_OK) {
        plannerRing.setTag(curLayer, nextRecord);
        if (travel) plannerRing.pushLine(p0, travelSpeed(), limits, kDeviation, travelCap);
        plannerRing.pushBezierFrom(b, from, layerFeed, limits, kDeviation, curveCap);
    }
    spin_unlock(plannerLock, s);
    if (out != PQ_OK) return out;

    nextRecord++;
    firstRecord = false;
    resumeTrim = 0;
    inContour = !end;
    replanAndCommit();
    return PQ_OK;
}

bool plannerJogFrom(float at[4], uint8_t reason) {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool idle = !plannerActive && plannerRing.count() == 0;
    const bool ok = idle || ringOwner == reason;
    if (ok) {
        // Z is actual: the soft range holds on the motors' Z.
        if (idle) meshLatch();
        const planner::Pos e = idle ? posFromSteps() : plannerRing.end();
        at[0] = e.x;
        at[1] = e.y;
        at[2] = e.z + meshLatchedOffset(e.x, e.y);
        at[3] = e.a + e.turns * 360.0f;
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
        // be consumed as "nothing to stop" and leave the ring to run. The
        // latch is this ring's, so the flat position matches its blocks'.
        const planner::Pos at = posFromSteps();
        plannerRing.reset(at);
        plannerExec.reset(at);
    }
    spin_unlock(plannerLock, s);
}

int plannerQueueDepth() { return plannerRing.count(); }
