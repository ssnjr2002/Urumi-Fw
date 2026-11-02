#pragma once
#include <stdint.h>
#include "config_decode.h"
#include "runtime_edit.h"

// ─────────────────────────────────────────────────────────────────────────────
// The active decoded config. config_store.* owns the bytes; this owns what
// they mean. Updated at boot, by an accepted CFG_SET and by `set`; a soft reset
// restores the stored one if `set` left it dirty.
// ─────────────────────────────────────────────────────────────────────────────

// Decode the stored blob (config_store) into the active config and clear dirty.
// Call from setup(), after configStoreInit(), and from a soft reset while
// dirty. No stored blob or a bad one leaves the active config invalid. Uses the
// staging buffer.
void machineCfgLoad();

// Copy the buffer times to Core 1 (shared_state.h).
void machineCfgPublish();

// Edit one field of the active config (runtime_edit.h). A change sets dirty.
RtError machineCfgSet(const char* path, size_t len, const char* text, RtApplied* out);

// The active config holds edits the stored one does not.
bool machineCfgDirty();

// Store the active config's table fields into the stored blob and clear dirty.
// Returns nullptr, or why not. The caller checks the machine state.
const char* machineCfgSave();

// Decode `len` bytes into the pending slot. Returns CFG_DEC_OK when the blob
// may be committed; the active config is untouched either way.
CfgDecodeError machineCfgStage(const uint8_t* blob, uint32_t len);

// Promote the pending config to active. Call only after the blob it came from
// has been committed to flash.
void machineCfgAdopt();

bool              machineCfgValid();
const MachineCfg& machineCfg();       // meaningful only when machineCfgValid()

// The most recent rejection — of the stored blob at boot, or of a CFG_SET —
// or CFG_DEC_OK once a config is adopted. For `cfg`.
CfgDecodeError    machineCfgError();

// No usable config and `uncfg` has not set it aside: F_CONFIG
// (ops/machine.h). Cleared by an adopted CFG_SET, or by machineCfgIgnore() until
// power-off (soft reset keeps it; the config loads only in setup()).
bool machineCfgBlocking();
void machineCfgIgnore();
bool machineCfgIgnored();
// Why boot found no usable config, for get cfgerr=: "absent", "fs",
// "file", or the stored blob's decode error name.
const char* machineCfgBlockName();
