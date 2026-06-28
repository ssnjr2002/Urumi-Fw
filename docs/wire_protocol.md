# Wire Protocol

**Branch:** `phase1-impl`
**Date:** 2026-06-28
**Status:** Phase 1 contract FROZEN — data plane (MSEG/JOG/MCFG, ACK/NACK) and control plane (text) defined below. Config commands (CMD_GET_CONFIG, CMD_SET_CONFIG, CMD_HANDSHAKE) deferred to Phase 2.

Single source of truth for all USB CDC framing between host and Pico.
All other docs cross-reference here rather than defining constants.

## Two planes on one USB pipe

Phase 1 splits the host↔Pico link into two planes that share the USB CDC pipe:

- **Data plane — binary, magic-dispatched.** High-rate streaming: MSEG, JOG,
  the MCFG preamble. Pico replies ACK/NACK. Each binary packet is a fixed
  length keyed by its magic byte.
- **Control plane — text lines.** Low-rate commands the operator/host issues:
  `getstate`, `getpos`, `enable`, `disable`, `pause`, `resume`, `cancel`,
  `setorigin`, `stop`, `unalarm`, `ping`, `pingnode`. One command per line,
  `\n`-terminated; the Pico replies with a text line. This matches the existing
  Core 0 text CLI (`move`/`stop`/`ping`/`enable`/`disable`/`getpos`).

**Dispatch rule (Core 0 ingest):** at a packet boundary, peek the first byte. If
it is a known data-plane magic, read the whole fixed-length binary packet. Any
other byte begins a text line, read to `\n`. Packets and lines are **atomic** —
never interleaved — so a control command issued mid-stream (e.g. `pause` during
RUNNING) is recognised at the next boundary between MSEG packets, not byte-wise
inside one. The data-plane magics all have bit 7 set (0xA?/0xB?) and the text
commands are lowercase ASCII, so the two never collide at a boundary.

---

## Magic Bytes — Packet Type Dispatch (data plane)

| Constant | Value | Direction | Description |
|---|---|---|---|
| `MSEG_MAGIC` | `0xAB` | Host → Pico | MicroSegment — pre-computed step event |
| `JOG_MAGIC`  | `0xAE` | Host → Pico | Jog packet (separate from MSEG) |
| `TILE_MAGIC` | `0xAD` | Host → Pico | SplineTile — local production (future) |
| `TOOL_MAGIC` | `0xAC` | Host → Pico | ToolConfig — local production (future) |
| `MCFG_MAGIC` | `0x4D434647` (4B "MCFG") | Host → Pico | Job stream preamble (required_axes + config CRC32 in Phase 2) |
| `MSEG_ACK`   | `0xAA` | Pico → Host | ACK response |
| `MSEG_NACK`  | `0xBB` | Pico → Host | NACK response |

All single-byte magics have bit 7 set, keeping them disjoint from the lowercase
ASCII that begins every control-plane line.

---

## Packet Formats

### MicroSegment — `0xAB` (26 bytes)
```
[0]      magic = 0xAB
[1..4]   dx        int32 LE   — X axis steps
[5..8]   dy        int32 LE   — Y axis steps
[9..12]  dz        int32 LE   — Z axis steps
[13..16] da        int32 LE   — A axis steps
[17..20] interval  uint32 LE  — step interval in CPU cycles
[21]     flags     uint8      — MSEG_FLAG_* bitmask
[22]     seq       uint8      — rolling duplicate guard (reset by seqreset)
[23..24] pad       uint8[2]
[25]     CRC8 over bytes [0..24]
```

### Job Stream Preamble — `0x4D434647` (Phase 1: 6 bytes, Phase 2: 10 bytes)

Must precede any MSEG packets in a job stream.

**Phase 1 layout** (Pico uses `required_axes` only; `config_crc32` reserved/ignored):
```
[0..3]   magic         = 0x4D434647 "MCFG"
[4]      version       uint8      — struct version
[5]      required_axes uint8      — tool axis mask (bit0=X bit1=Y bit2=Z bit3=A)
```

**Phase 2 layout** (Pico validates config CRC32 before accepting stream):
```
[0..3]   magic         = 0x4D434647 "MCFG"
[4]      version       uint8
[5]      required_axes uint8
[6..9]   config_crc32  uint32 LE  — CRC32 of MachineConfigFlash on Pico flash
```
Phase 2: Pico compares `config_crc32` against stored flash CRC32; mismatch →
NACK `NACK_STREAM_CONFIG_MISMATCH`.

