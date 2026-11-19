#pragma once
#include <stdint.h>
#include "../../ipc/shared_state.h"    // flash-quiesce handshake flags
#include "../usb_protocol.h"  // CFG_MAX_BYTES, CFG_NACK_*

// ─────────────────────────────────────────────────────────────────────────────
// Config blob store — one msgpack blob kept in LittleFS as /config.bin
// (docs/system/config.md). Owns the filesystem and the RAM staging buffer;
// knows nothing about USB. The data-plane receiver fills configStageBuf() and
// calls configStoreCommit(); readers stream the file via configStoreRead().
// The filesystem is the span board_build.filesystem_size reserves.
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

// RAM copy of the active blob's header. Set by configStoreInit() and a
// successful configStoreCommit() only; a soft reset keeps it, as flash is
// unchanged.
struct ConfigCache {
    bool     mounted;  // LittleFS mounted
    bool     valid;    // a verified /config.bin exists
    uint32_t length;   // active payload length, 0 if none
    uint32_t seq;      // active blob's seq
    uint32_t crc32;    // active payload CRC32
};
extern ConfigCache g_cfg;

// Why configStoreInit() left no valid config.
enum CfgFileStatus : uint8_t {
    CFG_FILE_OK = 0,
    CFG_FILE_ABSENT,    // no /config.bin
    CFG_FILE_FS,        // LittleFS did not mount
    CFG_FILE_BAD,       // header, length or CRC32 wrong
};
CfgFileStatus configStoreStatus();

// Mount LittleFS (formatting it if it holds no filesystem), then verify
// /config.bin (version/length/CRC32) and populate g_cfg. Call once from setup().
// If the file is missing or bad, g_cfg is left invalid.
void configStoreInit();

// Staging buffer the data-plane receiver fills before committing. Exactly
// CFG_MAX_BYTES; bytes beyond the committed length are ignored.
uint8_t* configStageBuf();

// Commit the first `len` bytes of the staging buffer as the new active blob.
// `crc` is the caller's CRC32 of those bytes, already checked against them; it
// is stored and used for the readback, not recomputed. Parks Core 1, writes
// /config.tmp, reads it back, renames it over /config.bin and updates g_cfg.
// False with *nack (CFG_NACK_*) on bad state, size or flash; the previous
// config stays active.
bool configStoreCommit(uint32_t len, uint32_t crc, uint8_t* nack);

// The CRC32 a commit wants, of `len` bytes at `p`.
uint32_t configStoreCrc(const uint8_t* p, uint32_t len);

// Copy up to `n` payload bytes starting at `off` of the active blob into `dst`.
// Returns the number of bytes copied (0 when there is no valid config).
uint32_t configStoreRead(uint32_t off, uint8_t* dst, uint32_t n);
