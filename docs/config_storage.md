# Config Blob Storage

**Status:** Implemented (firmware). Host sender + on-hardware bring-up pending.
**Scope:** storage and retrieval of one opaque config blob on the RP2350.
**Cross-ref:** USB framing conventions — [wire_protocol.md](wire_protocol.md).

The Pico stores **one opaque blob** (≤32 KB) supplied by the host. The host
converts its JSON machine config to msgpack; the firmware stores the bytes
**verbatim** — it does not parse them (today). A future firmware may add a
msgpack reader to consume fields (steps/unit, axis mask, …); the storage layer is
built so that can be added without touching it (the active blob sits at a stable
XIP address a reader can walk in place).

---

## 1. Flash layout

The store lives in a linker-reserved span at the top of flash, carved via
`board_build.filesystem_size = 128k` (platformio.ini). Reserving through the
filesystem mechanism **shrinks the program region** (`__FLASH_LENGTH__`), so the
linker provably cannot place program code in our span. We never mount LittleFS —
the span is repurposed as raw config flash. It is addressed through the
`_FS_start` / `_FS_end` linker symbols, never a hardcoded offset, so it follows
flash-size changes automatically.

```
flash top ┌────────────────────────┐ 0x10400000
          │ EEPROM emulation (4 KB) │           earlephilhower reserves this
          ├────────────────────────┤ _FS_end   0x103FF000
          │ (spare, ~56 KB)        │
          │ ┌────────────────────┐ │
          │ │ Slot B   (36 KB)   │ │
          │ ├────────────────────┤ │
          │ │ Slot A   (36 KB)   │ │
          │ └────────────────────┘ │ _FS_start 0x103DF000
          ├────────────────────────┤
          │ program (XIP)          │
          └────────────────────────┘ 0x10000000
```

Two **A/B slots** so a write leaves the previous config intact until the new one
is fully committed (see §3). Each slot:

```
slot ┌──────────────────────────┐
     │ header  (1 sector, 4 KB)  │  20-byte ConfigBlobHeader in the first page
     ├──────────────────────────┤
     │ payload (8 sectors, 32 KB)│  the blob, byte-for-byte
     └──────────────────────────┘   = 36 KB
```

Constants: `CFG_SECTOR=4096`, `CFG_SLOT_BYTES=36 KB`, `CFG_NUM_SLOTS=2`,
`CFG_REGION_BYTES=72 KB`, `CFG_MAX_BYTES=32768`.

### ConfigBlobHeader (20 bytes)

| Field | Type | Meaning |
|---|---|---|
| `magic` | u32 | `'CBLB'` (0x424C4243). Distinguishes a written slot from erased 0xFF flash. |
| `version` | u16 | Storage-format version (=1). Governs future header layout; **not** the payload's format. |
| `_rsvd` | u16 | Alignment padding. |
| `seq` | u32 | Monotonic. The higher valid `seq` is the active slot. |
| `length` | u32 | Payload byte count (1..`CFG_MAX_BYTES`). |
| `crc32` | u32 | CRC32 over `payload[0..length)`. |

The 20-byte struct is written into the first flash **page** (256 B) of the slot,
padded with 0xFF.

---

## 2. Boot

`configStoreInit()` runs once in `setup()` (pure flash reads — no core parking):

1. Read both slot headers directly from XIP.
2. A slot is **valid** iff `magic == 'CBLB'` **and** `version == 1` **and**
   `1 ≤ length ≤ CFG_MAX_BYTES` **and** `crc32(payload, length) == header.crc32`.
3. Active slot = the valid slot with the higher `seq`. Cache
   `{addr, length, seq, slot}` in `g_cfg`.
4. If neither is valid → **no config** (`g_cfg` empty, `slot = -1`).

**No-config policy: permissive.** An empty store is a normal state; the machine
boots to IDLE and runs. Job streams are *not* blocked on config presence (the
blob is opaque, so the firmware has no motion-critical reason to require it).
Gating would be added later, together with a firmware msgpack reader.

`g_cfg` is a RAM handle updated **only** by boot scan and a successful commit. It
is deliberately **not** cleared by soft reset — the blob lives in flash, which
soft reset does not touch; re-zeroing it would blank a valid config from RAM.

---

