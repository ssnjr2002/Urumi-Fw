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
static bool           dirty       = false;

void machineCfgPublish() {
    bufferQueueS = active.buffer.queueTime;
    bufferIdleMs = (uint32_t)(active.buffer.idleTime * 1000.0f);
}

void machineCfgLoad() {
    activeValid = false;
    dirty       = false;
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
    if (activeValid) machineCfgPublish();
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
    dirty       = false;
    machineCfgPublish();
}

RtError machineCfgSet(const char* path, size_t len, const char* text, RtApplied* out) {
    const RtError e = runtimeSet(active, path, len, text, out);
    if (e == RT_OK && out->changed) dirty = true;
    return e;
}

bool machineCfgDirty() { return dirty; }

const char* machineCfgSave() {
    uint8_t* buf = configStageBuf();
    if (configStoreRead(0, buf, g_cfg.length) != g_cfg.length) return "flash";
    size_t len;
    switch (runtimeEncode(active, buf, g_cfg.length, CFG_MAX_BYTES, &pending, &len)) {
        case RT_ENC_OK:      break;
        case RT_ENC_DECODE:  return "decode";
        case RT_ENC_TOO_BIG: return "too_big";
        case RT_ENC_VERIFY:  return "verify";
    }
    uint8_t nack;
    if (!configStoreCommit(len, configStoreCrc(buf, len), &nack))
        return nack == CFG_NACK_BAD_STATE ? "bad_state" : "flash";
    dirty = false;
    return nullptr;
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
