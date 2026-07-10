// Core 0: machine status reporting — binary STATUS_RSP + buffer telemetry.

#include <Arduino.h>
#include "../shared.h"
#include "status.h"

uint16_t getBufCount() {
    uint16_t h = mBufHead, t = mBufTail;
    if (t >= h) return t - h;
    return MASTER_BUF_SIZE - h + t;
}

void sendStatusRsp() {
    uint8_t buf[STATUS_RSP_SIZE];
    uint16_t bufCount = getBufCount();
    buf[0] = STATUS_RSP;
    buf[1] = machineState;
    buf[2] = axes_enabled;
    buf[3] = axes_homed;
    buf[4] = alarmReason;
    buf[5] = runningReason;
    buf[6] = (uint8_t)(bufCount & 0xFF);
    buf[7] = (uint8_t)(bufCount >> 8);
    buf[8] = crc8(buf, STATUS_RSP_SIZE - 1);
    Serial.write(buf, STATUS_RSP_SIZE);
}
