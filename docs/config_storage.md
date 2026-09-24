# Config Blob Storage

**Status:** Implemented (firmware and host sender, `Link.pushConfig` /
`Link.pullConfig`). On-hardware bring-up pending.
**Scope:** storage and retrieval of one opaque config blob on the RP2350.
**Cross-ref:** USB framing conventions — [wire_protocol.md](wire_protocol.md).

The Pico stores **one blob** (≤32 KB) supplied by the host: the resolved
machine config as msgpack with a payload schema version `v`
(`web/src/machine/json/blob.ts`). The store keeps the bytes **verbatim**;
`config/config_decode.*` decodes the fields the Pico consumes into a
`MachineCfg` at boot and before every commit (`config/machine_cfg.*` holds the
active one). See docs/plans/pico-config.md for the design.

---

## 1. Flash layout

The store is a LittleFS filesystem in a linker-reserved span at the top of
flash, carved via `board_build.filesystem_size = 128k` (platformio.ini).
Reserving through the filesystem mechanism **shrinks the program region**
(`__FLASH_LENGTH__`), so the linker provably cannot place program code in it.

```
flash top ┌────────────────────────┐ 0x10400000
          │ EEPROM emulation (4 KB) │           earlephilhower reserves this
          ├────────────────────────┤ _FS_end   0x103FF000
          │ LittleFS (128 KB)      │
          │   /config.bin          │
          │   /config.tmp (during  │
          │    a write only)       │
          ├────────────────────────┤ _FS_start 0x103DF000
          │ program (XIP)          │
          └────────────────────────┘ 0x10000000
```

LittleFS spreads writes across the whole span (wear levelling) and keeps its
metadata consistent across power loss.

### /config.bin

```
[0..15]   ConfigBlobHeader  (16 bytes)
[16..]    payload           (header.length bytes, the blob byte-for-byte)
```

| Field | Type | Meaning |
|---|---|---|
| `version` | u16 | File-header format version (=1). **Not** the payload's format. |
| `_rsvd` | u16 | Alignment padding. |
| `seq` | u32 | Monotonic write counter. |
| `length` | u32 | Payload byte count (1..`CFG_MAX_BYTES`). |
| `crc32` | u32 | CRC32 over the payload. |

`CFG_MAX_BYTES = 32768`.

---

## 2. Boot

`configStoreInit()` runs once in `setup()`:

1. Mount LittleFS. `LittleFS.begin()` formats the span if it holds no
   filesystem (first boot, or a span left by the older raw A/B store — that
   config is lost and must be pushed again).
2. `/config.bin` is **valid** iff `version == 1` **and**
   `1 ≤ length ≤ CFG_MAX_BYTES` **and** file size `== 16 + length` **and** the
   payload CRC32 matches the header.
3. Valid → cache `{length, seq, crc32}` in `g_cfg`. Otherwise → **no config**.

`machineCfgLoad()` then decodes the stored blob. A blob that is missing or does
not decode leaves the active config invalid.

**No-config policy: gated.** Without a valid config the machine sits in
`ALARM_CONFIG` and refuses all motion; an accepted `CFG_SET` is the way out.
With one, the controller commits the config's `defaultHead` axis map after
every soft reset (docs/engage_and_axis_map.md §6).

`g_cfg` is updated **only** by the boot check and a successful commit. It is
deliberately **not** cleared by soft reset — the blob lives in flash, which soft
reset does not touch.

---

## 3. Write (commit) — power-safe rename

Once a transfer completes and its CRC matches, the data-plane receiver first
decodes the staged blob (`machineCfgStage`). A blob that does not decode or
fails validation is refused with `CFG_NACK_SCHEMA` and never reaches flash.
Then `configStoreCommit(len, crc, &nack)`:

1. Reject unless `machineState` is IDLE or ALARM (`CFG_NACK_BAD_STATE`).
2. Reject `len == 0 || len > CFG_MAX_BYTES` (`CFG_NACK_TOO_BIG`).
3. `crc` is **trusted** here (the receiver already verified it against the staged
   bytes incrementally — §5). It is not recomputed.
4. **Park Core 1** (§4).
5. Write header (`seq = active.seq + 1`) + payload to `/config.tmp`.
6. **Readback-verify** `/config.tmp` with the same check as boot.
7. Rename `/config.tmp` over `/config.bin`.
8. Release Core 1. On any failure in 5–7 → remove `/config.tmp`,
   `CFG_NACK_FLASH`, the old config stays active.
