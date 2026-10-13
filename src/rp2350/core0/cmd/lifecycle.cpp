// lifecycle.cpp — the job lifecycle: stop, unstop, reset, pause, resume,
// cancel, and the data-plane sequence reset.
//
// All but unstop are state writes that Core 1 observes on its next pass, with
// no bus traffic. unstop is the estop's exit, and it has to hear every node.

#include <Arduino.h>
#include "table.h"
#include "gate.h"
#include "../data_plane.h"   // dataPlaneResetSeq (seqreset)
#include "../planner/queue.h"  // plannerEndContour
#include "../ops/slot_map.h"
#include "../ops/position.h"
#include "../ops/machine.h"
#include "../ops/bus.h"
#include "../config/machine_cfg.h"
#include "../usb_protocol.h" // BUS_ADDR_MAX
#include "../../ipc/shared_state.h"

// Core 1 flushes, sweeps the bus safe and publishes ALARM. The datum dies
// here, before the reply; the fenced slots' positions are cleared once Core 1
// has stopped writing them (FACT_ESTOP_SWEPT, ops/observe.cpp).
bool cmdStop(const char*) {
    estopLatch();
    originInvalidateAll();
    slotFenceAllBound();
    Serial.println("ok");
    return true;
}

// Make safe every touched node (energised, or holding a slot) that is not
// excluded; an excluded node's slot stays fenced. Each
// confirmation frees that node's slot and clears its fence. Refused, with the
// unconfirmed ids, until all confirm; then the requests are forgotten, the
// latch clears and the machine settles IDLE (or ALARM on a held fault),
// unmapped, de-energised, un-homed. Refused until Core 1's sweep published ALARM.
bool cmdUnstop(const char*) {
    if (!estopLatched() || machineState != STATE_ALARM) {
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
    estopClear();
    machineSettle();
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

// A buffering or running job holds and lands JOB.PAUSED (Core 1), keeping its
// ring for `resume`.
bool cmdPause(const char*) {
    const uint8_t why = jobReason;
    if (machineState != STATE_JOB || (why != JOB_BUFFERING && why != JOB_RUNNING)) {
        Serial.println("err bad_state"); return true;
    }
    pauseRequested = true;
    Serial.println("ok");
    return true;
}

// The job continues where it stopped, so the head must still be there. Core 1
// replans from rest and buffers again.
bool cmdResume(const char*) {
    if (!jobPaused()) { Serial.println("err bad_state"); return true; }
    if (machinePos[0] != resumePos[0] || machinePos[1] != resumePos[1]) {
        Serial.println("err moved");
        return true;
    }
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool paused = jobPaused();
    if (paused) resumeRequested = true;
    spin_unlock(plannerLock, s);
    Serial.println(paused ? "ok" : "err bad_state");
    return true;
}

// End a job at rest (buffering or paused): its ring is dropped. Core 1 starts a
// buffered job only under the lock, so the check holds.
bool cmdCancel(const char*) {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const uint8_t why = jobReason;
    const bool atRest = machineState == STATE_JOB && (why == JOB_PAUSED || why == JOB_BUFFERING);
    if (atRest) {
        const planner::Pos at = plannerExec.position();
        plannerRing.reset(at);
        plannerExec.reset(at);
        plannerActive   = false;
        resumeRequested = false;
        pauseRequested  = false;
        __dmb();
        machineState = STATE_IDLE;
    }
    spin_unlock(plannerLock, s);
    if (atRest) plannerEndContour();
    Serial.println(atRest ? "ok" : "err bad_state");
    return true;
}

#ifdef PICO_ALLOW_UNCONFIGURED
// Leave F_CONFIG unconfigured: the stored file is kept, and ignored until
// power-off.
bool cmdUncfg(const char*) {
    if (!(machineFaults() & F_CONFIG)) {
        Serial.println("err bad_state");
        return true;
    }
    machineCfgIgnore();
    machinePublish();
    Serial.println("ok");
    return true;
}
#endif
