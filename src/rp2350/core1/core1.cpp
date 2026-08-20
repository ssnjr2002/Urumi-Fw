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
#include "../shared.h"
#include "bus/RS485Bus.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

RS485Bus rs485;

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

// ─── Local Helpers ────────────────────────────────────────────────────────────

static void sendPacket(uint8_t* packet, uint8_t len) {
    packet[len - 1] = crc8(packet, len - 1);
    for (int i = 0; i < len; i++) rs485.writeCommand(packet[i]);
}

static uint8_t receivePacket(uint8_t expectedNode, uint8_t expectedCmd,
                              uint8_t* outPayload, uint32_t timeoutMs) {
    uint32_t start = millis();
    uint8_t  rxBuf[32];
    int      rxIdx = 0;

    while (millis() - start < timeoutMs) {
        if (!rs485.available()) continue;

        uint16_t rcv = rs485.read();
        if (!(rcv & (1 << 8))) { rxIdx = 0; continue; } // stream byte — discard

        rxBuf[rxIdx++] = (uint8_t)(rcv & 0xFF);
        if (rxIdx < 4) continue;

        uint8_t payloadLen      = rxBuf[2];
        int     expectedTotalLen = 3 + payloadLen + 1;
        if (rxIdx < expectedTotalLen) continue;

        bool ok = (rxBuf[0] == expectedNode) &&
                  (rxBuf[1] == expectedCmd)  &&
                  (rxBuf[rxIdx - 1] == crc8(rxBuf, rxIdx - 1));

        if (ok) {
            if (outPayload && payloadLen > 0) memcpy(outPayload, &rxBuf[3], payloadLen);
            return payloadLen;
        }
        rxIdx = 0; // bad packet — restart
    }
    return 0xFF; // timeout
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

// ─── Debug Step Emitter ───────────────────────────────────────────────────────
// Emits `count` raw stream bytes into one stream SLOT at debugStepSps steps/sec.
// Bypasses the MicroSegment path entirely — used to verify the Pico→node stream
// path in isolation. Only the node ENGAGE-bound to this slot moves, and it must
// also be enabled (CMD_ENABLE). Core 0 resolves the target bus node → slot (via
// the axis map) before pushing the FIFO word, so here the arg is already a slot.

static void emitDebugSteps(uint32_t req, int32_t signedCount) {
    uint8_t  slot = (req >> 16) & 0xFF;
    uint32_t sps  =  req & 0xFFFF;                    // rides the request, see shared.h
    bool     neg  = (signedCount < 0);
    uint32_t count = (uint32_t)(neg ? -(int64_t)signedCount : (int64_t)signedCount);

    if (slot >= 4) return;                            // 4 stream slots (X/Y/Z/A)

    uint8_t bit = slot * 2;
    uint8_t streamByte = (1 << bit);                  // step bit
    if (!neg) streamByte |= (1 << (bit + 1));         // dir bit (positive = CW)

    if (sps == 0) sps = STEP_DEBUG_SPS;
    uint32_t interval = F_CPU / sps;

    while (!rs485.txEmpty());
    rs485.flushRX();
    rs485.writeStream(0);  // NOP to reset slave parsers

    uint32_t t0 = rp2040.getCycleCount();
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (machineState == STATE_ESTOP) break;
        while ((rp2040.getCycleCount() - t0) < interval) {
            if (machineState == STATE_ESTOP) break;
        }
        t0 += interval;
        rs485.writeStream(streamByte);
        emitted++;
    }

    // Debug stepping is TRACKED, not untracked: the target node is engaged (Core 0
    // refuses otherwise), so its RX ISR counts every one of these bytes into its
    // own absolutePosition exactly as it would during a job. The node-frame datum
    // therefore stays valid — nodePos - nodeOrigin still resolves correctly — and
    // clearing axes_homed here would throw away a datum that is still sound.
    //
    // Count what was actually emitted, not what was asked for: an estop can cut
    // the burst short (and invalidates the datum by its own path anyway).
    machinePos[slot] += neg ? -(int32_t)emitted : (int32_t)emitted;
}

