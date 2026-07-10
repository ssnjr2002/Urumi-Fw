// Config blob store — A/B double-buffered flash storage for one opaque blob.
// See config_store.h and docs/config_storage.md for the scheme and rationale.

#include <Arduino.h>
#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "../shared.h"
#include "config_store.h"

// Linker symbols bounding the reserved filesystem span (memmap_default.ld
// PROVIDEs these; referencing them here forces emission). We repurpose the low
// CFG_REGION_BYTES of this span as raw config flash — LittleFS is never mounted.
extern uint8_t _FS_start;
extern uint8_t _FS_end;

// ─── Globals ──────────────────────────────────────────────────────────────────

ConfigCache g_cfg = { nullptr, 0, 0, -1 };

// RAM mirror of a blob, filled by the receiver, source for a commit. 32 KB in
// .bss — always resident, but RP2350 has 520 KB SRAM.
static uint8_t cfgStage[CFG_MAX_BYTES];

uint8_t* configStageBuf() { return cfgStage; }

// ─── Geometry helpers ─────────────────────────────────────────────────────────

// Flash offset (relative to XIP_BASE) of the config region base.
static inline uint32_t cfgRegionOff() {
    return (uint32_t)((uintptr_t)&_FS_start - XIP_BASE);
}

// True if the reserved FS span is large enough to hold both slots. Guards every
// read/write so a too-small filesystem_size fails closed instead of corrupting
// whatever sits above the region.
static inline bool cfgRegionOk() {
    return (uint32_t)((uintptr_t)&_FS_end - (uintptr_t)&_FS_start) >= CFG_REGION_BYTES;
}

static inline const uint8_t* slotBase(uint32_t i) {
    return (const uint8_t*)(XIP_BASE + cfgRegionOff() + i * CFG_SLOT_BYTES);
}

// A slot is valid only if the header is well-formed AND the payload CRC matches.
// Returns the payload length via *outLen and seq via *outSeq when valid.
static bool slotValid(uint32_t i, uint32_t* outLen, uint32_t* outSeq) {
    const uint8_t* base = slotBase(i);
    const ConfigBlobHeader* h = (const ConfigBlobHeader*)base;
    if (h->magic != CFG_MAGIC)                       return false;
    if (h->version != CFG_VERSION)                   return false;
    if (h->length == 0 || h->length > CFG_MAX_BYTES) return false;
    if (crc32(base + CFG_SECTOR, h->length) != h->crc32) return false;
    *outLen = h->length;
    *outSeq = h->seq;
    return true;
}

// ─── Boot scan ────────────────────────────────────────────────────────────────

void configStoreInit() {
    g_cfg = { nullptr, 0, 0, -1 };
    if (!cfgRegionOk()) return;   // misconfigured reservation — no config available

    for (uint32_t i = 0; i < CFG_NUM_SLOTS; i++) {
        uint32_t len, seq;
        if (!slotValid(i, &len, &seq)) continue;
        if (g_cfg.slot < 0 || seq > g_cfg.seq) {   // keep the higher-seq valid slot
            g_cfg.addr   = slotBase(i) + CFG_SECTOR;
            g_cfg.length = len;
            g_cfg.seq    = seq;
            g_cfg.slot   = (int8_t)i;
        }
    }
}

// ─── Core 1 flash quiesce (Core 0 side) ───────────────────────────────────────
// These run before/after the XIP-down window, so they may stay flash-resident.
// Core 1's matching park loop is RAM-resident (core1.cpp).

static void core1FlashQuiesce() {
    flash_op_requested = true;
    __dmb();
    while (!core1_parked_for_flash) tight_loop_contents();
}

static void core1FlashRelease() {
    flash_op_requested = false;
    __dmb();
    while (core1_parked_for_flash) tight_loop_contents();
}

// ─── Commit ───────────────────────────────────────────────────────────────────

bool configStoreCommit(uint32_t len, uint32_t crc, uint8_t* nack) {
    // Validate before touching flash — cheap rejects leave the active slot alone.
    // The caller (data-plane receiver) has already verified `crc` against the
    // staged bytes via its incremental CRC32, so we do NOT recompute it here — the
    // post-flash readback below is the remaining integrity gate. A caller passing
    // an inconsistent (buffer, crc) pair still cannot corrupt the active config:
    // readback would fail and the cache swap is skipped.
    if (machineState != STATE_IDLE && machineState != STATE_ALARM) {
        *nack = CFG_NACK_BAD_STATE; return false;
    }
    if (len == 0 || len > CFG_MAX_BYTES) { *nack = CFG_NACK_TOO_BIG; return false; }
    if (!cfgRegionOk())                  { *nack = CFG_NACK_FLASH;   return false; }

    // Target the inactive slot; the active one stays intact until we commit.
    uint8_t  writeSlot = (g_cfg.slot < 0) ? 0 : (uint8_t)(g_cfg.slot ^ 1);
    uint32_t newSeq    = (g_cfg.slot < 0) ? 1u : g_cfg.seq + 1u;
    uint32_t slotOff   = cfgRegionOff() + writeSlot * CFG_SLOT_BYTES;

    // Build the header page (payload CRC + length + new seq), padded to a page.
    uint8_t hp[CFG_HEADER_BYTES];
    memset(hp, 0xFF, sizeof(hp));
    ConfigBlobHeader* h = (ConfigBlobHeader*)hp;
    h->magic = CFG_MAGIC; h->version = CFG_VERSION; h->_rsvd = 0;
    h->seq = newSeq; h->length = len; h->crc32 = crc;

    // Payload program length must be a whole number of flash pages.
    uint32_t payLen = (len + (FLASH_PAGE_SIZE - 1)) & ~(uint32_t)(FLASH_PAGE_SIZE - 1);

    // ── XIP-down critical section ──────────────────────────────────────────────
    // Core 1 parked in RAM; Core 0 IRQs off so no flash-resident ISR runs. Erase
    // the whole slot, program payload, program header LAST so an interrupted write
    // leaves an invalid header (old slot still wins on the next boot scan).
    core1FlashQuiesce();
    uint32_t irq = save_and_disable_interrupts();

    flash_range_erase(slotOff, CFG_SLOT_BYTES);
    flash_range_program(slotOff + CFG_SECTOR, cfgStage, payLen);
    flash_range_program(slotOff, hp, CFG_HEADER_BYTES);

    restore_interrupts(irq);
    core1FlashRelease();
    // ── XIP restored ───────────────────────────────────────────────────────────

    // Readback verify from flash — catches a program failure (distinct from the
    // transfer-integrity CRC checked above). Do not swap the cache on failure.
    const uint8_t* payload = (const uint8_t*)(XIP_BASE + slotOff + CFG_SECTOR);
    if (crc32(payload, len) != crc) { *nack = CFG_NACK_FLASH; return false; }

    // Commit: the new slot is now the active handle.
    g_cfg.addr   = payload;
    g_cfg.length = len;
    g_cfg.seq    = newSeq;
    g_cfg.slot   = (int8_t)writeSlot;
    return true;
}
