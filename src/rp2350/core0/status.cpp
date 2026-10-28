// Core 0: machine status reporting — binary STATUS_RSP + buffer telemetry.

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "../ipc/shared_state.h"
#include "usb_protocol.h"
#include "status.h"
#include "ops/machine.h"
#include "ops/position.h"
#include "ops/frames.h"
#include "ops/tools.h"
#include "planner/queue.h"
#include "config/machine_cfg.h"

uint16_t getBufCount() { return (uint16_t)plannerRing.count(); }

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

static_assert(CFG_MAX_HEADS == 2, "STATUS_RSP carries tools[2]; a new layout needs a new magic");

static void putU16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }   // LE: native order

// One coherent sample, one transaction (layout in usb_protocol.h).
// Everything is read into locals up front: the frame is assembled while Core 1
// is running, and a field read late would describe a different instant than
// one read early.
void sendStatusRsp() {
    const uint8_t  st       = machineState;
    const uint8_t  reason   = stateReason(st);
    const uint16_t faults   = machineFaults();
    const uint16_t enabled  = nodeEnabled;
    const uint16_t homed    = originMask();
    const uint8_t  sel      = framesSelected();
    const uint16_t bufCount = getBufCount();
    const uint8_t heads = machineCfgValid() ? machineCfg().headCount : 0;
    uint8_t tools[CFG_MAX_HEADS];
    for (uint8_t h = 0; h < CFG_MAX_HEADS; h++) tools[h] = h < heads ? toolsMounted(h) : 0;
    float wpos[4];
    for (uint8_t i = 0; i < 4; i++)
        if (!framesWPos(i, &wpos[i])) wpos[i] = NAN;
    uint16_t layer  = 0xFFFF;
    uint32_t record = 0;
    uint8_t  ctx    = CTX_IDLE;
    if (!plannerExecPoint(&layer, &record, &ctx)) { layer = 0xFFFF; record = 0; }

    uint8_t buf[STATUS_RSP_SIZE];
    buf[0] = STATUS_RSP;
    buf[1] = st;
    buf[2] = reason;
    putU16(&buf[3], faults);
    putU16(&buf[5], enabled);
    putU16(&buf[7], homed);
    buf[9]  = sel;
    memcpy(&buf[10], tools, CFG_MAX_HEADS);
    memcpy(&buf[12], wpos, 16);
    putU16(&buf[28], bufCount);
    putU16(&buf[30], layer);
    memcpy(&buf[32], &record, 4);
    buf[36] = ctx;
    buf[37] = crc8(buf, STATUS_RSP_SIZE - 1);

    Serial.write(buf, STATUS_RSP_SIZE);     // one write — cost is per-transaction
}