// ─── Status-reply relay ───────────────────────────────────────────────────────
// Send `pkt` and forward a variable-length status payload to Core 0. The node
// answers [type][flags][type-specific tail…], so the length is not known here.
// Push a status word carrying the payload length (0 = timeout / too short), then
// the payload packed 4 bytes per word, MSB first. Core 0 unpacks and decodes by
// type.
//
// Shared by CMD_NODE_STATUS and CMD_ENGAGE because they now answer with the same
// bytes — the node has one serializer (buildNodeStatus), so the relay is one
// function rather than a shape per command. CMD_GET_POS used to be a third,
// special-cased variant that pushed a bare int32 in a second word; that oddity is
// gone, and Core 0 reads positions out of this payload instead.
static void relayStatusReply(uint8_t node, uint8_t cmd,
                             uint8_t* pkt, uint8_t pktLen) {
    sendPacket(pkt, pktLen);
    uint8_t buf[32];                                  // max node reply payload
    uint8_t rxLen = receivePacket(node, cmd, buf, RESPONSE_TIMEOUT_MS);
    bool ok = (rxLen != 0xFF && rxLen >= 2);          // at least [type][flags]
    multicore_fifo_push_blocking(((uint32_t)cmd << 24) | ((uint32_t)node << 16) |
                                 (ok ? rxLen : 0u));
    if (!ok) return;
    for (uint8_t i = 0; i < rxLen; i += 4) {
        uint32_t w = 0;
        for (uint8_t j = 0; j < 4 && (i + j) < rxLen; j++)
            w |= (uint32_t)buf[i + j] << (24 - j * 8);
        multicore_fifo_push_blocking(w);
    }
}

// ─── Whole-bus safe-off ──────────────────────────────────────────────────────
// CMD_DISABLE to every address, replies consumed and discarded.
//
// The sweep covers the WHOLE bus, not the axis map, because CMD_DISABLE is the
// generic "park yourself" hook and each node type implements it as its own safe
// state: a stepper de-energises, a vacuum node stops the pump, a knife node
// kills the oscillator and the blower. Peripherals hold no motion slot, so
// slotNode[] cannot reach them — and they are precisely the ones that must not
// keep running after an estop, since the blade is still in the material.
//
// Costs up to RESPONSE_TIMEOUT_MS per ABSENT address (a present node answers in
// microseconds), so a sparsely-populated bus makes this the slowest thing on
// the estop path. That is acceptable: motion has already stopped by flushing
// the queue, and this is the cleanup behind it.
// One unacknowledged frame that starts every node parking at once, ~50us on the
// wire. The serial sweep below still runs and still gates the ALARM transition —
// this only changes WHEN each node begins stopping, from "at its turn in an
// 8-address walk, most of it spent timing out on empty addresses" to "now".
//
// Nodes dispatch from cmdQueue in loop(), not in the RX ISR, so the honest claim
// is that every node starts within one loop() of every other — not that the stop
// is instantaneous. The serial sweep pays that same per-node latency anyway, plus
// the round trips.
// Send one broadcast command. Refuses anything outside the allowlist, so the
// deny-by-default rule is enforced at both ends rather than trusted at one: the
// node would drop it anyway, but a silent no-op on the wire is a worse bug to
// find than a call that never compiles into an effect.
static bool sendBroadcast(uint8_t cmd) {
    if (!cmdAllowsBroadcast(cmd)) return false;
    uint8_t pkt[4] = {BUS_ADDR_BROADCAST, cmd, 0, 0};
    sendPacket(pkt, 4);
    // No receivePacket: a broadcast is answered by nobody (see common.h).
    return true;
}