## 3. Write (commit) — power-safe A/B

`configStoreCommit(len, crc, &nack)` (called by the data-plane receiver once a
transfer completes):

1. Reject unless `machineState` is IDLE or ALARM (`CFG_NACK_BAD_STATE`).
2. Reject `len == 0 || len > CFG_MAX_BYTES` (`CFG_NACK_TOO_BIG`).
3. `crc` is **trusted** here (the receiver already verified it against the staged
   bytes incrementally — §5). It is not recomputed.
4. Pick the **inactive** slot; new `seq = active.seq + 1` (or slot 0 / seq 1 if
   none).
5. **Quiesce Core 1** (§4), disable IRQs.
6. Erase the inactive slot → program payload → **program header LAST**.
7. Restore IRQs, release Core 1.
8. **Readback-verify**: `crc32(flash_payload, len) == crc`? On mismatch →
   `CFG_NACK_FLASH`, leave the old slot active.
9. On success, atomically swap `g_cfg` to the new slot.

**Why header-last is the commit point:** if power fails after the payload but
before the header, the new slot has an invalid/absent header → fails the boot
scan → the old slot (higher-until-now `seq`) still wins. A write is therefore
all-or-nothing: an interrupted write can never leave the machine with *no* valid
config. This is the whole reason for two slots.

CRC is touched twice per write: once **free** (folded during receive, §5) and
once on **readback** (a real 32 KB pass from flash). Reads never CRC.

---

## 4. Core 1 flash quiesce

A flash erase/program stalls XIP for **both** cores. Core 1 runs time-critical
step timing from XIP, so it must stop executing from flash during a write. This
uses a **dedicated handshake, separate from soft reset** — a config write must
preserve machine state (position/homing), so it cannot reuse the soft-reset
path (which wipes everything).

- Flags: `flash_op_requested` (Core 0 → Core 1), `core1_parked_for_flash` (ack).
- Core 0: set request → spin until ack → IRQs off → flash ops → IRQs on → clear
  request → spin until ack clears.
- Core 1: at the top of its exec loop, if `flash_op_requested`, enter
  `core1FlashPark()` — a **RAM-resident** (`__not_in_flash_func`) spin. It *must*
  be in RAM: a flash-resident spin would fault when XIP is down.

Only asserted in IDLE/ALARM, where Core 1 is idle between segments — never
mid-motion.

---

## 5. Wire framing (USB CDC)

Two opcodes, dispatched by leading magic byte (bit 7 set, disjoint from the
lowercase-ASCII control plane). See [wire_protocol.md](wire_protocol.md) for the
shared dispatch rules.

Response opcodes are **dedicated** magics (no reuse of the MSEG `0xAA`/`0xBB`):

| Magic | Value | Direction | Meaning |
|---|---|---|---|
| `CFG_SET_MAGIC` | `0xB0` | H→P | Config write — header, then payload |
| `CFG_GET_MAGIC` | `0xB1` | H→P | Request the active blob |
| `CFG_RDY` | `0xB2` | P→H | Header accepted — send payload |
| `CFG_ACK` | `0xB3` | P→H | Blob committed |
| `CFG_NACK` | `0xB4` | P→H | Rejected — next byte is the reason |
| `CFG_DATA` | `0xB5` | P→H | CFG_GET response header |

### CFG_SET — `0xB0` (host → Pico): store a blob, **two-phase**

**Phase 1 — header (host → Pico), 9 bytes:**
```
[0]      magic  = 0xB0
[1..4]   length u32 LE   (1..32768)
[5..8]   crc32  u32 LE   (host's CRC32 of the payload)
```
Pico validates `length` (size) and machine state (§6) *before any payload*:
- OK  → replies `CFG_RDY` (0xB2). Host may now stream the payload.
- Bad → replies `CFG_NACK` (0xB4) + reason. Transaction ends; host sends no payload.

**Phase 2 — payload (host → Pico), on CFG_RDY:**
```
[0..]    payload  <length> bytes
```
The receiver stages the payload into a 32 KB RAM buffer, folding CRC32
**incrementally** as bytes arrive (one pass — the transfer-integrity check),
then calls `configStoreCommit`. Final reply:
- `CFG_ACK` (0xB3) — stored; `g_cfg` now serves the new blob.
- `CFG_NACK` (0xB4) + reason — rejected (CRC / state / flash).

