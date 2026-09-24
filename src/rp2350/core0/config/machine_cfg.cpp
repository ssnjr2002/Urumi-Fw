// Active decoded config — see machine_cfg.h.

#include <Arduino.h>
#include "machine_cfg.h"
#include "config_store.h"
#include "../../ipc/shared_state.h"

static_assert(CFG_BUS_ADDR_MAX == BUS_ADDR_MAX, "decoder node-id range must match the bus");

static MachineCfg     active;
static MachineCfg     pending;
static bool           activeValid = false;
static CfgDecodeError lastError   = CFG_DEC_OK;

void machineCfgLoad() {
    activeValid = false;
    lastError   = CFG_DEC_OK;
    if (!g_cfg.valid) return;

    // No transfer is in flight at boot, so the staging buffer is free.
    uint8_t* buf = configStageBuf();
    if (configStoreRead(0, buf, g_cfg.length) != g_cfg.length) {
        lastError = CFG_DEC_MSGPACK;
        return;
    }
    lastError   = configDecode(buf, g_cfg.length, &active);
    activeValid = (lastError == CFG_DEC_OK);
}

CfgDecodeError machineCfgStage(const uint8_t* blob, uint32_t len) {
    CfgDecodeError e = configDecode(blob, len, &pending);
    if (e != CFG_DEC_OK) lastError = e;
    return e;
}

void machineCfgAdopt() {
    active      = pending;
    activeValid = true;
    lastError   = CFG_DEC_OK;
}

bool              machineCfgValid() { return activeValid; }
const MachineCfg& machineCfg()      { return active; }
CfgDecodeError    machineCfgError() { return lastError; }
