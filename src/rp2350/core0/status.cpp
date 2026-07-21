// Core 0: machine status reporting — binary STATUS_RSP + buffer telemetry.

#include <Arduino.h>
#include <string.h>
#include "../shared.h"
#include "status.h"
#include "data_plane.h"

uint16_t getBufCount() {
    uint16_t h = mBufHead, t = mBufTail;
    if (t >= h) return t - h;
    return MASTER_BUF_SIZE - h + t;
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
    buf[1] = machineState;
    buf[2] = axes_enabled;
    buf[3] = axes_homed;
    buf[4] = alarmReason;
    buf[5] = runningReason;
    buf[6] = (uint8_t)(bufCount & 0xFF);
    buf[7] = (uint8_t)(bufCount >> 8);
    memcpy(&buf[8],  pos, 16);              // pos[4] i32 LE — native byte order
    buf[24] = dataPlaneExpectedSeq();
    memcpy(&buf[25], &qUs, 4);              // queuedUs u32 LE
    buf[29] = crc8(buf, STATUS_RSP_SIZE - 1);

    Serial.write(buf, STATUS_RSP_SIZE);     // one write — cost is per-transaction
}
