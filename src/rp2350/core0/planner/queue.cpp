// queue.cpp — plannerQueueLine / plannerQueueBezier: push under plannerLock,
// replan without it, commit under it again. A commit refused because Core 1
// claimed or released a block meanwhile, or came too close to the running
// block's horizon, is replanned and retried with Core 1's clock read again.

#include <Arduino.h>
#include "queue.h"
#include "../config/machine_cfg.h"
#include "../ops/frames.h"
#include "../ops/layer.h"
#include "../ops/mesh.h"
#include "../ops/position.h"
#include "../ops/machine.h"
#include "../../ipc/shared_state.h"
#include "common.h"
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

// Each slot's ceilings from the axis bound there; false without X and Y's.
static bool fillLimits(planner::AxisLimits& limits) {
    // Z and A stay 0 on a slot no head binds; a move there is refused.
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) {
        const CfgAxis* a = framesAxis(k);
        if (!a) continue;
        limits.max_feed[k]  = a->maxFeed;
        limits.max_accel[k] = a->maxAccel;
    }
    return limits.max_feed[0] > 0 && limits.max_accel[0] > 0 &&
           limits.max_feed[1] > 0 && limits.max_accel[1] > 0;
}

// The state, config and limit checks every push shares; fills `limits`. A jog
// joins JOGGING, a record a job that is buffering or running.
static PlannerQueueResult admit(planner::AxisLimits& limits, bool jog) {
    const uint8_t st = machineView();
    const uint8_t why = jobReason;
    // A tool change's line and the operator's jogs run inside the job.
    const bool tool = st == STATE_JOB && (why == JOB_TOOL_CHANGE || why == JOB_TOOL_WAIT);
    const bool joins = jog ? st == STATE_JOGGING || tool
                           : st == STATE_JOB && (why == JOB_BUFFERING || why == JOB_RUNNING ||
                                                 why == JOB_DWELL);
    if ((st != STATE_IDLE && !joins) || pauseRequested || abortRequested)
        return PQ_BAD_STATE;
    if (!machineCfgValid()) return PQ_NO_CONFIG;
    return fillLimits(limits) ? PQ_OK : PQ_NO_LIMITS;
}

// Signed steps per unit of slot k, 0 if no head binds it.
static float spmOf(uint8_t k) {
    const CfgAxis* a = framesAxis(k);
    if (!a) return 0;
    return a->invertDir ? -a->stepsPerUnit : a->stepsPerUnit;
}

