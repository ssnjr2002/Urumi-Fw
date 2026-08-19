# Config Storage: Migration to LittleFS

**Status:** Cancelled 
**Replaces:** `docs/config_storage.md` (raw-flash A/B implementation)  
**Effort:** ~2 hours, net deletion of ~100 lines

---

## Why bother

The current raw-flash implementation manually reimplements slot geometry, erase/program sequencing, and A/B rotation. LittleFS is already present in the earlephilhower Arduino-Pico core and activated by the same `board_build.filesystem_size` line already in `platformio.ini`. Switching removes that machinery while keeping the wire protocol, the blob format, and the quiesce handshake unchanged.

What does **not** change: `CFG_SET`/`CFG_GET` framing, the opaque msgpack blob, the Core 1 flash quiesce handshake, and `pipeline/config_pico.py`. LittleFS does not know about the dual-core XIP constraint — Core 1 must still be parked before any write.

---

## LittleFS power-loss safety — what it actually guarantees

LittleFS is power-loss safe at the **filesystem metadata** level: directory entries, file sizes, and the journal are always consistent after a power cut. What it does **not** guarantee is file *content* mid-write — if power dies while `f.write()` is streaming 32 KB, the file exists but its content is truncated at wherever the write stopped.

| Scenario | LittleFS guarantee |
|---|---|
| Power loss between two `open()` calls | ✓ Previous file intact |
| Power loss mid `f.write()` | ✗ File exists, content truncated |
| Power loss after `f.close()` | ✓ Full file committed |

The rename trick restores the old-config-survives guarantee: write to `/config.tmp`, verify, then `LittleFS.rename("/config.tmp", "/config.bin")`. `rename` is a metadata-only operation and is atomic in LittleFS's journal — the old `/config.bin` survives until the new one is fully closed and renamed over it. The implementation below uses this approach.

---

## Keeping ConfigBlobHeader (without `magic`)

`ConfigBlobHeader` is worth retaining, minus `magic`. `magic` existed to distinguish a written slot from erased 0xFF flash — LittleFS file presence replaces that. Everything else stays useful:

```c
struct ConfigBlobHeader {
    uint32_t seq;      // monotonic write counter
    uint32_t length;   // payload byte count
    uint32_t crc32;    // CRC32 of payload — cached so CFG_GET needs no CRC scan
};
```

Written as the first 16 bytes of `/config.bin`; the msgpack payload follows immediately.

Benefits:
- **`seq`** — `status cfg` can confirm a write incremented the counter without a power cycle.
- **`crc32` cached** — `CFG_GET` reads `g_cfg.crc32` directly; no file scan needed also can be validated on boot
- **`length` vs `file.size() - 16`** — cross-check on boot and after readback.

File layout:
```
/config.bin
  [0..15]   ConfigBlobHeader  (16 bytes)
  [16..]    msgpack payload   (header.length bytes)
```

---

## Comparison with current implementation

| | Current (raw flash) | LittleFS |
|---|---|---|
| `CFG_GET` streaming | XIP pointer, zero copy | File read → 256B chunk buffer → Serial |
| `CFG_SET` write path | Manual erase + program loop | `f.write()` |
| Boot CRC scan | Full payload scan from XIP | Full payload scan via file API |
| Power-loss safety | A/B slots, old config survives | Rename trick, old config survives |
| Wear levelling | Round-robin between 2 fixed slots | Spread across full 128 KB region |
| Code to maintain | ~150 lines | ~50 lines |
| Flash geometry bugs | Your problem | LittleFS's problem |

**CFG_GET** is the one place the current implementation has a concrete edge: `g_cfg.addr`
is an XIP pointer so `Serial.write(g_cfg.addr, len)` is zero-copy. Under LittleFS
the data goes flash → RAM chunk buffer → Serial. At 115200 baud the bottleneck is
the UART, so this makes no practical difference.

