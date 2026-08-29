// Core 1: Real-time RS485 step emitter
//
// Consumes MicroSegments produced by the host PC (via Core 0 ingest).
// All kinematics are pre-computed on the PC — Core 1 just packs stream bytes
// and waits the prescribed interval between steps.
//
// Stream byte format (9th bit = 0):
//   Bits 1-0 : Node 1 (dir | step)
//   Bits 3-2 : Node 2
//   Bits 5-4 : Node 3
//   Bits 7-6 : Node 4

#include <Arduino.h>
#include "../ipc/shared_state.h"
#include "../ipc/core1_rpc.h"
#include "../board.h"
#include "motion_limits.h"
#include "emit/emit.h"
#include "bus/RS485Bus.h"
#include "bus/packet.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

// RAM-resident park for a Core 0 flash op. MUST NOT execute from flash: the
// erase/program stalls XIP, so a flash-resident spin here would fault. Ack by
// setting core1_parked_for_flash, spin until Core 0 clears flash_op_requested,
// then release. Only entered in IDLE/ALARM (Core 0 gates config writes there),
// so no motion is ever interrupted. See shared.h flash-quiesce handshake.
static void __not_in_flash_func(core1FlashPark)() {
    core1_parked_for_flash = true;
    __dmb();
    while (flash_op_requested) tight_loop_contents();
    core1_parked_for_flash = false;
    __dmb();
}

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

static void __time_critical_func(processMicroSegments)() {
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


void processBus() {
    // 1. Estop — flush the queue, invalidate position, settle into ALARM.
    //    ALARM is sticky until Core 0 issues setorigin / unalarm.
    if (machineState == STATE_ESTOP) {
        mBufHead = mBufTail;            // flush the queue
        queuedUsOut  = queuedUsIn;     // flushed segments are never retired (§4.6)
        pauseRequested = abortRequested = false;  // estop outranks a pending ramp
        runningReason  = RUNNING_JOB;
        jobActive    = false;          // any suspended job is unrecoverable
        alarmReason  = ALARM_ESTOP;    // set reason before the ALARM transition

        // Actually de-energise, rather than only claiming to. axes_enabled is
        // host-side bookkeeping; clearing it alone left every node's EN pin
        // asserted and the oscillator running, while STATUS_RSP reported the
        // machine disarmed. The sweep runs BEFORE the ALARM transition so the
        // invariant the host can rely on is: once you observe ALARM, everything
        // on the bus is already parked.
        //
        // Core 0 clears axes_enabled, keyed on exactly that transition (ALARM +
        // ALARM_ESTOP), so the invariant is unchanged while the mask keeps a
        // single writer. Clearing it here raced Core 0's read-modify-writes.
        //
        // Broadcast first so the stop is parallel, then confirm it serially. Both
        // run before the ALARM transition, so the invariant above is unchanged:
        // the sweep, not the broadcast, is what makes it true. The broadcast is
        // one extra frame that buys every node an earlier start; if it is missed,
        // the sweep behind it still parks that node before ALARM is published.
        sendBroadcast(CMD_DISABLE);
        busDisableAll();

        __dmb();
        machineState = STATE_ALARM;
        return;
    }

    // 2. Emit any queued MicroSegments
    if (mBufHead != mBufTail) processMicroSegments();

    // An abort with nothing to stop still has to be consumed. The ramp lives
    // inside the emitter, so a request arriving while the ring is empty would
    // otherwise never be cleared — and the ingest barrier keyed off it would
    // NACK every packet forever. Nothing to decelerate: just discard the flag.
    if (abortRequested && mBufHead == mBufTail) {
        mBufHead = mBufTail;
        queuedUsOut    = queuedUsIn;
        abortRequested = false;
        jobActive      = false;
        runningReason  = RUNNING_JOB;
        __dmb();
        if (machineState == STATE_RUNNING || machineState == STATE_PAUSED)
            machineState = STATE_IDLE;
    }

    // 3. Service one channel-1 request (ipc/core1_rpc.h).
    //
    // Strictly after the queue drain above: a bus transaction takes up to
    // RESPONSE_TIMEOUT_MS, which is many step intervals, so serving one
    // mid-segment would stretch a step and mark the cut. The ordering IS the
    // policy -- it is why a relay issued mid-stream waits out the queue.
    if (!rpcServerPoll()) delayMicroseconds(10);
}

// ─── Core 1 Setup & Loop ──────────────────────────────────────────────────────

// setup1() never returns: after the framework's one-time hardware init it
// runs the park -> init -> run -> disable -> park cycle directly, forever.
// This is deliberately not loop1() — nothing here needs to yield back to the
// framework each pass, so pretending it's a per-frame callback was misleading.
// loop1() is left as an empty stub only because the framework requires it to
// exist; it will never actually run.
void setup1() {
    rs485.begin(RS485_BAUD, RS485_TX_PIN, RS485_RX_PIN, RS485_EN_PIN);

    for (;;) {
        // ══════════════════════════════════════════════════════════
        // ─── 1: THE PARKING LOT ─────────────────────────────
        // ══════════════════════════════════════════════════════════
        // soft_reset_requested starts true, so this is also the cold-boot
        // gate: Core 1 touches nothing (not even the RS485 hardware beyond
        // rs485.begin() above) until Core 0 has finished its first wipe and
        // cleared the flag.
        core1_parked_for_reset = true;
        while (soft_reset_requested) {
            delay(1);
        }
        core1_parked_for_reset = false;

        // ══════════════════════════════════════════════════════════
        // ─── 2: POST-RESET HARDWARE INIT ────────────────────
        // ══════════════════════════════════════════════════════════
        // Core 0 just gave us the green light.
        // Flush RS485 UART, ensure motor pins are LOW/Disabled, etc.
        while (!rs485.txEmpty());
        rs485.flushRX();
        rs485.writeStream(0); // NOP stream byte to reset slave parsers

        // ══════════════════════════════════════════════════════════
        // ─── 3: MAIN EXECUTION LOOP ─────────────────────────
        // ══════════════════════════════════════════════════════════
        // Run tight timing code as long as Core 0 doesn't request a reset
        while (!soft_reset_requested) {
            if (flash_op_requested) core1FlashPark();   // config write — quiesce XIP
            processBus();
        }

        // ══════════════════════════════════════════════════════════
        // ─── 4: DISABLE NODES ───────────────────────────────
        // ══════════════════════════════════════════════════════════
        // We only reach this line if soft_reset_requested became true!
        // TODO: deliberate if a proper reset handler should be put in
        // the node side
        //
        // Was 1..4 — the axis range — which left peripherals running across a
        // reset. Same sweep as the estop path now, for the same reason.
        busDisableAll();

        // Loop back to the parking lot.
    }
}

void loop1() {}
