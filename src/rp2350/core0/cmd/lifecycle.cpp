// lifecycle.cpp — the job lifecycle: stop, reset, pause, resume, cancel,
// unalarm, and the data-plane sequence reset.
//
// The estop and pause paths have no transport coupling and no position coupling
// to audit. Every one of these commands is a state write that Core 1 observes on
// its next pass -- none of them touches the bus.
//
// homing.h is the one exception, and it does not weaken that: `unalarm` calls
// resumeOrHold() to READ the latched-limit mask, because a recovery that cannot
// see which conditions still hold is not a recovery. Still no bus, still a
// state write.

#include <Arduino.h>
#include "table.h"
#include "gate.h"
#include "../homing.h"
#include "../data_plane.h"   // dataPlaneResetSeq (seqreset)
#include "../../ipc/shared_state.h"

bool cmdStop(const char*) {
    machineState = STATE_ESTOP;            // Core 1 flushes, clears axes, → ALARM
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
    pauseRequested = true;
    Serial.println("ok");
    return true;
}

bool cmdResume(const char*) {
    if (machineState != STATE_PAUSED) { Serial.println("err bad_state"); return true; }
    // Phase 1: the host has already pre-positioned the head, so resume simply
    // leaves PAUSED. The next operation streams in fresh (IDLE accepts MSEG).
    jobActive    = false;
    machineState = STATE_IDLE;
    Serial.println("ok");
    return true;
}

bool cmdCancel(const char*) {
    if (machineState != STATE_PAUSED) { Serial.println("err bad_state"); return true; }
    mBufHead = mBufTail;                   // buffer already drained at pause; defensive
    queuedUsOut = queuedUsIn;              // …and its queued time with it (§4.6)
    jobActive    = false;
    machineState = STATE_IDLE;
    Serial.println("ok");
    return true;
}

bool cmdUnalarm(const char*) {
    if (machineState != STATE_ALARM) { Serial.println("err bad_state"); return true; }
    // The config gate is not a clearable fault — only a committed axis_map
    // leaves it (docs/engage_and_axis_map.md §6.1).
    if (alarmReason == ALARM_CONFIG) { Serial.println("err unconfigured"); return true; }
    // Not a plain clear-to-IDLE: an axis may still be standing on its switch,
    // and `unalarm` does not move anything, so the condition that raised
    // ALARM_LIMIT_LATCHED is still true afterwards. Answering `ok` and dropping
    // to IDLE would let a job start against a node that refuses stream steps.
    // Retract it (a `home` in the opposite direction) to leave that alarm.
    resumeOrHold();
    Serial.println("ok");                  // position still invalid — run setorigin
    return true;
}
