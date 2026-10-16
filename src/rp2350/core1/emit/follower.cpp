// follower.cpp — planner motion on Core 1.
//
// A 20 µs slot loop. Every slot, the X, Y, Z and A position followers (Q32.32 step
// accumulators, seed §13) advance by their increment and one stream byte goes
// out, steps or not. Every 50 slots the executor ticks 1 ms ahead, and each
// follower's increment is set to reach that position by the next tick; what a
// follower falls short by is corrected on the tick after.
//
// A tick holds plannerLock only when needsRing() says it will claim, release or
// reset, and only with a try-lock: a busy lock defers the tick to the next slot,
// its time carried over, while the followers keep their increments. Before it,
// adopt() takes a raised profile Core 0 staged for the running block; it may end
// the block sooner, so needsRing() must see it.
//
// Everything the slot loop calls runs from RAM: the helpers here are always
// inlined, and lib/planner's tick path is placed by PLANNER_RAM (platformio.ini).
//
// Stream byte: bit 2n = step, bit 2n+1 = dir (1 = positive), for slot n.
// X, Y, Z, A are slots 0 to 3, as machinePos; Z and A are the selected head's.
//
// The planner's Z is flat: the bed mesh latched for this ring (plannerMesh) is
// added to Z's target at the tip, each tick.

#include <Arduino.h>
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_facts.h"
#include "../bus/packet.h"   // the shared `rs485` instance, busNodeCmd
#include "common.h"
#include "../cycles.h"
#include "emit.h"
#include "hardware/sync.h"

using planner::Executor;
using planner::Pos;

static constexpr uint32_t kSlotCycles   = F_CPU / 50000;   // 20 µs
static constexpr int      kSlotsPerTick = 50;               // 1 ms
static constexpr float    kSlotS        = 1.0f / 50000;
static constexpr int64_t  kOne          = 1LL << 32;        // one step, Q32.32
static constexpr int64_t  kHalf         = 1LL << 31;

#define FOLLOWER_INLINE static inline __attribute__((always_inline))

// Steps as Q32.32, without a 64-bit float conversion (a flash-resident libcall).
FOLLOWER_INLINE int64_t toQ32(float steps) {
    int32_t whole = (int32_t)steps;
    if ((float)whole > steps) whole--;
    const float frac = (steps - (float)whole) * 4294967296.0f;
    const uint32_t f = frac >= 4294967040.0f ? 4294967040u : (uint32_t)frac;
    return (int64_t)whole * kOne + f;
}

// Per-slot increment that closes `err` over one tick. Divided in 32 bits at
// 2⁻²⁵ step resolution; the remainder is error again on the next tick. Capped
// below one step per slot, the most one stream byte can carry.
FOLLOWER_INLINE int64_t incrementFor(int64_t err) {
    const int64_t lim = (1LL << 38) - 1;
    if (err > lim) err = lim;
    else if (err < -lim) err = -lim;
    int64_t inc = (int64_t)((int32_t)(err >> 7) / kSlotsPerTick) * 128;
    if (inc >= kOne) inc = kOne - 1;
    else if (inc <= -kOne) inc = -(kOne - 1);
    return inc;
}

FOLLOWER_INLINE int32_t stepOf(int64_t q) { return (int32_t)(q >> 32); }

static void __time_critical_func(jobAt)(uint8_t why) {
    jobReason = why;
    __dmb();
}

// A job at rest in BUFFERING: start it once bufferQueueS is queued, the ring is
// full, or no record has arrived for bufferIdleMs. Under the lock.
static bool __time_critical_func(bufferDone)() {
    static int seenCount = -1;
    static uint32_t seenMs = 0;
    const int n = plannerRing.count();
    const uint32_t now = millis();
    if (n != seenCount) { seenCount = n; seenMs = now; }
    if (n == 0) return false;
    return plannerRing.full() || now - seenMs >= bufferIdleMs ||
           plannerRing.queuedTime() >= bufferQueueS;
}

