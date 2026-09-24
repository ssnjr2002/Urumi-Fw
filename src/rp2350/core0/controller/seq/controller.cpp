// Controller — see controller.h.

#include <Arduino.h>
#include "controller.h"
#include "../../config/machine_cfg.h"
#include "../../ops/position.h"      // SLOT_NONE
#include "../../ops/axis_map.h"
#include "../../ops/state.h"

void controllerApplyDefaultMap() {
    if (!machineCfgValid()) {
        resumeOrHold();                    // ALARM_CONFIG
        return;
    }
    const MachineCfg& cfg = machineCfg();
    uint8_t map[4];
    configSlotMap(cfg, cfg.defaultHead, SLOT_NONE, map);
    // Quiet: nothing asked for this, so there is no command to answer. The
    // outcome is the state axisMapApply leaves behind.
    axisMapApply(map, /*quiet=*/true);
}
