// unalarm.cpp — leave an alarm once the conditions behind it are gone.

#include <Arduino.h>
#include "table.h"
#include "../../ops/state.h"
#include "../../ops/axis_map.h"
#include "../../../ipc/shared_state.h"

bool cmdUnalarm(const char*) {
    if (machineState != STATE_ALARM) { Serial.println("err bad_state"); return true; }
    // An incomplete axis map is retried once (docs/engage_and_axis_map.md §6.1).
    if (!axisMapComplete() && !axisMapRetry()) {
        Serial.println("err unmapped");
        return true;
    }
    // Not a plain clear-to-IDLE: an axis may still be standing on its switch,
    // and `unalarm` does not move anything, so the condition that raised
    // ALARM_LIMIT_LATCHED is still true afterwards. Answering `ok` and dropping
    // to IDLE would let a job start against a node that refuses stream steps.
    // Retract it (a `home` in the opposite direction) to leave that alarm.
    resumeOrHold();
    Serial.println("ok");                  // position still invalid — run setorigin
    return true;
}