9. On success, update `g_cfg`.

After a successful commit the decoded config becomes active
(`machineCfgAdopt`) and the controller re-commits the `defaultHead` axis map
from it before `CFG_ACK` is sent. A node that does not answer leaves
`ALARM_NODE_FAULT`, not a NACK: the config itself was stored.

**Why the rename is the commit point:** LittleFS renames atomically. A power cut
before it leaves `/config.bin` untouched (a stray `/config.tmp` is ignored and
overwritten by the next write); after it, the new file is complete and verified.
An interrupted write can never leave the machine with *no* valid config.

---

## 4. Core 1 flash quiesce

A flash erase/program stalls XIP for **both** cores. Core 1 runs time-critical
step timing from XIP, so it must stop executing from flash during a write.

Two layers:

- **LittleFS's own idle.** Around each erase/program, the library disables Core 0
  interrupts and idles Core 1 through a doorbell interrupt whose handler spins in
  RAM (`rp2040.idleOtherCore()`). This works because Core 1 is started by the
  framework (`setup1`) and keeps interrupts enabled.
- **The park handshake, around the whole write.** The doorbell can land anywhere
  in Core 1's bus loop, so Core 0 first parks Core 1 at a known point:
  `flash_op_requested` (Core 0 → Core 1), `core1_parked_for_flash` (ack). Core 1
  checks the request at the top of its exec loop and enters `core1FlashPark()`, a
  **RAM-resident** (`__not_in_flash_func`) spin. This is separate from soft reset
  — a config write must preserve machine state (position/homing).

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
[5..8]   crc32  u32 LE   (the verified CRC32 from g_cfg; 0 when length 0)
[9..]    payload  <length> bytes   (streamed from /config.bin; omitted when length 0)
```

`length == 0` means **no config** — a successful read of an empty store, **not**
an error (so there is no NACK-on-empty; absence is data, not failure).

A read failure after the header has been sent cannot be reported in-band, so the
payload is zero-padded to `length` and the host's CRC check rejects it.

### CFG NACK reasons

| Constant | Value | Meaning |
|---|---|---|
| `CFG_NACK_CRC` | `0x01` | Incremental CRC32 ≠ host-declared CRC (corrupt transfer). |
| `CFG_NACK_TOO_BIG` | `0x02` | `length` is 0 or > `CFG_MAX_BYTES` (phase 1). |
| `CFG_NACK_BAD_STATE` | `0x03` | Machine not in IDLE/ALARM (phase 1, re-checked at commit). |
| `CFG_NACK_FLASH` | `0x04` | Filesystem not mounted, write failed, or readback verify failed. |
| `CFG_NACK_TIMEOUT` | `0x05` | Transfer stalled — no byte within `CFG_RX_TIMEOUT_MS`. |
| `CFG_NACK_SCHEMA` | `0x06` | Blob did not decode or failed validation (`config_decode.h`). |

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

## 7. Known limitations

**CFG_GET streams up to 32 KB in one blocking loop.** During a GET, Core 0
blocks in its ingest loop until the USB TX buffer drains the whole payload. It
does not write flash or touch Core 1, so motion is unaffected — but Core 0 will
not service other serial (including STATUS_REQ) until it finishes. GET is best
treated as an idle-time operation.

---

## 8. Control-plane inspection

`status cfg` (text command, always available) prints the active blob metadata
from `g_cfg` and the decode result, without touching flash:

```
cfg seq=1 len=1374 crc=0x1c291ca3 schema=1
```

| Output | Meaning |
|---|---|
| `cfg fs=unmounted` | LittleFS failed to mount; CFG_SET NACKs with `CFG_NACK_FLASH` |
| `cfg none` | No valid `/config.bin` |
| `seq` | Monotonic write counter — increments on every successful `CFG_SET` |
| `len` | Payload byte count of the active blob |
| `crc` | Payload CRC32 |
| `schema` | Payload schema version of the decoded active config |
| `decoded=0` | The stored blob is intact but did not decode (`ALARM_CONFIG`) |
| `rejected=<name>` | The most recent rejection, at boot or of a `CFG_SET` (`configDecodeErrorName`) |

This is a human-readable diagnostic, not a host-facing binary response. The binary
path for reading the blob is `CFG_GET` (§5).
