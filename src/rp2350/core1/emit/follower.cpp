// follower.cpp — planner motion on Core 1.
//
// A 20 µs slot loop. Every slot, the X and Y position followers (Q32.32 step
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
// X is slot 0 and Y slot 1, as machinePos.

#include <Arduino.h>
#include "../../ipc/shared_state.h"
#include "../bus/packet.h"   // for the shared `rs485` instance
#include "../cycles.h"
#include "emit.h"
#include "hardware/sync.h"

using planner::Executor;
using planner::Vec2;

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

// Enter planner motion, from IDLE with lines queued or from PAUSED on a resume.
// False if there is nothing to run.
static bool __time_critical_func(enter)() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    bool ok = false;
    if (machineState == STATE_PAUSED) {
        if (resumeRequested && plannerActive) {
            resumeRequested = false;
            plannerExec.resume(plannerRing);   // replans; the machine is at rest
            ok = true;
        }
    } else if (machineState == STATE_IDLE && plannerRing.count() > 0) {
        plannerActive = true;
        ok = true;
    }
    // RUNNING under the lock: `cancel` checks for PAUSED under it.
    if (ok) {
        jobActive = false;
        runningReason = RUNNING_PLANNER;
        __dmb();
        machineState = STATE_RUNNING;
    }
    spin_unlock(plannerLock, s);
    return ok;
}

// Leave planner motion for IDLE, unless lines arrived meanwhile.
static bool __time_critical_func(leaveIdle)() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool empty = plannerRing.count() == 0;
    if (empty) plannerActive = false;
    spin_unlock(plannerLock, s);
    if (!empty) return false;

    pauseRequested = abortRequested = false;
    jobActive = false;
    runningReason = RUNNING_JOB;
    __dmb();
    machineState = STATE_IDLE;
    return true;
}

static void __time_critical_func(leavePaused)() {
    resumePos[0] = machinePos[0]; resumePos[1] = machinePos[1];
    resumePos[2] = machinePos[2]; resumePos[3] = machinePos[3];
    pauseRequested = false;
    jobActive = true;
    runningReason = RUNNING_JOB;
    __dmb();
    machineState = STATE_PAUSED;
}

void __time_critical_func(processPlanner)() {
    if (!enter()) return;

    // Followers start where Core 1 last emitted. The half step makes a step fall
    // where the target crosses a half, so position rounds to the nearest step.
    int64_t accum[2], inc[2] = {0, 0};
    for (int i = 0; i < 2; i++) accum[i] = (int64_t)machinePos[i] * kOne + kHalf;

    bool holding = false, aborting = false;
    uint32_t late = plannerExec.lateAdoptions();
    int slot = 0;
    float pending = 0;
    uint32_t t0 = cycleCount();

    for (;;) {
        uint8_t streamByte = 0;
        bool stepped[2] = {false, false};
        for (int i = 0; i < 2; i++) {
            if (inc[i] == 0) continue;
            const int32_t before = stepOf(accum[i]);
            accum[i] += inc[i];
            if (inc[i] > 0) streamByte |= 1u << (2 * i + 1);
            if (stepOf(accum[i]) != before) {
                streamByte |= 1u << (2 * i);
                stepped[i] = true;
            }
        }

        while ((cycleCount() - t0) < kSlotCycles) {
            if (machineState == STATE_ESTOP) return;
        }
        t0 += kSlotCycles;
        rs485.writeStream(streamByte);
        for (int i = 0; i < 2; i++)
            if (stepped[i]) machinePos[i] = stepOf(accum[i]);

        if (machineState == STATE_ESTOP) return;

        pending += kSlotS;
        if (++slot < kSlotsPerTick) continue;

        // One found after its switch time is refused and the executor holds:
        // land PAUSED as for `pause`, resumable from rest.
        plannerExec.adopt(plannerRing);
        if (plannerExec.lateAdoptions() != late) {
            late = plannerExec.lateAdoptions();
            pauseRequested = true;
        }

        if (abortRequested && !aborting) {
            plannerExec.abort();
            aborting = holding = true;
            runningReason = RUNNING_ABORT_DECEL;
        } else if (pauseRequested && !holding) {
            plannerExec.hold();
            holding = true;
            runningReason = RUNNING_ABORT_DECEL;
        }

        Vec2 pos;
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

        // At rest only once the followers have caught up with the executor.
        const bool settled = inc[0] == 0 && inc[1] == 0 && plannerExec.speed() == 0;
        if (!settled) continue;
        const Executor::State st = plannerExec.state();
        if (aborting) {
            // A finished abort has emptied the ring and reset the executor.
            if (st == Executor::State::Running && leaveIdle()) return;
        } else if (holding) {
            if (st == Executor::State::Held) { leavePaused(); return; }
        } else if (plannerRing.count() == 0 && leaveIdle()) {
            return;
        }
    }
}
