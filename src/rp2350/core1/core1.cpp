// Core 1: the real-time service loop.
//
// What is left here after the emitters moved to emit/ is scheduling: the order
// in which Core 1 attends to estop, queued motion, and channel-1 requests, and
// the park/init/run/disable cycle that brackets it. The work itself lives in
// emit/microsegment.cpp, emit/debug_step.cpp, and rpc_server.cpp.
//
// The ordering in processBus() is the policy, not an implementation detail --
// see the comment at step 3.

#include <Arduino.h>
#include "../ipc/shared_state.h"
#include "../ipc/core1_rpc.h"
#include "../board.h"
#include "emit/emit.h"
#include "bus/RS485Bus.h"
#include "bus/packet.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

// RAM-resident park for a Core 0 flash op. MUST NOT execute from flash: the
// erase/program stalls XIP, so a flash-resident spin here would fault. Ack by
// setting core1_parked_for_flash, spin until Core 0 clears flash_op_requested,
// then release. Only entered in IDLE/ALARM (Core 0 gates config writes there),
// so no motion is ever interrupted. See ipc/shared_state.h for the handshake.
static void __not_in_flash_func(core1FlashPark)() {
    core1_parked_for_flash = true;
    __dmb();
    while (flash_op_requested) tight_loop_contents();
    core1_parked_for_flash = false;
    __dmb();
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