### Jog Packet — `JOG_MAGIC` (0xAE, 26 bytes)
```
[0]      magic = 0xAE
[1..4]   dx        int32 LE   — X axis steps
[5..8]   dy        int32 LE   — Y axis steps
[9..12]  dz        int32 LE   — Z axis steps
[13..16] da        int32 LE   — A axis steps
[17..20] interval  uint32 LE  — step interval in CPU cycles
[21]     flags     uint8      — MSEG_FLAG_* bitmask (PATH_END terminates a jog burst)
[22]     jogSeq    uint8      — 1-byte rolling duplicate guard (independent of MSEG seq)
[23..24] pad       uint8[2]
[25]     CRC8 over bytes [0..24]
```
Same 26-byte layout as MSEG (so one parser serves both), differing only in the
magic and in byte [22] carrying `jogSeq` instead of the stream `seq`. Accepted
in `STATE_IDLE` and `STATE_PAUSED`. No seqnum window — window-1 fire-and-wait;
jogSeq provides duplicate rejection only. A jog burst (host-computed move, e.g.
the return to `pausePos`) is one or more jog packets ending with
`MSEG_FLAG_PATH_END`; the Pico runs `runningReason = JOG` while emitting and
returns to its prior state (IDLE, or PAUSED when `PausedJobContext.active`) when
the burst drains.

### ACK — `0xAA` (3 bytes)
```
[0]      0xAA
[1]      seq_lo   uint8   — rolling ACK echo (lo byte)
[2]      seq_hi   uint8   — rolling ACK echo (hi byte)
```

### NACK — `0xBB` (3 bytes)
```
[0]      0xBB
[1]      reason   uint8   — see NACK reason tables below
[2]      0x00
```

### CMD_GET_CONFIG response (Pico → Host) *(Phase 2)*
```
MachineConfigFlash binary struct + CRC32 (4B LE) appended
```
CRC32 is the stored flash checksum — the same value embedded in the Phase 2
MCFG header and used for connect-time handshake comparison.

### CMD_SET_CONFIG (Host → Pico) *(Phase 2)*
```
MachineConfigFlash binary struct + CRC32 (4B LE)
```

### CMD_HANDSHAKE response (Pico → Host) *(Phase 2)*
```
[0..1]   firmware_version  uint16 LE
[2..5]   config_crc32      uint32 LE  — stored flash CRC32
[6]      machineState      uint8
[7]      axes_homed        uint8      — bitmask
```
Extensible — future fields appended; version field governs layout.

---

## MSEG Flags (`flags` byte in MicroSegment)

| Constant | Value | Description |
|---|---|---|
| `MSEG_FLAG_NONE` | `0x00` | No flags |
| `MSEG_FLAG_PATH_END` | `0x01` | Last segment in path — Core 1 signals idle |
| `MSEG_FLAG_ESTOP` | `0x02` | Poison pill — flush and halt immediately |
| `MSEG_FLAG_PAUSE` | `0x04` | Predetermined pause point — Core 1 drains and enters PAUSED |

---

## NACK Reason Bytes

Two independent namespaces — MSEG stream NACKs and config command NACKs.
The reason byte meaning depends on which command the NACK is responding to.

### MSEG Stream NACK reasons (responses to MSEG / jog / job stream packets)

| Constant | Value | Description |
|---|---|---|
| `MSEG_NACK_CRC` | `0x01` | CRC8 mismatch — packet corrupt |
| `MSEG_NACK_FULL` | `0x02` | Ring buffer full — backpressure |
| `MSEG_NACK_MAGIC` | `0x03` | Unrecognised magic byte |
| `MSEG_NACK_PAUSED` | `0x04` | Stream rejected — machine is PAUSED |
| `MSEG_NACK_CONFIG_MISMATCH` | `0x05` | MCFG header CRC32 disagrees with Pico flash |
| `MSEG_NACK_BAD_STATE` | `0x06` | Command rejected — wrong machine state |

### Config command NACK reasons (responses to CMD_SET_CONFIG) *(Phase 2)*

| Constant | Value | Description |
|---|---|---|
| `NACK_CONFIG_CRC` | `0x01` | CRC32 mismatch — struct corrupt in transit |
| `NACK_CONFIG_INVALID` | `0x02` | Semantic validation failed (zero steps_per_unit, zero f_cpu, node id out of range, duplicate node ids, zero max_travel on present axis) |
| `NACK_CONFIG_VERSION` | `0x03` | Struct version unknown |
| `NACK_CONFIG_BAD_STATE` | `0x04` | Push rejected — machine not in IDLE or ALARM |

---

## Control Plane — Text Commands (Phase 1)

One command per line, `\n`-terminated, lowercase ASCII. The Pico replies with a
single text line. Arguments are space-separated. Numbers are decimal unless
prefixed `0x`.