static void busDisableAll() {
    for (uint8_t node = 1; node <= BUS_ADDR_MAX; node++) {
        uint8_t pkt[4] = {node, CMD_DISABLE, 0, 0};
        sendPacket(pkt, 4);
        receivePacket(node, CMD_DISABLE, nullptr, RESPONSE_TIMEOUT_MS);  // consume
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

    // 3. Handle text commands from Core 0 (ping / enable / disable / getpos)
    if (multicore_fifo_rvalid()) {
        uint32_t req  = multicore_fifo_pop_blocking();

        // Debug step — two words: the tagged word, then the int32 count.
        if ((req >> 24) == FIFO_STEP_DEBUG) {
            emitDebugSteps(req, (int32_t)multicore_fifo_pop_blocking());
            return;
        }

        // Home — four words (shared.h). Core 1 only marshals: it does not know
        // seek from retract, does not interpret the reply, and runs no
        // supervision. The node decides the mode from its own limit pin, and
        // Core 0 polls for the outcome.
        if ((req >> 24) == FIFO_HOME) {
            const uint32_t w1 = multicore_fifo_pop_blocking();
            const uint32_t w2 = multicore_fifo_pop_blocking();
            const uint32_t w3 = multicore_fifo_pop_blocking();

            const uint8_t  hNode = req & 0xFF;
            uint8_t pkt[3 + CMD_HOME_PAYLOAD_LEN + 1];
            pkt[0]  = hNode;
            pkt[1]  = CMD_HOME;
            pkt[2]  = CMD_HOME_PAYLOAD_LEN;
            pkt[3]  = (req >> 16) & 0x01;          // dir
            pkt[4]  = (w1 >> 24) & 0xFF;           // start_interval_us
            pkt[5]  = (w1 >> 16) & 0xFF;
            pkt[6]  = (w1 >>  8) & 0xFF;           // floor_interval_us
            pkt[7]  =  w1        & 0xFF;
            pkt[8]  = (w2 >> 24) & 0xFF;           // ramp_steps
            pkt[9]  = (w2 >> 16) & 0xFF;
            pkt[10] = (w3 >> 24) & 0xFF;           // max_steps
            pkt[11] = (w3 >> 16) & 0xFF;
            pkt[12] = (w3 >>  8) & 0xFF;
            pkt[13] =  w3        & 0xFF;

            while (!rs485.txEmpty());
            rs485.flushRX();
            rs485.writeStream(0);   // NOP stream byte to reset slave parsers
            relayStatusReply(hNode, CMD_HOME, pkt, sizeof pkt);
            return;
        }

        uint8_t  cmd     = (req >> 8)  & 0xFF;
        uint8_t  node    =  req        & 0xFF;
        uint8_t  payload = (req >> 16) & 0xFF;  // bits[23:16], 0 for payloadless cmds

        while (!rs485.txEmpty());
        rs485.flushRX();
        rs485.writeStream(0); // NOP stream byte to reset slave parsers

        // Broadcast: one frame to every node, answered by none. Core 0 still
        // blocks on a reply word, so push one — but it means "the frame went out",
        // NOT "the nodes acted". Nothing on this path can know the latter.
        //
        // TODO(verify): follow with a per-node CMD_NODE_STATUS poll and check
        // NODE_FLAG_ENABLED to turn this into a real result. What to do about a
        // node that answers with the wrong state — retry, fault mask, alarm — is
        // still undecided, so today the broadcast is fire-and-forget and the
        // estop path keeps its serial CMD_DISABLE sweep as the actual guarantee.
        if (node == BUS_ADDR_BROADCAST) {
            bool sent = sendBroadcast(cmd);
            multicore_fifo_push_blocking(((uint32_t)cmd << 24) |
                                         ((uint32_t)node << 16) | (sent ? 1u : 0u));
            return;
        }

        switch (cmd) {
            case CMD_PING: {
                uint8_t pkt[4] = {node, CMD_PING, 0, 0};
                sendPacket(pkt, 4);
                uint8_t rxLen = receivePacket(node, CMD_PONG, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_PING << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            case CMD_NODE_STATUS: {
                uint8_t pkt[4] = {node, CMD_NODE_STATUS, 0, 0};
                relayStatusReply(node, CMD_NODE_STATUS, pkt, 4);
                break;
            }
            // Arm the node's datum witness and read back the counter it refers
            // to — one transaction, so Core 0's origin and the node's witness
            // describe the same instant.
            case CMD_DATUM_SET: {
                uint8_t pkt[4] = {node, CMD_DATUM_SET, 0, 0};
                relayStatusReply(node, CMD_DATUM_SET, pkt, 4);
                break;
            }
            case CMD_ENABLE: {
                uint8_t pkt[4] = {node, CMD_ENABLE, 0, 0};
                sendPacket(pkt, 4);
                uint8_t rxLen = receivePacket(node, CMD_ENABLE, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_ENABLE << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            case CMD_DISABLE: {
                uint8_t pkt[4] = {node, CMD_DISABLE, 0, 0};
                sendPacket(pkt, 4);
                uint8_t rxLen = receivePacket(node, CMD_DISABLE, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_DISABLE << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            // Stepper ENGAGE — bind/unbind the node's stream slot. Dumb relay:
            // Core 0 owns the axis map; here we just carry one
            // [node][CMD_ENGAGE][1][slot] packet (payload = slot, 0..3 or 0xFF =
            // disengage). See docs/engage_and_axis_map.md §5.
            //
            // The ACK is a full status payload, not a bare ok, so Core 0 learns
            // position AND enabled state on the same round trip that does the
            // bind — no second transaction, and no window between them for the
            // node to reboot in (docs/node_session_and_datum.md §2).
            case CMD_ENGAGE: {
                uint8_t pkt[5] = {node, CMD_ENGAGE, 1, payload, 0};
                relayStatusReply(node, CMD_ENGAGE, pkt, 5);
                break;
            }
            // Vacuum-node commands. payload carries the args (packed by Core 0):
            //   servo — high nibble = channel idx (0=all, 1..6), low bit = on/off
            //   ssr   — low bit = state
            // Host talks on/off; here on expands to SERVO_ON_ANGLE (off = 0°) so
            // the node sees a raw angle on the wire.
            case CMD_SERVO_SET: {
                uint8_t idx   = (payload >> 4) & 0x0F;
                uint8_t angle = (payload & 0x01) ? SERVO_ON_ANGLE : 0;
                uint8_t pkt[6] = {node, CMD_SERVO_SET, 2, idx, angle, 0};
                sendPacket(pkt, 6);
                uint8_t rxLen = receivePacket(node, CMD_SERVO_SET, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_SERVO_SET << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            case CMD_SSR_SET: {
                uint8_t state = payload & 0x01;
                uint8_t pkt[5] = {node, CMD_SSR_SET, 1, state, 0};
                sendPacket(pkt, 5);
                uint8_t rxLen = receivePacket(node, CMD_SSR_SET, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_SSR_SET << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            case CMD_SWITCH_GET: {
                // Query — replies with a 1-byte level. Two FIFO words back
                // (status, then level) like GET_POS.
                uint8_t pkt[4] = {node, CMD_SWITCH_GET, 0, 0};
                sendPacket(pkt, 4);
                uint8_t lvl[1];
                uint8_t rxLen = receivePacket(node, CMD_SWITCH_GET, lvl, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_SWITCH_GET << 24) | (node << 16) | (rxLen == 1 ? 1u : 0u));
                if (rxLen == 1) multicore_fifo_push_blocking((uint32_t)lvl[0]);
                break;
            }
            // Knife-node commands. payload carries the arg (packed by Core 0):
            //   osc    — low bit = oscillator state
            //   blower — duty 0..100 %
            case CMD_KNIFE_OSC: {
                uint8_t state = payload & 0x01;
                uint8_t pkt[5] = {node, CMD_KNIFE_OSC, 1, state, 0};
                sendPacket(pkt, 5);
                uint8_t rxLen = receivePacket(node, CMD_KNIFE_OSC, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_KNIFE_OSC << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            case CMD_KNIFE_BLOWER: {
                uint8_t duty = payload;  // 0..100, validated by Core 0
                uint8_t pkt[5] = {node, CMD_KNIFE_BLOWER, 1, duty, 0};
                sendPacket(pkt, 5);
                uint8_t rxLen = receivePacket(node, CMD_KNIFE_BLOWER, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_KNIFE_BLOWER << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            // Stepper laser gate — only the -DNODE_HAS_LASER node ACKs; others NAK
            // (rxLen 0xFF → Core 0 prints "timeout").
            case CMD_LASER: {
                uint8_t state = payload & 0x01;
                uint8_t pkt[5] = {node, CMD_LASER, 1, state, 0};
                sendPacket(pkt, 5);
                uint8_t rxLen = receivePacket(node, CMD_LASER, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_LASER << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
        }
    } else {
        delayMicroseconds(10);
    }
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
