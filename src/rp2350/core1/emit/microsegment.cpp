// microsegment.cpp — the MicroSegment step emitter.
//
// Lifted verbatim out of core1.cpp, which had grown to hold three unrelated
// jobs: this emitter, the debug-step burst, and the bus service loop. What is
// left in core1.cpp is the loop that calls them.
//
// Everything here runs in Core 1's step-timing budget (~5000 cycles per step),
// which is why emitMicroSegment and processMicroSegments are both
// __time_critical_func: they must execute from RAM, not from XIP flash.
//
// Stream byte format (9th bit = 0):
//   Bits 1-0 : Node 1 (dir | step)
//   Bits 3-2 : Node 2
//   Bits 5-4 : Node 3
//   Bits 7-6 : Node 4

#include <Arduino.h>
#include "../../ipc/shared_state.h"
#include "../motion_limits.h"
#include "../bus/packet.h"   // for the shared `rs485` instance
#include "emit.h"
#include "hardware/sync.h"

// ─── MicroSegment Step Emitter ────────────────────────────────────────────────
// Pack a MicroSegment's axis deltas into a single RS485 stream byte and wait
// the pre-computed interval before sending. Supports up to 4 axes (nodes 1-4).
//
// Node assignment (matches host PC convention):
//   Node 1 = X,  Node 2 = Y,  Node 3 = Z,  Node 4 = A

// Returns true if the segment was emitted in full; false if estop aborted it
// mid-way (in which case the caller must NOT accumulate its position delta).
// True while some request wants motion brought to rest gracefully. Both flags
// use the same ramp — the difference is only where Core 1 lands afterwards.
static inline bool rampRequested() { return pauseRequested || abortRequested; }

// Soft-limit gate for steps emitted DURING a ramp. Phase 2 of the ramp emits
// beyond the segment's planned delta, so it can cross a bound that the
// per-segment check already cleared.
//
// HARNESS ONLY — always returns true for now. Wiring the real bounds check in
// means deciding where Core 1 reads soft limits from, which is the same
// unresolved config-access question as DECEL_SPS2 below. When it lands, the
// caller's response is already written: setAlarm(ALARM_SOFT_LIMIT).
static inline bool rampStepInBounds(const int32_t /*pos*/[4]) {
    return true;
}