**Boot scan** is equivalent: both implementations do a full payload CRC pass on
boot. The current one reads from XIP; LittleFS reads through the file API. Same
cost, different path.

**Wear levelling** is a genuine structural advantage for LittleFS. The current
two-slot scheme concentrates all writes on two fixed 36 KB spans. LittleFS spreads
writes across the full 128 KB reservation — ~3.5× better effective endurance for
the same write frequency. Irrelevant for rarely-changed machine config, but real.

---

## What gets deleted

| Symbol | Fate |
|---|---|
| `CFG_SECTOR`, `CFG_SLOT_BYTES`, `CFG_NUM_SLOTS`, `CFG_REGION_BYTES`, `CFG_HEADER_BYTES`, `CFG_PAYLOAD_SECTORS` | Delete — geometry is LittleFS's problem |
| `ConfigBlobHeader.magic` | Delete — file presence replaces it |
| `slotValid()`, `cfgRegionOff()`, `cfgRegionOk()` | Delete |
| `g_cfg.slot`, `g_cfg.addr` | Delete |
| A/B boot scan loop | Delete |

`ConfigCache` keeps `length`, `seq`, and adds `crc32` (read from header on boot).

---

## Replacement implementation

### `config_store.h`

```cpp
#define CFG_FILENAME      "/config.bin"
#define CFG_MAX_BYTES     32768u
#define CFG_HEADER_BYTES  16u
#define CFG_VERSION       1u

struct ConfigBlobHeader {
    uint16_t version;
    uint16_t _rsvd;
    uint32_t seq;
    uint32_t length;
    uint32_t crc32;
};

struct ConfigCache {
    uint32_t length;   // 0 = no config stored
    uint32_t seq;
    uint32_t crc32;
};
extern ConfigCache g_cfg;

void     configStoreInit();
uint8_t* configStageBuf();
bool     configStoreCommit(uint32_t len, uint32_t crc, uint8_t* nack);
```

### `config_store.cpp`

```cpp
#include <LittleFS.h>
#include "../shared.h"
#include "config_store.h"
#include "../../include/common.h"

ConfigCache g_cfg = { 0, 0, 0 };

static uint8_t cfgStage[CFG_MAX_BYTES];

uint8_t* configStageBuf() { return cfgStage; }

void configStoreInit() {
    if (!LittleFS.begin()) return;
    File f = LittleFS.open(CFG_FILENAME, "r");
    if (!f) return;

    ConfigBlobHeader hdr;
    if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
        hdr.version != CFG_VERSION ||
        hdr.length  == 0 || hdr.length > CFG_MAX_BYTES ||
        (uint32_t)f.size() != CFG_HEADER_BYTES + hdr.length) {
        f.close(); return;
    }

    // Full payload CRC verify — same cost as current slotValid()
    uint32_t actual = 0xFFFFFFFFu;
    uint8_t buf[256]; int n;
    while ((n = f.read(buf, sizeof(buf))) > 0)
        for (int i = 0; i < n; i++)
            actual = crc32Byte(actual, buf[i]);
    f.close();

    if (~actual != hdr.crc32) return;   // corrupted — boot with no config

    g_cfg = { hdr.length, hdr.seq, hdr.crc32 };
}

bool configStoreCommit(uint32_t len, uint32_t crc, uint8_t* nack) {
    if (len == 0 || len > CFG_MAX_BYTES)
        { *nack = CFG_NACK_TOO_BIG;   return false; }
    if (machineState != STATE_IDLE && machineState != STATE_ALARM)
        { *nack = CFG_NACK_BAD_STATE; return false; }

    ConfigBlobHeader hdr = { CFG_VERSION, 0, g_cfg.seq + 1, len, crc };

    core1FlashQuiesce();

    // Write to tmp first — rename is atomic, so old config survives a power cut
    File f = LittleFS.open("/config.tmp", "w");
    if (!f) { core1FlashRelease(); *nack = CFG_NACK_FLASH; return false; }
    f.write((const uint8_t*)&hdr, sizeof(hdr));
    f.write(cfgStage, len);
    f.close();

    // Readback verify against the tmp file before committing
    File v = LittleFS.open("/config.tmp", "r");
    bool ok = v && ((uint32_t)v.size() == CFG_HEADER_BYTES + len);
    if (ok) {
        v.seek(CFG_HEADER_BYTES);
        uint32_t actual = 0xFFFFFFFFu;
        uint8_t buf[256]; int n;
        while ((n = v.read(buf, sizeof(buf))) > 0)
            for (int i = 0; i < n; i++)
                actual = crc32Byte(actual, buf[i]);
        ok = (~actual == crc);
    }
    if (v) v.close();

    if (!ok) {
        LittleFS.remove("/config.tmp");
        core1FlashRelease();
        *nack = CFG_NACK_FLASH;
        return false;
    }

    LittleFS.rename("/config.tmp", CFG_FILENAME);   // atomic commit
    core1FlashRelease();

    g_cfg = { len, hdr.seq, crc };
    return true;
}
```