Because the host waits for `CFG_RDY` before sending payload, an early phase-1
NACK can never desync the byte stream. A stalled transfer (no byte within
`CFG_RX_TIMEOUT_MS`, 2 s) is aborted with `CFG_NACK` + `CFG_NACK_TIMEOUT`.

### CFG_GET — `0xB1` (host → Pico): read the active blob

Answered inline (no receive state). Reply:
```
[0]      magic  = 0xB5   (CFG_DATA)
[1..4]   length u32 LE   (0 = no config stored)
[5..8]   crc32  u32 LE   (CRC32 of payload; 0 when length 0)
[9..]    payload  <length> bytes   (from XIP; omitted when length 0)
```

`length == 0` means **no config** — a successful read of an empty store, **not**
an error (so there is no NACK-on-empty; absence is data, not failure).

### CFG NACK reasons

| Constant | Value | Meaning |
|---|---|---|
| `CFG_NACK_CRC` | `0x01` | Incremental CRC32 ≠ host-declared CRC (corrupt transfer). |
| `CFG_NACK_TOO_BIG` | `0x02` | `length` is 0 or > `CFG_MAX_BYTES` (phase 1). |
| `CFG_NACK_BAD_STATE` | `0x03` | Machine not in IDLE/ALARM (phase 1, re-checked at commit). |
| `CFG_NACK_FLASH` | `0x04` | Post-flash readback verify failed (or region too small). |
| `CFG_NACK_TIMEOUT` | `0x05` | Transfer stalled — no byte within `CFG_RX_TIMEOUT_MS`. |

CRC is standard reflected CRC-32 (poly 0xEDB88320) — verified against zlib's
canonical `0xCBF43926`.

---

## 6. Allowed states

| | IDLE | RUNNING | PAUSED | ALARM | HOMING |
|---|---|---|---|---|---|
| CFG_GET | ✓ | ✓ | ✓ | ✓ | ✓ |
| CFG_SET (commit) | ✓ | ✗ | ✗ | ✓ | ✗ |

CFG_SET in ALARM is the config-recovery path (a stopped state is safe to write
in). Committing is blocked in RUNNING/PAUSED because a flash write stalls XIP and
would disrupt Core 1 step timing.

---

## 7. Known limitations / rough edges

Deliberate simplifications for the current phase, flagged so they are not
mistaken for oversights:

1. **CFG_GET streams up to 32 KB in one blocking write.** During a GET, Core 0
   blocks in its ingest loop until the USB TX buffer drains the whole payload. It
   does not touch flash or Core 1, so motion is unaffected — but Core 0 will not
   service other serial (including STATUS_REQ) until it finishes. GET is best
   treated as an idle-time operation.

2. **CFG_GET recomputes the payload CRC each call.** Deliberate (avoids storing
   the CRC in `g_cfg`); fine because GET is a one-shot fetch, not polled. If the
   host ever polls GET, cache the CRC in `ConfigCache`.

Earlier rough edges — receive-then-reject waste, no transfer timeout, and the
mid-transfer desync risk on a bad length — were removed by the two-phase CFG_SET
(§5): size/state are rejected in phase 1 before any payload, the host only sends
payload after `CFG_RDY`, and `dataPlaneTick()` enforces the inter-byte timeout.

---

## 8. Control-plane inspection

`status cfg` (text command, always available) prints the active slot metadata from
`g_cfg` without touching flash:

```
cfg slot=0 seq=1 len=32768 addr=0x103df000
```

| Field | Meaning |
|---|---|
| `slot` | Active flash slot (0 or 1), or `none` if no config stored |
| `seq` | Monotonic write counter — increments on every successful `CFG_SET` |
| `len` | Payload byte count of the active blob |
| `addr` | XIP address of the payload — useful for verifying slot geometry |

This is a human-readable diagnostic, not a host-facing binary response. The binary
path for reading the blob is `CFG_GET` (§5).

---

## 9. Open questions for review

- **Future config parsing.** When a firmware msgpack reader lands, the boot scan
  and commit should additionally *apply* the blob (parse into working structs),
  and the no-config policy may move from permissive to gated. The storage layer
  does not change.
