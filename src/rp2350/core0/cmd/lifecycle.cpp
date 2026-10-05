// lifecycle.cpp — the job lifecycle: stop, unstop, reset, pause, resume,
// cancel, and the data-plane sequence reset.
//
// All but unstop are state writes that Core 1 observes on its next pass, with
// no bus traffic. unstop is the estop's exit, and it has to hear every node.

#include <Arduino.h>
#include "table.h"
#include "gate.h"
#include "../data_plane.h"   // dataPlaneResetSeq (seqreset)
#include "../ops/slot_map.h"
#include "../ops/position.h"
#include "../ops/state.h"
#include "../ops/bus.h"
#include "../config/machine_cfg.h"
#include "../usb_protocol.h" // BUS_ADDR_MAX
#include "../../ipc/shared_state.h"

bool cmdStop(const char*) {
    machineState = STATE_ESTOP;            // Core 1 flushes, clears axes, → ALARM
    Serial.println("ok");
    return true;
}

// Make safe every touched node (energised, or holding a slot) that is not
// excluded; an excluded node's slot stays fenced. Each
// confirmation frees that node's slot and clears its fence. Refused, with the
// unconfirmed ids, until all confirm; then the requests are forgotten and the
// machine settles IDLE (or ALARM_BUS_DEGRADED), unmapped, de-energised,
// un-homed.
bool cmdUnstop(const char*) {
    if (machineState != STATE_ALARM || alarmReason != ALARM_ESTOP) {
        Serial.println("err bad_state"); return true;
    }
    uint16_t unconfirmed = 0;
    for (uint8_t n = 1; n <= BUS_ADDR_MAX; n++) {
        if (!(busTouched() & ~busExcluded() & (1u << n))) continue;
        NodeStatus st;
        if (slotMakeSafe(n, &st) != RPC_OK || (st.flags & NODE_FLAG_ENABLED))
            unconfirmed |= (1u << n);
    }
    if (unconfirmed) {
        Serial.print("err unconfirmed");
        for (uint8_t n = 1; n <= BUS_ADDR_MAX; n++)
            if (unconfirmed & (1u << n)) Serial.printf(" %d", n);
        Serial.println();
        return true;
    }
    slotMapForget();
    axesReqForget();
    resumeOrHold();
    Serial.println("ok");
    return true;
}

bool cmdReset(const char*) {
    if (machineState != STATE_IDLE && machineState != STATE_ALARM) {
        Serial.println("err bad_state");
        return true;
    }
    soft_reset_requested = true;           // Exits loop(), triggers soft reset
    Serial.println("ok");
    return true;
}

bool cmdSeqReset(const char*) {            // data-plane support (see wire doc)
    dataPlaneResetSeq();
    Serial.println("seq reset");
    return true;
}

bool cmdPause(const char*) {
    if (machineState != STATE_RUNNING) { Serial.println("err bad_state"); return true; }
    // Core 1 ramps the current segment to rest, flushes the ring, snapshots
    // resumePos and enters PAUSED (§4.5). It no longer finishes the segment
    // and drains — resume re-plans from position, so stopping early is safe.
    // Gated on RUNNING, so unlike abort this flag can never strand.
    // Planner motion instead holds and keeps its ring, for `resume`.
    pauseRequested = true;
    Serial.println("ok");
    return true;
}

// A held planner job resumes where it stopped, so the head must still be there.
static bool resumePlanner() {
    if (machinePos[0] != resumePos[0] || machinePos[1] != resumePos[1]) {
        Serial.println("err moved");
        return true;
    }
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool held = plannerActive && machineState == STATE_PAUSED;
    if (held) resumeRequested = true;
    spin_unlock(plannerLock, s);
    Serial.println(held ? "ok" : "err bad_state");
    return true;
}

bool cmdResume(const char*) {
    if (machineState != STATE_PAUSED) { Serial.println("err bad_state"); return true; }
    if (plannerActive) return resumePlanner();
    // Phase 1: the host has already pre-positioned the head, so resume simply
    // leaves PAUSED. The next operation streams in fresh (IDLE accepts MSEG).
    jobActive    = false;
    machineState = STATE_IDLE;
    Serial.println("ok");
    return true;
}

bool cmdCancel(const char*) {
    if (machineState != STATE_PAUSED) { Serial.println("err bad_state"); return true; }
    if (plannerActive) {
        // Core 1 leaves PAUSED for a resume only under the lock.
        const uint32_t s = spin_lock_blocking(plannerLock);
        const bool held = plannerActive && machineState == STATE_PAUSED;
        if (held) {
            const planner::Vec2 at = plannerExec.position();
            plannerRing.reset(at);
            plannerExec.reset(at);
            plannerActive   = false;
            resumeRequested = false;
        }
        spin_unlock(plannerLock, s);
        if (!held) { Serial.println("err bad_state"); return true; }
    }
    mBufHead = mBufTail;                   // buffer already drained at pause; defensive
    queuedUsOut = queuedUsIn;              // …and its queued time with it (§4.6)
    jobActive    = false;
    machineState = STATE_IDLE;
    Serial.println("ok");
    return true;
}

#ifdef PICO_ALLOW_UNCONFIGURED
// Leave ALARM_CONFIG unconfigured: the stored file is kept, and ignored until
// power-off.
bool cmdUncfg(const char*) {
    if (machineState != STATE_ALARM || alarmReason != ALARM_CONFIG) {
        Serial.println("err bad_state");
        return true;
    }
    machineCfgIgnore();
    resumeOrHold();
    Serial.println("ok");
    return true;
}
#endif
