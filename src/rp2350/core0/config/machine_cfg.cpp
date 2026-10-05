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
static CfgDecodeError bootError   = CFG_DEC_OK;   // the stored blob's, for cfgerr=
static bool           ignored     = false;

void machineCfgLoad() {
    activeValid = false;
    lastError   = CFG_DEC_OK;
    if (!g_cfg.valid) return;

    // No transfer is in flight at boot, so the staging buffer is free.
    uint8_t* buf = configStageBuf();
    if (configStoreRead(0, buf, g_cfg.length) != g_cfg.length) {
        lastError = bootError = CFG_DEC_MSGPACK;
        return;
    }
    lastError   = configDecode(buf, g_cfg.length, &active);
    activeValid = (lastError == CFG_DEC_OK);
    bootError   = lastError;
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
    ignored     = false;
}

bool              machineCfgValid() { return activeValid; }
const MachineCfg& machineCfg()      { return active; }
CfgDecodeError    machineCfgError() { return lastError; }

bool machineCfgBlocking() { return !activeValid && !ignored; }
void machineCfgIgnore()   { ignored = true; }
bool machineCfgIgnored()  { return ignored; }

const char* machineCfgBlockName() {
    switch (configStoreStatus()) {
        case CFG_FILE_ABSENT: return "absent";
        case CFG_FILE_FS:     return "fs";
        case CFG_FILE_BAD:    return "file";
        default:              return configDecodeErrorName(bootError);
    }
}
