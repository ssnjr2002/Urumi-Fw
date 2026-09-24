// Controller — see controller.h.

#include <Arduino.h>
#include "controller.h"
#include "../config/machine_cfg.h"
#include "../core0/position.h"      // SLOT_NONE
#include "../core0/cmd/axis_map.h"
#include "../ipc/shared_state.h"
#include "hardware/sync.h"          // __dmb

void controllerApplyDefaultMap() {
    if (!machineCfgValid()) {
        alarmReason  = ALARM_CONFIG;
        __dmb();
        machineState = STATE_ALARM;
        return;
    }
    const MachineCfg& cfg = machineCfg();
    uint8_t map[4];
    configSlotMap(cfg, cfg.defaultHead, SLOT_NONE, map);
    // Quiet: nothing asked for this, so there is no command to answer. The
    // outcome is the state axisMapApply leaves behind.
    axisMapApply(map, /*quiet=*/true);
}
