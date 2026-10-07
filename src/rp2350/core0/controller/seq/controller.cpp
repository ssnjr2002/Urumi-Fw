// Controller — see controller.h.

#include <Arduino.h>
#include "controller.h"
#include "../../config/machine_cfg.h"
#include "../../ops/position.h"      // SLOT_NONE
#include "../../ops/axes_map.h"
#include "../../ops/state.h"

void controllerApplyDefaultMap() {
    if (!machineCfgValid()) {
        resumeOrHold();                    // ALARM_CONFIG, or IDLE after uncfg
        return;
    }
    const MachineCfg& cfg = machineCfg();
    uint8_t map[4];
    configSlotMap(cfg, cfg.defaultHead, SLOT_NONE, map);
    // Quiet: nothing asked for this, so there is no command to answer. The
    // outcome is the state axesMapApply leaves behind. A wrong type in the
    // config is kept pending rather than refused, so it shows as NODE_FAULT.
    axesMapApply(map, /*keepWrongType=*/true);
}
