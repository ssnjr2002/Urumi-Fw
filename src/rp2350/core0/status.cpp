// Core 0: machine status reporting — binary STATUS_RSP + buffer telemetry.

#include <Arduino.h>
#include <string.h>
#include "../ipc/shared_state.h"
#include "usb_protocol.h"
#include "status.h"
#include "data_plane.h"
#include "ops/machine.h"

uint16_t getBufCount() { return (uint16_t)plannerRing.count(); }

// The ring's unclaimed motion, µs. Under the lock: Core 1 claims and pops.
static uint32_t queuedUs() {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const float t = plannerRing.queuedTime();
    spin_unlock(plannerLock, s);
    return (uint32_t)(t * 1e6f);
}

// The current state's reason byte; 0 for a state without one.
static uint8_t stateReason(uint8_t st) {
    switch (st) {
        case STATE_JOB:     return jobReason;
        case STATE_JOGGING: return ringOwner;
        case STATE_HOMING:  return homingReason;
        case STATE_PROBING: return probingReason;
        default:            return 0;
    }
}

// One coherent sample, one transaction (§4.2/§4.6 — layout in shared.h).
// Everything is read into locals up front: the frame is assembled over ~30
// bytes while Core 1 is running, and a field read late would describe a
// different instant than one read early. Cheap here, and it makes the sample
// mean what a caller assumes it means.
void sendStatusRsp() {
    uint8_t  buf[STATUS_RSP_SIZE];
    uint16_t bufCount = getBufCount();
    uint32_t qUs      = queuedUs();
    int32_t  pos[4]   = { machinePos[0], machinePos[1],
                          machinePos[2], machinePos[3] };

    buf[0] = STATUS_RSP;
    const uint8_t st = machineState;
    buf[1] = st;
    buf[2] = axes_enabled;
    buf[3] = axes_homed;
    const uint16_t faults = machineFaults();
    buf[4] = (uint8_t)(faults & 0xFF);
    buf[5] = (uint8_t)(faults >> 8);
    buf[6] = stateReason(st);
    buf[7] = (uint8_t)(bufCount & 0xFF);
    buf[8] = (uint8_t)(bufCount >> 8);
    memcpy(&buf[9],  pos, 16);              // pos[4] i32 LE — native byte order
    buf[25] = dataPlaneExpectedSeq();
    memcpy(&buf[26], &qUs, 4);              // queuedUs u32 LE
    buf[30] = crc8(buf, STATUS_RSP_SIZE - 1);

    Serial.write(buf, STATUS_RSP_SIZE);     // one write — cost is per-transaction
}
