#pragma once
#include <stdint.h>
#include "../ipc/shared_state.h"    // flash-quiesce handshake flags
#include "../core0/usb_protocol.h"  // CFG_MAX_BYTES, CFG_NACK_*

// ─────────────────────────────────────────────────────────────────────────────
// Config blob store — flash-backed storage for one opaque msgpack blob.
//
// Ownership: this module owns the flash region and the RAM staging buffer. It
// knows nothing about USB — the data-plane receiver fills configStageBuf() and
// calls configStoreCommit(); readers stream straight from g_cfg. Design and the
// A/B power-safety scheme: docs/config_storage.md.
//
// Region: carved from the linker-reserved filesystem span (board_build.
// filesystem_size in platformio.ini), addressed via the _FS_start/_FS_end
// symbols — never a hardcoded offset — so the linker provably keeps program code
// out and the layout survives flash-size changes. We never mount LittleFS on it.
// ─────────────────────────────────────────────────────────────────────────────

// Flash geometry. Two slots (A/B) so a write leaves the previous config intact
// until the new one is fully committed (header programmed last = commit point).
#define CFG_SECTOR        4096u
#define CFG_HEADER_BYTES  256u                             // header occupies 1 flash page
#define CFG_PAYLOAD_SECTORS 8u                             // 8 * 4096 = 32 KB payload
#define CFG_SLOT_BYTES    ((1u + CFG_PAYLOAD_SECTORS) * CFG_SECTOR)  // 36 KB (hdr sector + payload)
#define CFG_NUM_SLOTS     2u
#define CFG_REGION_BYTES  (CFG_NUM_SLOTS * CFG_SLOT_BYTES) // 72 KB

#define CFG_MAGIC   0x424C4243u   // 'C','B','L','B' (little-endian)
#define CFG_VERSION 1u            // header/storage format version (not the payload's)

// 20-byte header; stored in the first page of each slot (padded to CFG_HEADER_BYTES).
struct ConfigBlobHeader {
    uint32_t magic;     // CFG_MAGIC — distinguishes written from erased (0xFF) flash
    uint16_t version;   // CFG_VERSION — storage format, governs future field layout
    uint16_t _rsvd;     // alignment padding (keeps seq 4-byte aligned)
    uint32_t seq;       // monotonic; the higher valid seq is the active slot
    uint32_t length;    // payload byte count (1..CFG_MAX_BYTES)
    uint32_t crc32;     // CRC32 over payload[0..length)
};

// RAM handle to the active blob. Set once by configStoreInit(), updated only by a
// successful configStoreCommit(). Intentionally NOT wiped by soft reset (it mirrors
// flash, which soft reset does not touch). addr points into XIP — reads are free.
struct ConfigCache {
    const uint8_t* addr;   // XIP pointer to active payload, or nullptr if none
    uint32_t       length; // active payload length, 0 if none
    uint32_t       seq;    // active slot's seq
    int8_t         slot;   // active slot index (0/1), -1 if none valid
};
extern ConfigCache g_cfg;

// Cold-boot scan: validate both slots (magic/version/length/CRC32), pick the
// higher-seq valid slot, populate g_cfg. Pure flash reads — no parking needed.
// Call once from setup(). If neither slot is valid, g_cfg is left empty.
void configStoreInit();

// Staging buffer the data-plane receiver fills before committing. Exactly
// CFG_MAX_BYTES; bytes beyond the committed length are ignored.
uint8_t* configStageBuf();

// Commit the first `len` bytes of the staging buffer to the inactive slot.
// `crc` is the caller's CRC32 of those bytes — the caller MUST have verified it
// against the staged bytes (the receiver does this incrementally); it is stored
// in the header and used for the post-flash readback verify, not recomputed here.
// Quiesces Core 1, erases+programs the inactive slot, readback-verifies, and on
// success atomically swaps g_cfg. Returns false and sets *nack (CFG_NACK_*) on
// bad state / size / flash-verify — leaving the previous config active.
bool configStoreCommit(uint32_t len, uint32_t crc, uint8_t* nack);