static EmitResult __time_critical_func(emitMicroSegment)(const MicroSegment& ms,
                                                         int32_t out[4]) {
    // A MicroSegment describes a block of steps: the major axis takes
    // max(|dx|,|dy|,|dz|,|da|) steps, minor axes are Bresenham-distributed
    // against it. `interval` is the time (CPU cycles) per major-axis step.
    // The host has already resolved velocity — the Pico just executes.
    //
    // Node bit layout: bit(2n) = step, bit(2n+1) = dir (1 = CW / positive).

    int32_t  delta[4] = { ms.dx, ms.dy, ms.dz, ms.da };
    uint32_t absSteps[4];
    int32_t  sign[4];
    uint8_t  dirBits = 0;
    uint32_t maxSteps = 0;
    int      majorAxis = 0;

    out[0] = out[1] = out[2] = out[3] = 0;

    for (int i = 0; i < 4; i++) {
        absSteps[i] = (delta[i] < 0) ? (uint32_t)(-delta[i]) : (uint32_t)delta[i];
        sign[i]     = (delta[i] < 0) ? -1 : 1;
        if (absSteps[i] > maxSteps) { maxSteps = absSteps[i]; majorAxis = i; }
        if (delta[i] > 0) dirBits |= (1 << (i * 2 + 1)); // positive = CW
    }

    if (maxSteps == 0) return EMIT_DONE; // no motion this segment

    // Bresenham error accumulators — symmetric init for centred distribution
    uint32_t err[4] = { maxSteps / 2, maxSteps / 2, maxSteps / 2, maxSteps / 2 };

    uint32_t interval = ms.interval;   // mutable: the ramp stretches it per step
    bool     ramping  = false;
    float    v        = 0.0f;          // steps/s — only meaningful while ramping
    // Hoisted out of the step loop: the major axis cannot change mid-segment,
    // and this is a branch we do not want inside a ~5000-cycle step budget.
    const float decel2 = 2.0f * decelForAxis(majorAxis);

    uint32_t t0 = rp2040.getCycleCount();
    for (uint32_t s = 0; ; s++) {
        if (machineState == STATE_ESTOP) return EMIT_ESTOP;

        // Enter the ramp once, at whatever velocity we happen to be doing. The
        // interval IS the velocity, so nothing needs to be handed in.
        if (!ramping && rampRequested()) {
            ramping = true;
            v = (float)F_CPU / (float)interval;
        }

        // Termination. Planned run: the segment's own step count. Ramping: rest
        // — which can fall BEFORE or AFTER that count, so once the ramp is live
        // the loop is no longer bounded by maxSteps.
        if (ramping) { if (v <= V_REST_SPS) return EMIT_RAMPED; }
        else         { if (s >= maxSteps)   return EMIT_DONE;   }

        uint8_t streamByte = dirBits;
        for (int i = 0; i < 4; i++) {
            if (absSteps[i] == 0) continue;
            err[i] += absSteps[i];
            if (err[i] >= maxSteps) {
                err[i] -= maxSteps;
                streamByte |= (1 << (i * 2));   // step bit
                out[i]     += sign[i];          // count what is actually EMITTED,
            }                                   // not what was planned
        }

        // Wait the prescribed per-step interval, then emit
        while ((rp2040.getCycleCount() - t0) < interval) {
            if (machineState == STATE_ESTOP) return EMIT_ESTOP;
        }
        t0 += interval; // ms = microsegment, not to be confused with millisecond

        rs485.writeStream(streamByte);

        if (ramping) {
            if (!rampStepInBounds(out)) return EMIT_SOFT_LIMIT;

            // v² ← v² − 2·a·d with d = one major-axis step: constant decel per
            // unit DISTANCE, so the stopping distance is exact regardless of
            // where in the segment the ramp began. All literals need the `f`
            // suffix — a bare 1.0 is a double and would promote the expression
            // onto the (much slower) double path.
            float v2 = v * v - decel2;
            v = (v2 <= V_REST_SPS * V_REST_SPS) ? V_REST_SPS : sqrtf(v2);
            interval = (uint32_t)((float)F_CPU / v);
        }
    }
}

