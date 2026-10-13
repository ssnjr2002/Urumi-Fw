#pragma once
#include <Arduino.h>
#include "../../ipc/shared_state.h"
#include "../ops/machine.h"

// gate.h — the argument-dependent checks the handlers share. The state rule
// each command needs is its kind (CmdKind, ops/machine.h), applied by
// control_plane before the handler runs.

static inline bool jobPaused() { return machineContext == CTX_JOB && jobReason == JOB_PAUSED; }

// Peripherals may be turned off under any fault, so an operator can park a
// machine that faulted with the pump running or the blade hot; turning one ON
// with a fault set energises it on a machine whose datum may be lost, and has
// no recovery role. Prints its own error; callers return on true.
static inline bool alarmDeniesOn(bool turningOn) {
    if (turningOn && machineFaults()) {
        Serial.println("err bad_state");
        return true;
    }
    return false;
}

static inline const char* stateName(uint8_t s) {
    switch (s) {
        case STATE_IDLE:     return "IDLE";
        case STATE_ESTOP:    return "ESTOP";
        case STATE_ALARM:    return "ALARM";
        case STATE_HOMING:   return "HOMING";
        case STATE_PROBING:  return "PROBING";
        case STATE_JOGGING:  return "JOGGING";
        case STATE_TRAVERSE: return "TRAVERSE";
        case STATE_JOB:      return "JOB";
        default:             return "?";
    }
}