// A pause at rest: keep where the job stopped (jobResume), drop the ring and
// stay in JOB.HOLDING; Core 0 lifts and lands PAUSED (FACT_HELD). The host
// resends from there. Under the lock.
static void __time_critical_func(landHeld)() {
    const planner::Block* b = plannerExec.current();
    float s = b ? b->s0 + plannerExec.along() : 0;
    if (!b && (b = plannerRing.oldest())) s = b->s0;
    jobResume.known = b != nullptr;
    if (b) {
        jobResume.layer  = b->layer;
        jobResume.record = b->record;
        jobResume.s      = b->kind == planner::Block::BEZIER ? s : 0;
    }
    const Pos at = plannerExec.position();
    plannerRing.reset(at);
    plannerExec.reset(at);
    plannerActive = false;
    toolChangePending = false;   // the pause wins; the host resends the header
    jobAt(JOB_HOLDING);
    core1FactPush(FACT_HELD);
}

// Enter planner motion: a jog's ring, a job once it has buffered, or the lift
// Core 0 queues in a held or aborted job. Core 0 opens a job in BUFFERING, and
// resumes a paused one there; a pause at rest is taken here. All under the
// lock: `cancel` ends a job at rest under it. False if there is nothing to run.
static bool __time_critical_func(enter)() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    bool ok = false;
    if (estopRequested) {
        // The estop sweep drops the ring.
    } else if (ringOwner != RING_JOB) {
        if (!plannerActive && plannerRing.count() > 0) {
            plannerActive = true;
            ok = true;
        }
    } else if (machineContext == CTX_JOB && !abortRequested) {
        const uint8_t why = jobReason;
        if ((why == JOB_HOLDING || why == JOB_ABORTING) && !plannerActive &&
            plannerRing.count() > 0) {
            plannerActive = true;
            ok = true;
        } else if (why == JOB_BUFFERING && pauseRequested) {
            landHeld();
        } else if (why == JOB_BUFFERING && bufferDone()) {
            plannerActive = true;
            jobAt(JOB_RUNNING);
            ok = true;
        }
    }
    spin_unlock(plannerLock, s);
    return ok;
}

// Leave planner motion with the ring drained (a jog's end, an abort, or a
// lift), unless lines arrived meanwhile. A job's context closes on Core 0, once
// abortRequested is clear.
static bool __time_critical_func(leaveDrained)() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool empty = plannerRing.count() == 0;
    if (empty) plannerActive = false;
    spin_unlock(plannerLock, s);
    if (!empty) return false;

    pauseRequested = abortRequested = false;
    __dmb();
    core1FactPush(FACT_RING_DRAINED);
    return true;
}

// A job's ring ran dry: wait in BUFFERING, or TOOL_CHANGE for a layer header
// that wants one, unless records arrived meanwhile.
static bool __time_critical_func(leaveDry)() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool empty = plannerRing.count() == 0;
    if (empty) {
        plannerActive = false;
        jobAt(toolChangePending ? JOB_TOOL_CHANGE : JOB_BUFFERING);
    }
    spin_unlock(plannerLock, s);
    return empty;
}

static void __time_critical_func(leaveHeld)() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    landHeld();
    spin_unlock(plannerLock, s);
}

// ─── Tool events ──────────────────────────────────────────────────────────────
// The duty counters (docs/plans/job-z.md, Duty), kept across rings so that a
// pause counts as off time: on-time since the last reset, and when the tool
// last went on or off. `offKnown` false is an off time long enough to reset.

static uint32_t dutyOnMs    = 0;   // closed on-spans since the reset
static uint32_t toolOnAtMs  = 0;
static uint32_t toolOffAtMs = 0;
static bool     offKnown    = false;
static uint32_t dutySeen    = 0;

static void dutyCatchUp(uint32_t now) {
    if (dutyEpoch == dutySeen) return;
    dutySeen = dutyEpoch;
    dutyOnMs = 0;
    toolOnAtMs = now;
    offKnown = false;
}

