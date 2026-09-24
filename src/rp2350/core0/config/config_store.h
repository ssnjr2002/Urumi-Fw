#pragma once
#include <stdint.h>
#include "../../ipc/shared_state.h"    // flash-quiesce handshake flags
#include "../usb_protocol.h"  // CFG_MAX_BYTES, CFG_NACK_*

// ─────────────────────────────────────────────────────────────────────────────
// Config blob store — one opaque msgpack blob kept in LittleFS as /config.bin.
//
// Ownership: this module owns the filesystem and the RAM staging buffer. It
// knows nothing about USB — the data-plane receiver fills configStageBuf() and
// calls configStoreCommit(); readers stream the file via configStoreRead().
// Design and the power-safety scheme: docs/config_storage.md.
//
// The filesystem occupies the linker-reserved span set by board_build.
// filesystem_size in platformio.ini.
// ─────────────────────────────────────────────────────────────────────────────

#define CFG_VERSION 1u            // file header format version (not the payload's)

// 16-byte header at the start of /config.bin; the payload follows it.
struct ConfigBlobHeader {
    uint16_t version;   // CFG_VERSION
    uint16_t _rsvd;     // alignment padding
    uint32_t seq;       // monotonic write counter
    uint32_t length;    // payload byte count (1..CFG_MAX_BYTES)
    uint32_t crc32;     // CRC32 over the payload
};

// RAM copy of the active blob's header. Set once by configStoreInit(), updated
// only by a successful configStoreCommit(). Intentionally NOT wiped by soft reset
// (it mirrors flash, which soft reset does not touch).
struct ConfigCache {
    bool     mounted;  // LittleFS mounted
    bool     valid;    // a verified /config.bin exists
    uint32_t length;   // active payload length, 0 if none
    uint32_t seq;      // active blob's seq
    uint32_t crc32;    // active payload CRC32
};
extern ConfigCache g_cfg;

// Mount LittleFS (formatting it if it holds no filesystem), then verify
// /config.bin (version/length/CRC32) and populate g_cfg. Call once from setup().
// If the file is missing or bad, g_cfg is left invalid.
void configStoreInit();

// Staging buffer the data-plane receiver fills before committing. Exactly
// CFG_MAX_BYTES; bytes beyond the committed length are ignored.
uint8_t* configStageBuf();

// Commit the first `len` bytes of the staging buffer as the new active blob.
// `crc` is the caller's CRC32 of those bytes — the caller MUST have verified it
// against the staged bytes (the receiver does this incrementally); it is stored
// in the header and used for the readback verify, not recomputed here.
// Parks Core 1, writes /config.tmp, readback-verifies, then renames it over
// /config.bin and updates g_cfg. Returns false and sets *nack (CFG_NACK_*) on
// bad state / size / flash failure — leaving the previous config active.
bool configStoreCommit(uint32_t len, uint32_t crc, uint8_t* nack);

// Copy up to `n` payload bytes starting at `off` of the active blob into `dst`.
// Returns the number of bytes copied (0 when there is no valid config).
uint32_t configStoreRead(uint32_t off, uint8_t* dst, uint32_t n);
