#pragma once
#include <stdint.h>
#include "config_decode.h"

// ─────────────────────────────────────────────────────────────────────────────
// The active decoded config. config_store.* owns the bytes; this owns what
// they mean. Updated at boot and by an accepted CFG_SET, never by soft reset.
// ─────────────────────────────────────────────────────────────────────────────

// Decode the stored blob (config_store) into the active config. Call once from
// setup(), after configStoreInit(). No stored blob or a bad one leaves the
// active config invalid.
void machineCfgLoad();

// Decode `len` bytes into the pending slot. Returns CFG_DEC_OK when the blob
// may be committed; the active config is untouched either way.
CfgDecodeError machineCfgStage(const uint8_t* blob, uint32_t len);

// Promote the pending config to active. Call only after the blob it came from
// has been committed to flash.
void machineCfgAdopt();

bool              machineCfgValid();
const MachineCfg& machineCfg();       // meaningful only when machineCfgValid()

// The most recent rejection — of the stored blob at boot, or of a CFG_SET —
// or CFG_DEC_OK once a config is adopted. For `status cfg`.
CfgDecodeError    machineCfgError();