// machinePos in planner units: XY the blade tip (less the latched blade's
// lead), Z flat (less the latched mesh). A splits into heading and turns in
// double, so a far-turned A starts exact.
static planner::Pos posFromSteps() {
    planner::Pos p;
    float* const c[3] = {&p.x, &p.y, &p.z};
    for (uint8_t k = 0; k < 3; k++) {
        const float spm = spmOf(k);
        *c[k] = spm != 0 ? machinePos[k] / spm : 0;
    }
    const float spmA = spmOf(SLOT_A);
    if (spmA != 0) {
        const double deg = (double)machinePos[3] / spmA;
        const double turns = floor(deg / 360.0);
        p.turns = (int32_t)turns;
        p.a = (float)(deg - turns * 360.0);
        if (p.a >= 360.0f) { p.a -= 360.0f; p.turns++; }
    }
    const planner::Vec2 lead = bladeLead(plannerBlade, p.a);
    p.x -= lead.x;
    p.y -= lead.y;
    p.z -= meshLatchedOffset(p.x, p.y);
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
    framesBlade(&plannerBlade.d, &plannerBlade.aOffsetDeg);
    plannerRing.setBlade(plannerBlade.d);
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

// The soft-range check of flat Z `z` at offset `off`, from the actual Z at the
// ring's end.
static bool zInRange(float z, float off) {
    const planner::Pos e = plannerRing.end();
    float left;
    return !framesCheckMove(SLOT_Z, e.z + meshLatchedOffset(e.x, e.y), z + off, &left, off);
}

// A block at flat Z `z` whose offset walk is `w`.
static PlannerQueueResult meshFit(const planner::MeshWalk& w, float z,
                                  const planner::AxisLimits& limits, planner::PathCap& cap) {
    if (!zInRange(z, w.lo) || !zInRange(z, w.hi)) return PQ_SOFT_LIMIT;
    if (limits.max_feed[SLOT_Z] > 0 && limits.max_accel[SLOT_Z] > 0)
        cap = planner::meshCap(w, limits.max_feed[SLOT_Z], limits.max_accel[SLOT_Z]);
    return PQ_OK;
}

static PlannerQueueResult meshLine(planner::Vec2 to, float z, const planner::AxisLimits& limits,
                                   planner::PathCap& cap) {
    if (!plannerMesh.mesh) return PQ_OK;
    const planner::MeshWalk w = planner::meshWalkLine(*plannerMesh.mesh, plannerRing.end().xy(),
                                                      to, tipOffset(), plannerMesh.ref);
    return meshFit(w, z, limits, cap);
}

static PlannerQueueResult meshBezier(const planner::Bezier& b, float z,
                                     const planner::AxisLimits& limits, planner::PathCap& cap) {
    if (!plannerMesh.mesh) return PQ_OK;
    const planner::MeshWalk w = planner::meshWalkBezier(*plannerMesh.mesh, b, tipOffset(),
                                                        plannerMesh.ref);
    return meshFit(w, z, limits, cap);
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
    const float z = plannerRing.end().z;
    auto fits = [&](float f) { const planner::Vec2 p = at(f); return zInRange(z, meshLatchedOffset(p.x, p.y)); };
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

PlannerQueueResult plannerQueueLine(float x, float y, float feed, uint8_t reason, bool aZero) {
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, true);
    if (r != PQ_OK) return r;

    const uint32_t s = spin_lock_blocking(plannerLock);
    resetIfIdle(reason);
    const bool mine = ringOwner == reason;
    planner::Vec2 to = {x, y};
    planner::PathCap cap;
    PlannerQueueResult fit = mine ? meshLine(to, plannerRing.end().z, limits, cap) : PQ_OK;
    if (fit == PQ_SOFT_LIMIT && reason == RING_JOG_CONT) {
        to = meshClip(to);
        cap = planner::PathCap();
        const planner::Pos e = plannerRing.end();
        fit = to.x == e.x && to.y == e.y ? PQ_SOFT_LIMIT : meshLine(to, e.z, limits, cap);
    }
    planner::Pos target = plannerRing.end();
    target.setXy(to);
    if (aZero) target.addA(planner::Pos().aSince(target));
    // A turning swings a blade's axis about the tip, from a start that must fit.
    if (fit == PQ_OK && target.aSince(plannerRing.end()) != 0 &&
        framesCheckXY(plannerRing.end().x, plannerRing.end().y))
        fit = PQ_SOFT_LIMIT;
    const bool pushed = mine && fit == PQ_OK && plannerRing.pushMove(target, feed, limits, kDeviation, cap);
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
    const PlannerQueueResult fit = moved ? PQ_OK : meshBezier(b, e.z, limits, cap);
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
static LayerMotion layerMo = {}; // the open layer's feeds and heights
static bool inContour = false;
// The record the next BEZIER is, and whether it is the first since a LAYER_HDR.
static uint16_t curLayer = 0;
static uint32_t nextRecord = 0;
static bool firstRecord = false;
// mm of the first record a resume skips.
static float resumeTrim = 0;
// Core 0's estimate of the tool's on-time where the queued records end, s: it
// counts from the switch on, each stretch's time from rest to rest at the cut
// feed, and goes back to 0 where a START will dwell or a duty lift was queued.
// Curvature slows a cut more than this counts; Core 1's deadline catches it.
static float estOnS = 0;
// The record's stretch was counted at its START or BREAK (nextBreakDist set).
static bool stretchCounted = false;

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

bool plannerExecPoint(uint16_t* layer, uint32_t* record, uint8_t* ctx) {
    const uint32_t s = spin_lock_blocking(plannerLock);
    *ctx = machineContext;
    const bool ok = *ctx == CTX_JOB;
    if (ok) {
        const planner::Block* b = plannerRing.oldest();
        if (paused() && jobResume.known) { *layer = jobResume.layer; *record = jobResume.record; }
        else if (paused())  { *layer = curLayer;      *record = nextRecord; }
        else if (jobExec.known) { *layer = jobExec.layer; *record = jobExec.record; }
        else if (b)        { *layer = b->layer;      *record = b->record; }
        else               { *layer = curLayer;      *record = nextRecord; }
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

PlannerQueueResult plannerOpenLayer(const LayerMotion& mo, uint16_t layerNo, uint32_t record) {
    const PlannerQueueResult r = plannerLayerCheck(mo.cutFeed, layerNo, record);
    if (r != PQ_OK) return r;
    const bool resume = paused();

    const uint32_t s = spin_lock_blocking(plannerLock);
    PlannerQueueResult out = PQ_OK;
    const bool opening = machineContext != CTX_JOB;
    if (resume) {
        // Paused, the ring is empty and idle: the first record resets it, from
        // wherever the head is now.
        if (!paused()) {
            out = PQ_BAD_STATE;
        } else {
            resumeTrim = jobResume.known ? fmaxf(0.0f, jobResume.s - kResumeMargin) : 0;
            jobResume.known = false;
            jobExec.known = false;
            inContour = false;
            __dmb();
            jobReason = JOB_BUFFERING;
        }
    } else if (machineContext != CTX_JOB) {
        // From IDLE: no jog runs, so the ring is empty and becomes the job's.
        if (resetIfIdle(RING_JOB)) {
            jobExec.known = false;
            jobLift = {0, 0, 0};
            jobReason = JOB_BUFFERING;
            __dmb();
            machineContext = CTX_JOB;
        } else {
            out = PQ_BAD_STATE;
        }
    }
    if (out == PQ_OK) {
        if (opening || mo.tool != layerMo.tool) {
            dutyEpoch = dutyEpoch + 1;
            estOnS = 0;
        }
        const CfgPower& p = mo.power;
        jobPower.node    = p.node;
        jobPower.cmd     = p.nodeType == CFG_NODE_KNIFE_OSC ? CMD_KNIFE_OSC : CMD_LASER;
        jobPower.settleS = p.settleS;
        jobPower.hasDuty = p.hasDuty;
        jobPower.minOnS  = p.minOnS;
        jobPower.dwellS  = p.dwellS;
        jobPower.maxOnS  = p.maxOnS;
        layerMo = mo;
        curLayer = layerNo;
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
    } else if (why == JOB_BUFFERING || why == JOB_RUNNING || why == JOB_DWELL ||
               why == JOB_HOLDING) {
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

// Flat Z of a lift or travel (zClearance over the material top, work Z the
// bed) and of the cut (the layer's depth under the top).
static float clearZ() { return framesWork(SLOT_Z) + layerMo.top + machineCfg().zClearance; }
static float cutZ()   { return framesWork(SLOT_Z) + layerMo.top - layerMo.depth; }

// A Z-only move at (x, y) from flat `from` to flat `to` inside Z's soft range.
static bool zMoveFits(planner::Vec2 at, float from, float to) {
    const float off = meshLatchedOffset(at.x, at.y);
    float left;
    return !framesCheckMove(SLOT_Z, from + off, to + off, &left, off);
}

static void pushZ(float z, float feed, const planner::AxisLimits& limits) {
    planner::Pos to = plannerRing.end();
    to.z = z;
    plannerRing.pushMove(to, feed, limits, kDeviation);
}

// A tangential tool's heading, degrees: + counter-clockwise from X+
// (coordinate-system.md, Directions).
static float headingDeg(planner::Vec2 d) { return atan2f(d.y, d.x) * (180.0f / (float)M_PI); }

static double aAbs(const planner::Pos& p) { return (double)p.turns * 360.0 + p.a; }

// The equivalent of heading `h` mod `period` nearest `cur` whose curve, turning
// `turn` from it, stays inside ±`aMax` (0 = no cable); false if none fits.
static bool pickA(double cur, float h, float period, float aMax,
                  const planner::BezierTurn& turn, double* out) {
    double c = h + period * round((cur - h) / period);
    if (aMax > 0) {
        const double hi = aMax - turn.hi, lo = -aMax - turn.lo;
        if (c > hi) c -= period * ceil((c - hi) / period);
        else if (c < lo) c += period * ceil((lo - c) / period);
        if (c > hi || c < lo) return false;
    }
    *out = c;
    return true;
}

// The equivalent of heading `h` mod `period` whose sweep `lo`..`hi` (degrees
// from it) stays inside ±`aMax` (> 0), the sweep's middle nearest A = 0, ties
// to the nearest `cur`; false if none fits.
static bool pickCentredA(double cur, float h, float period, float aMax, double lo, double hi,
                         double* out) {
    // The fitting starts form an interval centred on −mid.
    const double mid = (lo + hi) / 2;
    const double c0 = h + period * floor((-mid - h) / period), c1 = c0 + period;
    const double d0 = fabs(c0 + mid), d1 = fabs(c1 + mid);
    const double c = fabs(d0 - d1) < 1e-6 ? (fabs(c0 - cur) <= fabs(c1 - cur) ? c0 : c1)
                                          : (d0 < d1 ? c0 : c1);
    if (c + hi > aMax || c + lo < -aMax) return false;
    *out = c;
    return true;
}

// For a sweep `lo`..`hi` too wide for ±`aMax`: the equivalent fitting this
// record's own `turn` nearest the edge the sweep turns away from, leaving the
// most room before the next unwind; false if none fits.
static bool pickRoomA(float h, float period, float aMax, double lo, double hi,
                      const planner::BezierTurn& turn, double* out) {
    const double edge = lo + hi >= 0 ? -aMax - turn.lo : aMax - turn.hi;
    return pickA(edge, h, period, aMax, turn, out);
}

// Host and Pico unwrap the heading in different float widths.
static constexpr double kSweepMarginDeg = 0.1;

static planner::Pos withA(planner::Pos to, double a) {
    to.addA((float)(a - aAbs(to)));
    return to;
}

static void pushA(double a, const planner::AxisLimits& limits) {
    plannerRing.pushMove(withA(plannerRing.end(), a), limits.max_feed[SLOT_A], limits, kDeviation);
}

// Seconds for `d` mm from rest to rest at speed `v` and acceleration `a`.
static float restToRestS(float d, float v, float a) {
    return d >= v * v / a ? d / v + v / a : 2 * sqrtf(d / a);
}

static float zMoveS(float d, float feed, const planner::AxisLimits& limits) {
    return restToRestS(d, fminf(feed, limits.max_feed[SLOT_Z]), limits.max_accel[SLOT_Z]);
}

// A cut of `d` mm from rest to rest, at a BEZIER's speed and acceleration
// (planner::pathOf) before curvature limits it.
static float cutS(float d, const planner::AxisLimits& limits) {
    const float v = fminf(layerMo.cutFeed, fminf(limits.max_feed[0], limits.max_feed[1]));
    const float a = fminf(limits.max_accel[0], limits.max_accel[1]) * 0.70710678f;
    return restToRestS(d, v, a);
}

// Slack under maxOnS for a duty lift: covers Core 0 picking up the deadline's fact.
static constexpr float kDutyMarginS = 0.05f;

PlannerQueueResult plannerQueueRecord(planner::Bezier& b, bool start, bool brk, bool end,
                                      uint16_t breakDist, int16_t sweepMin, int16_t sweepMax) {
    if (machineContext != CTX_JOB) return PQ_NO_JOB;
    planner::AxisLimits limits;
    const PlannerQueueResult r = admit(limits, false);
    if (r != PQ_OK) return r;
    if (firstRecord) start = true;
    if (start == inContour) return PQ_BAD_CURVE;   // START inside a contour, or none outside
    if (planner::checkBezier(b) != planner::BezierError::None) return PQ_BAD_CURVE;
    if (!(limits.max_feed[SLOT_Z] > 0 && limits.max_accel[SLOT_Z] > 0)) return PQ_NO_LIMITS;
    const bool tangential = layerMo.tangential;
    const bool chamber = layerMo.chamber;
    if ((tangential || chamber) && !(limits.max_feed[SLOT_A] > 0 && limits.max_accel[SLOT_A] > 0))
        return PQ_NO_LIMITS;
    // An angle means nothing on an A that is not homed.
    if ((tangential || chamber) && !(axes_homed & (1u << SLOT_A))) return PQ_BAD_STATE;

    // A resumed record starts at its trim, travelled to as a START's p0.
    const float from = firstRecord ? resumeTrim : 0;
    const planner::Vec2 p0 = from > 0 ? planner::bezierPoint(b, planner::bezierT(b, from)) : b.p[0];
    const float liftFeed = machineCfg().liftFeed;
    const float up = clearZ(), down = cutZ();
    // Measured from p0; the trim is shorter than the stretch, which holds this record.
    if (from > 0 && breakDist != 0 && breakDist != UINT16_MAX)
        breakDist = (uint16_t)ceilf(breakDist - from);

    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool reset = resetIfIdle(RING_JOB);
    const planner::Pos e = plannerRing.end();
    const float dx = p0.x - e.x, dy = p0.y - e.y;
    // A START: Z to clearance, travel, [tool on,] plunge, then the curve; an
    // END lifts after it [and switches the tool off].
    const bool powered = layerMo.power.node != 0;
    const CfgPower& pw = layerMo.power;
    const float liftS = zMoveS(up - down, liftFeed, limits);
    const float plungeS = zMoveS(up - down, layerMo.plungeFeed, limits);
    // At a BREAK past minOnS, a duty lift here if the next corner is certainly
    // too late: lift, tool off, tool on (its dwell), plunge, then the curve.
    const bool dutyStop = powered && pw.hasDuty && !start && breakDist != 0 &&
                          estOnS >= pw.minOnS &&
                          estOnS + cutS(breakDist, limits) + liftS + kDutyMarginS > pw.maxOnS;
    // A tool with a duty keeps 4 slots for a deadline lift (plannerDutyLift).
    int blocks = 1 + (start ? 3 + powered : 0) + (dutyStop ? 4 : 0) +
                       (end ? 1 + powered : 0) + (powered && pw.hasDuty ? 4 : 0);
    // A tangential tool follows the curve's heading. It turns to the start
    // heading during a START's travel and swivels at a BREAK, lifting for a
    // swivel past cornerAngleDeg; the whole curve stays inside the cable.
    const planner::Vec2 d0 = from > 0 ? planner::bezierDeriv(b, planner::bezierT(b, from))
                                      : planner::Vec2{b.p[1].x - b.p[0].x, b.p[1].y - b.p[0].y};
    const bool swivel = tangential && (start || brk);
    const planner::BezierTurn turn = tangential ? planner::bezierTurn(b, from) : planner::BezierTurn();
    double aTo = aAbs(e);
    // The rest of the stretch from p0, or from a resume's trim: the sweep less
    // the trimmed part's turn, still holding this record's own.
    const double shift = from > 0 ? planner::bezierTurn(b).total - turn.total : 0;
    const double sweepLo = fmin(sweepMin - kSweepMarginDeg - shift, turn.lo);
    const double sweepHi = fmax(sweepMax + kSweepMarginDeg - shift, turn.hi);
    // Machine A is the heading less the tool's edge offset at unlock.
    const float h0 = headingDeg(d0) - layerMo.aOffsetDeg;
    const bool cable = tangential && layerMo.aMaxDeg > 0;
    const auto pickFit = [&](float h) {
        return pickCentredA(aAbs(e), h, layerMo.periodDeg, layerMo.aMaxDeg, sweepLo, sweepHi, &aTo) ||
               pickRoomA(h, layerMo.periodDeg, layerMo.aMaxDeg, sweepLo, sweepHi, turn, &aTo);
    };
    bool aFits = !swivel || (cable ? pickFit(h0) : pickA(aAbs(e), h0, layerMo.periodDeg, 0, turn, &aTo));
    // A smooth join this record no longer fits from unwinds: lift, A by whole
    // periods from where it is, plunge at the same heading.
    const bool unwind = cable && !swivel && (aTo + turn.hi > layerMo.aMaxDeg ||
                                             aTo + turn.lo < -layerMo.aMaxDeg);
    if (unwind) aFits = pickFit((float)aAbs(e));
    // A chamber layer turns to its chamber during a START's travel and holds it.
    if (chamber && start) aFits = pickA(aAbs(e), layerMo.chamberDeg, 360, layerMo.aMaxDeg, turn, &aTo);
    if (tangential && layerMo.aMaxDeg > 0)
        aFits = aFits && aTo + turn.hi <= layerMo.aMaxDeg && aTo + turn.lo >= -layerMo.aMaxDeg;
    const bool cornerLift = swivel && !start && !dutyStop &&
                            fabs(aTo - aAbs(e)) > layerMo.cornerAngleDeg;
    // A planned duty stop needs a breakDist, sent only on a START or BREAK, so
    // it likely never meets a smooth-join unwind; kept as a guard.
    const bool unwindLift = unwind && !dutyStop;
    // A duty stop does a corner's or an unwind's turn in its own lift; it
    // carries their reasons too.
    const uint8_t breakLift =
        (dutyStop ? LIFT_DUTY : 0) |
        (swivel && !start && fabs(aTo - aAbs(e)) > layerMo.cornerAngleDeg ? LIFT_CORNER : 0) |
        (unwind ? LIFT_UNWIND : 0);
    blocks += (swivel && !start) + (cornerLift ? 2 : 0) + unwind + (unwindLift ? 2 : 0);
    const float curveZ = start ? down : e.z;
    PlannerQueueResult out = PQ_OK;
    if (ringOwner != RING_JOB) {
        out = PQ_BAD_STATE;                         // a jog owns the ring
    } else if (plannerRing.count() > planner::Planner::kSize - blocks) {
        out = PQ_FULL;                              // a record's blocks go in together
    } else if (!start && (dx != 0 || dy != 0)) {
        // An idle reset restarts the ring at machinePos, which rounds p3 to a
        // step; a contour continuing after a drain is within one step of it.
        const float step = 1 / fminf(fabsf(plannerSpm[0]), fabsf(plannerSpm[1]));
        if (reset && fabsf(dx) <= step && fabsf(dy) <= step) b.p[0] = e.xy();
        else out = PQ_BAD_CURVE;
    }
    // Every block checked before any goes in.
    planner::PathCap travelCap, curveCap;
    if (out == PQ_OK && start) {
        if (!zMoveFits(e.xy(), e.z, up)) out = PQ_SOFT_LIMIT;
        else out = meshLine(p0, up, limits, travelCap);
        if (out == PQ_OK && !zMoveFits(p0, up, down)) out = PQ_SOFT_LIMIT;
    }
    if (out == PQ_OK && !aFits) out = PQ_SOFT_LIMIT;
    // The travel's A turn swings a blade's axis about a tip that must fit.
    if (out == PQ_OK && start && aTo != aAbs(e) && framesCheckXY(e.x, e.y)) out = PQ_SOFT_LIMIT;
    if (out == PQ_OK && (dutyStop || cornerLift || unwindLift) &&
        (!zMoveFits(p0, e.z, up) || !zMoveFits(p0, up, e.z)))
        out = PQ_SOFT_LIMIT;
    if (out == PQ_OK) out = meshBezier(b, curveZ, limits, curveCap);
    if (out == PQ_OK && end && !zMoveFits(b.p[3], curveZ, up)) out = PQ_SOFT_LIMIT;
    if (out == PQ_OK) {
        jobPower.dutyLiftS = liftS;
        plannerRing.setTag(curLayer, nextRecord, breakDist);
        if (start) {
            pushZ(up, e.z < up ? liftFeed : layerMo.plungeFeed, limits);
            planner::Pos to = withA(plannerRing.end(), aTo);
            to.setXy(p0);
            plannerRing.pushMove(to, travelSpeed(), limits, kDeviation, travelCap);
            if (powered) plannerRing.pushEvent(TOOL_EV_ON);
            pushZ(down, layerMo.plungeFeed, limits);
        }
        if (dutyStop || cornerLift || unwindLift) plannerRing.setLift(breakLift);
        if (dutyStop) {
            pushZ(up, liftFeed, limits);
            plannerRing.pushEvent(TOOL_EV_OFF);
            if (swivel || unwind) pushA(aTo, limits);
            plannerRing.pushEvent(TOOL_EV_ON);
            pushZ(e.z, layerMo.plungeFeed, limits);
        } else if (cornerLift || unwindLift) {
            pushZ(up, liftFeed, limits);
            pushA(aTo, limits);
            pushZ(e.z, layerMo.plungeFeed, limits);
        } else if (swivel && !start) {
            pushA(aTo, limits);
        }
        plannerRing.setLift(0);
        plannerRing.pushBezierFrom(b, from, layerMo.cutFeed, limits, kDeviation, curveCap,
                                   tangential ? &turn : nullptr);
        if (end) {
            // Paused in the lift, the job resumes at the next record.
            plannerRing.setTag(curLayer, nextRecord + 1);
            plannerRing.setLift(LIFT_END);
            pushZ(up, liftFeed, limits);
            if (powered) plannerRing.pushEvent(TOOL_EV_OFF);
        }
    }
    spin_unlock(plannerLock, s);
    if (out != PQ_OK) return out;

    if (start && estOnS >= pw.minOnS) estOnS = 0;   // ZB's rule: this START dwells
    if (start || dutyStop) estOnS = (dutyStop ? 0 : estOnS) + pw.settleS + plungeS;
    // A stretch counts whole at its first record; without its length, by record.
    if (breakDist != 0) stretchCounted = true;
    else if (start) stretchCounted = false;
    if (breakDist != 0) estOnS += cutS(breakDist, limits);
    else if (!stretchCounted) estOnS += (b.length - from) / layerMo.cutFeed;
    if (cornerLift || unwindLift) estOnS += liftS + plungeS;   // the tool stays on through it
    if (end) estOnS += liftS;

    nextRecord++;
    firstRecord = false;
    resumeTrim = 0;
    inContour = !end;
    replanAndCommit();
    return PQ_OK;
}

void plannerDutyLift() {
    planner::AxisLimits limits;
    const bool cfg = machineCfgValid() && fillLimits(limits) &&
                     limits.max_feed[SLOT_Z] > 0 && limits.max_accel[SLOT_Z] > 0;
    const DutyStop d = dutyStop;
    bool failed = false;
    const uint32_t s = spin_lock_blocking(plannerLock);
    // An abort requested meanwhile is Core 1's to finish: it drops the ring.
    if (d.cut && !abortRequested && machineContext == CTX_JOB && ringOwner == RING_JOB) {
        const float up = cfg ? clearZ() : 0;
        failed = !cfg || plannerRing.count() > planner::Planner::kSize - 4 ||
                 !zMoveFits(d.at.xy(), d.at.z, up) || !zMoveFits(d.at.xy(), up, d.at.z);
        if (!failed) {
            // A pause in these resumes at the stop, in the held record.
            plannerRing.setTag(d.layer, d.record, 0, d.s);
            plannerRing.setLift(LIFT_DEADLINE);
            plannerRing.beginFront(d.at);
            pushZ(up, machineCfg().liftFeed, limits);
            plannerRing.pushEvent(TOOL_EV_OFF);
            plannerRing.pushEvent(TOOL_EV_ON);
            pushZ(d.at.z, layerMo.plungeFeed, limits);
            plannerRing.endFront();
            plannerRing.setLift(0);
        }
    }
    spin_unlock(plannerLock, s);
    if (failed) faultSet(F_TOOL);
    replanAndCommit();
    __dmb();
    dutyWait = false;
}

bool plannerQueueLift(uint8_t why) {
    planner::AxisLimits limits;
    if (!machineCfgValid() || !fillLimits(limits) ||
        !(limits.max_feed[SLOT_Z] > 0 && limits.max_accel[SLOT_Z] > 0)) return false;
    const float up = clearZ();
    const float feed = machineCfg().liftFeed;

    const uint32_t s = spin_lock_blocking(plannerLock);
    const planner::Pos e = plannerRing.end();
    const bool rest = machineContext == CTX_JOB && ringOwner == RING_JOB && !plannerActive &&
                      plannerRing.count() == 0;
    const bool lift = rest && e.z < up && zMoveFits(e.xy(), e.z, up);
    // The tool lifts on, then goes off; off alone where Z cannot lift.
    const bool off = rest && toolOn;
    // A tool switched off where Z cannot lift is not a lift.
    if (lift) plannerRing.setLift(why == JOB_ABORTING ? LIFT_ABORT : LIFT_PAUSE);
    if (lift) pushZ(up, feed, limits);
    if (off) plannerRing.pushEvent(TOOL_EV_OFF);
    plannerRing.setLift(0);
    if (lift || off) jobReason = why;
    spin_unlock(plannerLock, s);
    if (lift || off) replanAndCommit();
    return lift || off;
}

bool plannerLandPaused() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool ok = machineContext == CTX_JOB && jobReason == JOB_HOLDING &&
                    !plannerActive && plannerRing.count() == 0;
    if (ok) {
        pauseRequested = false;
        __dmb();
        jobReason = JOB_PAUSED;
    }
    spin_unlock(plannerLock, s);
    return ok;
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
