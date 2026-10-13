// unalarm.cpp — clear the event faults and fix what can be fixed at once.

#include <Arduino.h>
#include "table.h"
#include "../../ops/machine.h"
#include "../../ops/slot_map.h"
#include "../../ops/axes_map.h"
#include "../../../ipc/shared_state.h"

// The event bits' cleanup ran when they were raised, so they just clear. The
// held bits are fixed in dependency order and the first that cannot be stops
// it: a mute node is `bus_exclude`'s, then an unmet slot request is retried
// once (docs/engage_and_axis_map.md §6.1; one from axes_map re-checks its
// pending axes first). F_CONFIG and F_LATCHED have no instant fix (a config;
// a retract) and are reported in what is left.
bool cmdUnalarm(const char*) {
    if (!machineFaults()) { Serial.println("err bad_state"); return true; }
    // The estop's exit is `unstop`: a map retry here would leave its nodes
    // unconfirmed.
    if (estopLatched()) { Serial.println("err estop"); return true; }
    faultsClearEvents();
    if (machineFaults() & F_BUS) {
        machinePublish();
        Serial.println("err degraded"); return true;
    }
    if (!slotMapComplete()) {
        bool met;
        if (slotMapFromAxes()) { axesMapRetry(); met = slotMapComplete(); }
        else                   met = slotMapRetry();
        if (!met) {
            machinePublish();
            Serial.println("err unmapped"); return true;
        }
    }
    machinePublish();
    // Position is still invalid: run setorigin.
    Serial.printf("ok faults=0x%04x\n", machineFaults());
    return true;
}
