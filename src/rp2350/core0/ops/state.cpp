// state.cpp — see state.h.

#include "state.h"
#include "position.h"          // homingLatched
#include "axis_map.h"
#include "../../ipc/shared_state.h"
#include "hardware/sync.h"     // __dmb

void resumeOrHold(void) {
    uint8_t reason;
    if      (!axisMapComplete())  reason = ALARM_NODE_FAULT;
    else if (homingLatched)       reason = ALARM_LIMIT_LATCHED;
    else                          reason = ALARM_NONE;
    // Reason before state, matching how Core 1 publishes the pair.
    alarmReason  = reason;
    __dmb();
    machineState = reason == ALARM_NONE ? STATE_IDLE : STATE_ALARM;
}