// Switch the job's tool, three tries.
static bool toolSwitch(bool on) {
    for (int i = 0; i < 3; i++)
        if (busNodeCmd(jobPower.node, jobPower.cmd, on ? 1 : 0)) return true;
    return false;
}

static void toolWentOff(uint32_t now) {
    if (toolOn) dutyOnMs += now - toolOnAtMs;
    toolOn = false;
    toolOffAtMs = now;
    offKnown = true;
}

// Where a TOOL_EV_ON is; it runs over several ticks.
enum EventPhase : uint8_t { EV_START, EV_DWELL, EV_SWITCH, EV_SETTLE, EV_FAILED };

static bool reached(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

// Run the event the executor waits at, at rest. True if it used the bus.
static bool runEvent(const planner::Block& ev, EventPhase& phase, uint32_t& until) {
    const uint32_t now = millis();
    dutyCatchUp(now);
    if (ev.event == TOOL_EV_OFF) {
        // A tool that will not switch off is made safe instead.
        if (!toolSwitch(false)) {
            busNodeSafe(jobPower.node, CMD_MAKE_SAFE);
            core1FactPush(FACT_TOOL_FAILED);
        }
        toolWentOff(now);
        plannerExec.eventDone();
        return true;
    }
    switch (phase) {
        case EV_START: {
            const uint32_t dwellMs = (uint32_t)(jobPower.dwellS * 1000.0f);
            const bool offLong = toolOn ? false : !offKnown || now - toolOffAtMs >= dwellMs;
            if (jobPower.hasDuty && !toolOn && !offLong &&
                dutyOnMs >= (uint32_t)(jobPower.minOnS * 1000.0f)) {
                phase = EV_DWELL;
                until = toolOffAtMs + dwellMs;
                jobAt(JOB_DWELL);
                return false;
            }
            if (offLong) dutyOnMs = 0;
            phase = EV_SWITCH;
            return false;
        }
        case EV_DWELL:
            if (!reached(now, until)) return false;
            dutyOnMs = 0;
            jobAt(JOB_RUNNING);
            phase = EV_SWITCH;
            return false;
        case EV_SWITCH:
            if (!toolSwitch(true)) {
                // Waits here, unplunged, for the abort the fault brings.
                phase = EV_FAILED;
                core1FactPush(FACT_TOOL_FAILED);
                return true;
            }
            if (!toolOn) toolOnAtMs = now;
            toolOn = true;
            phase = EV_SETTLE;
            until = now + (uint32_t)(jobPower.settleS * 1000.0f);
            return true;
        case EV_SETTLE:
            if (!reached(now, until)) return false;
            plannerExec.eventDone();
            phase = EV_START;
            return false;
        case EV_FAILED:
            return false;
    }
    return false;
}

// A late block's hold has stopped: replan from rest and buffer again.
static void __time_critical_func(rebuffer)() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    plannerExec.resume(plannerRing);
    spin_unlock(plannerLock, s);
    jobAt(JOB_BUFFERING);
}

