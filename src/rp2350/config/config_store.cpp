// Config blob store — /config.bin in LittleFS.
// See config_store.h and docs/config_storage.md for the scheme and rationale.

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include "hardware/sync.h"
#include "../ipc/shared_state.h"
#include "../core0/usb_protocol.h"
#include "config_store.h"

static const char* const CFG_PATH = "/config.bin";
static const char* const TMP_PATH = "/config.tmp";

// ─── Globals ──────────────────────────────────────────────────────────────────

ConfigCache g_cfg = { false, false, 0, 0, 0 };

// RAM mirror of a blob, filled by the receiver, source for a commit. 32 KB in
// .bss — always resident, but RP2350 has 520 KB SRAM.
static uint8_t cfgStage[CFG_MAX_BYTES];

uint8_t* configStageBuf() { return cfgStage; }

// ─── File helpers ─────────────────────────────────────────────────────────────

static inline uint32_t crc32Fold(uint32_t crc, const uint8_t* p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (uint8_t k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

// Validate a blob file: header well-formed, size consistent, payload CRC matches.
static bool fileValid(const char* path, ConfigBlobHeader* out) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    ConfigBlobHeader h;
    bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h)
           && h.version == CFG_VERSION
           && h.length != 0 && h.length <= CFG_MAX_BYTES
           && f.size() == sizeof(h) + h.length;

    if (ok) {
        uint8_t  chunk[256];
        uint32_t crc  = 0xFFFFFFFFu;
        uint32_t left = h.length;
        while (left) {
            uint32_t n = left < sizeof(chunk) ? left : sizeof(chunk);
            if (f.read(chunk, n) != n) { ok = false; break; }
            crc   = crc32Fold(crc, chunk, n);
            left -= n;
        }
        ok = ok && (~crc == h.crc32);
    }
    f.close();
    if (ok) *out = h;
    return ok;
}

// ─── Boot ─────────────────────────────────────────────────────────────────────

void configStoreInit() {
    g_cfg = { false, false, 0, 0, 0 };
    g_cfg.mounted = LittleFS.begin();   // formats on first use
    if (!g_cfg.mounted) return;

    ConfigBlobHeader h;
    if (!fileValid(CFG_PATH, &h)) return;
    g_cfg.valid  = true;
    g_cfg.length = h.length;
    g_cfg.seq    = h.seq;
    g_cfg.crc32  = h.crc32;
}

// ─── Core 1 flash quiesce (Core 0 side) ───────────────────────────────────────
// LittleFS idles Core 1 around each erase/program by itself, but that doorbell
// can land anywhere in Core 1's bus loop. Parking Core 1 first holds it at a
// known point for the whole write. Core 1's park loop is RAM-resident
// (core1.cpp) and keeps interrupts enabled, so LittleFS's own idle still works.

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

// Write header + staged payload to TMP_PATH. Core 1 must be parked.
static bool writeTmp(const ConfigBlobHeader& h) {
    File f = LittleFS.open(TMP_PATH, "w");
    if (!f) return false;
    bool ok = f.write((const uint8_t*)&h, sizeof(h)) == sizeof(h)
           && f.write(cfgStage, h.length) == h.length;
    f.close();
    return ok;
}

bool configStoreCommit(uint32_t len, uint32_t crc, uint8_t* nack) {
    // The caller has already verified `crc` against the staged bytes, so it is
    // not recomputed here; the readback of TMP_PATH is the remaining integrity
    // gate before the rename makes it active.
    if (machineState != STATE_IDLE && machineState != STATE_ALARM) {
        *nack = CFG_NACK_BAD_STATE; return false;
    }
    if (len == 0 || len > CFG_MAX_BYTES) { *nack = CFG_NACK_TOO_BIG; return false; }
    if (!g_cfg.mounted)                  { *nack = CFG_NACK_FLASH;   return false; }

    ConfigBlobHeader h = { CFG_VERSION, 0, g_cfg.seq + 1u, len, crc };

    // The rename is the commit point: a power cut before it leaves /config.bin
    // untouched, and LittleFS renames atomically.
    core1FlashQuiesce();
    ConfigBlobHeader back;
    bool ok = writeTmp(h)
           && fileValid(TMP_PATH, &back) && back.crc32 == crc
           && LittleFS.rename(TMP_PATH, CFG_PATH);
    if (!ok) LittleFS.remove(TMP_PATH);
    core1FlashRelease();

    if (!ok) { *nack = CFG_NACK_FLASH; return false; }

    g_cfg.valid  = true;
    g_cfg.length = len;
    g_cfg.seq    = h.seq;
    g_cfg.crc32  = crc;
    return true;
}

// ─── Read ─────────────────────────────────────────────────────────────────────

uint32_t configStoreRead(uint32_t off, uint8_t* dst, uint32_t n) {
    if (!g_cfg.valid || off >= g_cfg.length) return 0;
    if (n > g_cfg.length - off) n = g_cfg.length - off;

    File f = LittleFS.open(CFG_PATH, "r");
    if (!f) return 0;
    uint32_t got = 0;
    if (f.seek(sizeof(ConfigBlobHeader) + off)) got = f.read(dst, n);
    f.close();
    return got;
}