### `handleCfgGet()` in `data_plane.cpp`

Cached CRC in `g_cfg` makes this a single streaming pass — no scan needed:

```cpp
static void handleCfgGet() {
    uint32_t len = g_cfg.length;
    uint32_t crc = g_cfg.crc32;
    Serial.write(CFG_DATA);
    Serial.write((const uint8_t*)&len, 4);
    Serial.write((const uint8_t*)&crc, 4);
    if (len == 0) return;

    File f = LittleFS.open(CFG_FILENAME, "r");
    if (!f) return;
    f.seek(CFG_HEADER_BYTES);
    uint8_t buf[256]; int n;
    while ((n = f.read(buf, sizeof(buf))) > 0)
        Serial.write(buf, n);
    f.close();
}
```

---

## What stays the same

- `board_build.filesystem_size = 128k` in `platformio.ini` — unchanged, now used for real by LittleFS
- Core 1 quiesce handshake (`core1FlashQuiesce` / `core1FlashRelease`) — unchanged
- `CFG_SET` / `CFG_GET` wire framing — unchanged
- `cfgStage[CFG_MAX_BYTES]` static buffer — stays (LittleFS writes from it)
- `pipeline/config_pico.py` — unchanged, zero host-side impact
- `status cfg` — loses `slot` and `addr`; keeps `seq` and `len`; can add `crc`

---

## Migration steps

1. Add `LittleFS.h` include to `config_store.cpp`.
2. Replace `config_store.cpp` with the implementation above.
3. Update `config_store.h` — remove geometry constants, drop `magic` from `ConfigBlobHeader`, update `ConfigCache` (drop `slot`/`addr`, add `crc32`).
4. Move `crc32Byte` from `data_plane.cpp` to `common.h` (alongside `crc8`) — needed by both `config_store.cpp` and `data_plane.cpp`.
5. Update `handleCfgGet()` in `data_plane.cpp` — read from file, use cached CRC.
6. Update `status cfg` in `control_plane.cpp` — remove `slot`/`addr`.
7. Update `config_storage.md` — replace flash layout diagram and slot geometry with file layout; keep wire protocol sections unchanged.
8. Build and verify: `status cfg` shows no config on fresh flash; `store` + `get` round-trip via `config_pico.py`; power-cycle retains config; `seq` increments on each write.

---

## Risks

- **First `LittleFS.begin()` on a board with the old raw A/B layout** will format the region, erasing any stored config. Expected on a migration flash; not a concern for fresh boards.
- **`crc32Byte` visibility** — currently a static in `data_plane.cpp`. Must move to `common.h` as part of this migration.

---

## Ideas

- Maybe have a constant `CFG_NAME` and we can just set the file extension appropriately, i.e. actual file is CFG_NAME.bin while temp file is CFG_NAME.tmp