void __time_critical_func(processPlanner)() {
    if (!enter()) return;

    // Followers start where Core 1 last emitted. The half step makes a step fall
    // where the target crosses a half, so position rounds to the nearest step.
    int64_t accum[4], inc[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) accum[i] = (int64_t)machinePos[i] * kOne + kHalf;

    // A jog never pauses: a late adoption holds it, then it runs on. Nor does a
    // held or aborted job's lift, which ends with the ring drained.
    const bool jog = ringOwner != RING_JOB;
    const uint8_t why0 = jobReason;
    const bool runsOn = jog || why0 == JOB_HOLDING || why0 == JOB_ABORTING;
    const PlannerMesh mesh = plannerMesh;
    bool holding = false, aborting = false, lateHold = false;
    uint32_t late = plannerExec.lateAdoptions();
    int slot = 0;
    float pending = 0;
    EventPhase evPhase = EV_START;
    uint32_t evUntil = 0;
    uint32_t t0 = cycleCount();

    for (;;) {
        uint8_t streamByte = 0;
        uint8_t stepped = 0;   // bit i: slot i stepped
        for (int i = 0; i < 4; i++) {
            if (inc[i] == 0) continue;
            const int32_t before = stepOf(accum[i]);
            accum[i] += inc[i];
            if (inc[i] > 0) streamByte |= 1u << (2 * i + 1);
            if (stepOf(accum[i]) != before) {
                streamByte |= 1u << (2 * i);
                stepped |= 1u << i;
            }
        }

        while ((cycleCount() - t0) < kSlotCycles) {
            if (estopRequested) return;
        }
        t0 += kSlotCycles;
        rs485.writeStream(streamByte);
        for (int i = 0; i < 4; i++)
            if (stepped & (1u << i)) machinePos[i] = stepOf(accum[i]);

        if (estopRequested) return;

        pending += kSlotS;
        if (++slot < kSlotsPerTick) continue;

        // One found after its switch time is refused and the executor holds:
        // a jog resumes itself once at rest, a job buffers again.
        plannerExec.adopt(plannerRing);
        if (plannerExec.lateAdoptions() != late) {
            late = plannerExec.lateAdoptions();
            if (!holding) {
                plannerExec.hold();
                holding = lateHold = true;
                if (!runsOn) jobReason = JOB_HOLDING;
            }
        }

        if (abortRequested && !aborting) {
            plannerExec.abort();
            aborting = holding = true;
            lateHold = false;
            if (!jog) jobReason = JOB_ABORTING;
        } else if (pauseRequested && !runsOn && !aborting && (!holding || lateHold)) {
            // A pause during a late block's hold lands PAUSED instead.
            if (!holding) plannerExec.hold();
            holding = true;
            lateHold = false;
            jobReason = JOB_HOLDING;
        }

        Pos pos;
        if (!plannerExec.needsRing(pending)) {
            pos = plannerExec.tick(plannerRing, pending);
        } else if (spin_try_lock_unsafe(plannerLock)) {
            pos = plannerExec.tick(plannerRing, pending);
            spin_unlock_unsafe(plannerLock);
        } else {
            continue;   // retry next slot
        }
        slot = 0;
        pending = 0;

        inc[0] = incrementFor(toQ32(pos.x * plannerSpm[0]) + kHalf - accum[0]);
        inc[1] = incrementFor(toQ32(pos.y * plannerSpm[1]) + kHalf - accum[1]);
        float z = pos.z;
        if (mesh.mesh) z += planner::meshAt(*mesh.mesh, pos.x + mesh.tipX, pos.y + mesh.tipY) - mesh.ref;
        inc[2] = incrementFor(toQ32(z * plannerSpm[2]) + kHalf - accum[2]);
        inc[3] = incrementFor(toQ32(pos.a * plannerSpm[3]) + (int64_t)pos.turns * plannerTurnQ32
                              + kHalf - accum[3]);

        // At rest only once the followers have caught up with the executor.
        const bool settled = inc[0] == 0 && inc[1] == 0 && inc[2] == 0 && inc[3] == 0 &&
                             plannerExec.speed() == 0;
        if (!settled) continue;
        const Executor::State st = plannerExec.state();
        if (aborting) {
            // A finished abort has emptied the ring and reset the executor.
            if (st == Executor::State::Running && leaveDrained()) return;
        } else if (lateHold) {
            if (st == Executor::State::Held) {
                if (!runsOn) { rebuffer(); return; }
                const uint32_t s = spin_lock_blocking(plannerLock);
                plannerExec.resume(plannerRing);
                spin_unlock(plannerLock, s);
                holding = lateHold = false;
            }
        } else if (holding) {
            if (st == Executor::State::Held) { leaveHeld(); return; }
        } else if (const planner::Block* ev = plannerExec.event()) {
            // A bus transaction stretches the slot; the stream restarts from now.
            if (runEvent(*ev, evPhase, evUntil)) t0 = cycleCount();
        } else if (plannerRing.count() == 0 && (runsOn ? leaveDrained() : leaveDry())) {
            return;
        }
    }
}