void __time_critical_func(processMicroSegments)() {
#ifdef DEBUG_TIMING
    static uint32_t jobStartUs = 0;
#endif
    // Entry: a job starts from IDLE (runningReason=JOB); a jog burst starts from
    // PAUSED (runningReason=JOG, set by Core 0 at ingest). Remember where to land
    // when the buffer drains — a jog during pause returns to PAUSED (the job is
    // still suspended), a job returns to IDLE.
    uint8_t returnState = (machineState == STATE_PAUSED || jobActive)
                          ? STATE_PAUSED : STATE_IDLE;

    // Enact the RUNNING transition with its reason set together (the cross-core
    // stand-in for setRunning(reason); streamIsJog is Core 0's ingest intent).
    if (machineState == STATE_IDLE) {
        runningReason = streamIsJog ? RUNNING_JOG : RUNNING_JOB;
        machineState  = STATE_RUNNING;
#ifdef DEBUG_TIMING
        jobExpectedUs = 0;   // new job — reset the timing diagnostic
        jobMeasuredUs = 0;
        jobStartUs    = micros();
#endif
    } else if (machineState == STATE_PAUSED) {
        runningReason = RUNNING_JOG;    // only jogs are accepted during pause
        machineState  = STATE_RUNNING;
    }

    while (mBufHead != mBufTail) {
        if (machineState == STATE_ESTOP) return;

        MicroSegment ms = masterBuf[mBufHead];
        uint8_t flags = ms.flags & MSEG_FLAG_WIRE_MASK;   // ignore host hint bits

        // Poison pill — signal estop, leave the flush/ALARM to loop1
        if (flags & MSEG_FLAG_ESTOP) {
            machineState = STATE_ESTOP;
            return;
        }

#ifdef DEBUG_TIMING
        // Timing diagnostic: expected duration from intervals vs wall time by
        // the 1 MHz hardware timer (independent of the cycle-counter domain)
        uint32_t maxSteps = 0;
        {
            int32_t d[4] = { ms.dx, ms.dy, ms.dz, ms.da };
            for (int i = 0; i < 4; i++) {
                uint32_t a = (d[i] < 0) ? (uint32_t)(-d[i]) : (uint32_t)d[i];
                if (a > maxSteps) maxSteps = a;
            }
        }
        uint32_t tStart = micros();
#endif
        // A ramp may start mid-segment, so advertise the sub-mode before emitting
        // rather than after. Reading it back is also how the ramp is observable
        // at all — it is over in tens of milliseconds.
        if (rampRequested() && runningReason != RUNNING_ABORT_DECEL)
            runningReason = RUNNING_ABORT_DECEL;

        int32_t got[4];
        EmitResult r = emitMicroSegment(ms, got);

        // Estop forfeits position BY CHOICE — got[] is accurate here too, the
        // caller just discards it. If estop should ever stop costing a re-home,
        // this is a one-line change, not a rework.
        if (r == EMIT_ESTOP) return;

#ifdef DEBUG_TIMING
        jobMeasuredUs += micros() - tStart;
        jobExpectedUs += (uint32_t)(((uint64_t)ms.interval * maxSteps) / (F_CPU / 1000000));
#endif

        // Exact machine position: count what was EMITTED, not what was planned.
        // A ramp stops mid-segment and may overshoot it, so neither ms.dx… nor
        // zero is right on that path.
        machinePos[0] += got[0];
        machinePos[1] += got[1];
        machinePos[2] += got[2];
        machinePos[3] += got[3];

        if (r == EMIT_RAMPED || r == EMIT_SOFT_LIMIT) {
            // Motion has ended somewhere inside this segment. The rest of the
            // plan is void — it was computed from a velocity the machine no
            // longer has — so discard the whole ring rather than resuming into it.
            bool toPause = pauseRequested;
            mBufHead = mBufTail;
            queuedUsOut = queuedUsIn;              // flushed segments never retire
            pauseRequested = abortRequested = false;
            __dmb();

            if (r == EMIT_SOFT_LIMIT) {            // harness — not raised yet
                // Signal only. Core 0 folds ALARM_SOFT_LIMIT into axes_homed and
                // the node-frame origins (reconcileValidity) — Core 1 no longer
                // writes validity bitmasks it does not own.
                alarmReason  = ALARM_SOFT_LIMIT;
                jobActive    = false;
                __dmb();
                machineState = STATE_ALARM;
            } else if (toPause) {
                resumePos[0] = machinePos[0]; resumePos[1] = machinePos[1];
                resumePos[2] = machinePos[2]; resumePos[3] = machinePos[3];
                jobActive    = true;
                runningReason = RUNNING_JOB;
                __dmb();
                machineState = STATE_PAUSED;
            } else {                               // abort — not resumable
                jobActive    = false;
                runningReason = RUNNING_JOB;
                __dmb();
                machineState = STATE_IDLE;
            }
            return;
        }

        // Retire this segment's contribution to queued time (§4.6). Paired with
        // the enqueue-side add in data_plane.cpp; each counter has one writer.
        queuedUsOut += microSegmentUs(ms.dx, ms.dy, ms.dz, ms.da, ms.interval);

        __dmb();
        mBufHead = (mBufHead + 1) % MASTER_BUF_SIZE;

        // Host-placed tool-change marker. An operator `pause` no longer lands
        // here — it ramps inside the emitter and returns EMIT_RAMPED above —
        // but MSEG_FLAG_PAUSE is a PLANNED boundary the host has already
        // decelerated into, so it still stops cleanly at the segment edge.
        if (flags & MSEG_FLAG_PAUSE) {
            resumePos[0] = machinePos[0]; resumePos[1] = machinePos[1];
            resumePos[2] = machinePos[2]; resumePos[3] = machinePos[3];
            jobActive = true;
            __dmb();
            machineState = STATE_PAUSED;
            return;
        }
    }

    // Queue drained — return to where we belong: PAUSED if this was a jog burst
    // during a pause (the job stays suspended), otherwise IDLE.
    if (machineState == STATE_RUNNING) {
#ifdef DEBUG_TIMING
        jobWallUs = micros() - jobStartUs;
#endif
        machineState = returnState;
        if (returnState == STATE_IDLE) runningReason = RUNNING_JOB;
    }
}
