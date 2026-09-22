#pragma once
#include <Arduino.h>
#include "../../ipc/shared_state.h"

// gate.h — the state predicates the handlers gate on.
//
// Header-only, and deliberately NOT a `gate` field on the command table. A gate
// bitmask can express "which states admit this command" and nothing else, while
// the real rules here are argument-dependent (alarmDeniesOn keys on the value
// being written, not the command) and in one case time-dependent (a fault that
// arrives while setorigin is on the bus). Those do not fit in a table column,
// so the checks stay where they can see their own arguments -- see
// docs/PLAN_rp2350_refactor.md §7.

static inline bool stateIs(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t s = machineState;
    return s == a || s == b || s == c;
}

// ALARM is a fault state, and the peripheral gates admit it so an operator can
// park a machine that faulted with the pump running or the blade hot. That
// direction is recovery; the other is not. Turning a peripheral ON in ALARM
// energises a hot blade or a pump on a machine whose datum is already lost and
// whose estop sweep has just parked the whole bus — there is no workflow that
// wants it, and an operator reaching for `knife_osc N on` to test something has
// misread the state.
//
// Off stays permitted in every state the gate allows, so this can never trap a
// running peripheral. Prints its own error; callers return on true.
static inline bool alarmDeniesOn(bool turningOn) {
    if (turningOn && machineState == STATE_ALARM) {
        Serial.println("err bad_state");
        return true;
    }
    return false;
}

static inline const char* stateName(uint8_t s) {
    switch (s) {
        case STATE_IDLE:    return "IDLE";
        case STATE_RUNNING: return "RUNNING";
        case STATE_ESTOP:   return "ESTOP";
        case STATE_ALARM:   return "ALARM";
        case STATE_PAUSED:  return "PAUSED";
        case STATE_HOMING:  return "HOMING";
        default:            return "?";
    }
}