| Command | Args | Reply | Meaning |
|---|---|---|---|
| `ping` | — | `pong` | Is the Pico alive (USB link)? |
| `pingnode` | `<id>` | `node <id> ok` / `node <id> timeout` | Relay an RS485 CMD_PING to a bus node; report presence |
| `getstate` | — | `state=<s> homed=<hex> alarm=<a> running=<r>` | Operational status snapshot (see below) |
| `getpos` | — | `pos <x> <y> <z> <a>` | Absolute machinePos in steps (signed) |
| `enable` | — | `ok` / `err <reason>` | Energise motors (per allowed-state matrix) |
| `disable` | — | `ok` / `err <reason>` | De-energise; clears `axes_homed`, resets `axisBounds` |
| `setorigin` | `[axes]` | `ok` / `err <reason>` | Set datum for given axes (default all): home bits + zero pos + real bounds |
| `pause` | — | `ok` / `err <reason>` | Request pause of the running job (Core 0 sets flag, Core 1 drains) |
| `resume` | — | `ok` / `err <reason>` | Continue a paused job (gated on `axes_homed & required_axes`) |
| `cancel` | — | `ok` | Abandon the paused job → IDLE |
| `stop` | — | `ok` | Emergency stop — flush, ALARM(ESTOP); always available |
| `unalarm` | — | `ok` / `err <reason>` | Clear ALARM → IDLE (when the cause is resolved) |

### `getstate` reply fields

```
state=<s>    machineState   0=IDLE 1=RUNNING 2=ESTOP 3=ALARM 4=PAUSED 5=HOMING
homed=<hex>  axes_homed     bitmask, bit0=X bit1=Y bit2=Z bit3=A (e.g. 0x0f = all)
alarm=<a>    alarmReason    0=NONE 1=ESTOP 2=CONFIG 3=SOFT_LIMIT 4=HOMING_FAIL
running=<r>  runningReason  0=JOB 1=JOG  (only meaningful while state=RUNNING)
```

This is the read the host polls during pre-flight and the PAUSE choreography to
know what is blocking a resume. (It is the Phase 1 subset of what the Phase 2
`CMD_HANDSHAKE` also reports — minus `config_crc32`.) Fields are key=value so
the host parser tolerates later additions.

---

## Command Allowed-State Matrix

Stream, jog, and control-command acceptance depend on state. Config commands are
Phase 2.

**Phase 1 — data plane:**

| | IDLE | RUNNING | PAUSED | ALARM | HOMING |
|---|---|---|---|---|---|
| MSEG / job stream | ✓ | ✓ (enqueue) | ✗ | ✗ | ✗ |
| Jog packet | ✓ | ✗ | ✓ | ✗ | ✗ |

**Phase 1 — control plane:**

| | IDLE | RUNNING | PAUSED | ALARM | HOMING |
|---|---|---|---|---|---|
| `ping` / `getstate` / `getpos` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `pingnode` | ✓ | ✗ | ✓ | ✓ | ✗ |
| `enable` / `disable` | ✓ | ✗ | ✓ | ✓ | ✗ |
| `setorigin` | ✓ | ✗ | ✓ | ✓ | ✗ |
| `pause` | ✗ | ✓ | ✗ | ✗ | ✗ |
| `resume` / `cancel` | ✗ | ✗ | ✓ | ✗ | ✗ |
| `stop` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `unalarm` | ✗ | ✗ | ✗ | ✓ | ✗ |

`pingnode` is blocked in RUNNING because the RS485 bus is saturated with stream
traffic; node presence is checked at pre-flight (IDLE) and tool change (PAUSED).
Rejected commands reply `err <reason>` and change nothing.

**Phase 2 additions** (`CMD_HANDSHAKE`, `CMD_GET_CONFIG`, `CMD_SET_CONFIG`):

| | IDLE | RUNNING | PAUSED | ALARM | HOMING |
|---|---|---|---|---|---|
| `CMD_HANDSHAKE` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `CMD_GET_CONFIG` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `CMD_SET_CONFIG` | ✓ | ✗ | ✗ | ✓ | ✗ |

`CMD_SET_CONFIG` in ALARM is the config-recovery path — any alarm is a safe
stopped state. Push rejected in RUNNING/PAUSED because RP2350 flash write stalls
XIP for both cores; disrupts step timing on Core 1.

---

## CRC Algorithms

| Use | Algorithm | Notes |
|---|---|---|
| MSEG packets, jog packets | CRC8 polynomial `0x8C` | Existing implementation in `common.h` |
| CMD_SET_CONFIG, CMD_GET_CONFIG, stored flash checksum *(Phase 2)* | CRC32 | Stronger collision resistance for config correctness gate; same value used for flash storage, GET response, and MCFG header |

*(Phase 2)* Both the connect-time handshake and the job stream MCFG header compare against
the identical stored flash CRC32 — same algorithm, same byte region
(`MachineConfigFlash` struct), same reference value.
