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
  `get`, `enable`, `disable`, `pause`, `resume`, `cancel`,
  `setorigin`, `stop`, `unalarm`, `ping`, `pingnode`. One command per line,
  `\n`-terminated; the Pico replies with a text line. This matches the existing
  Core 0 text CLI (`move`/`stop`/`ping`/`enable`/`disable`/`get`).

**Dispatch rule (Core 0 ingest):** at a packet boundary, peek the first byte. If
it is a known data-plane magic, read the whole fixed-length binary packet. Any
other byte begins a text line, read to `\n`. Packets and lines are **atomic** —
never interleaved — so a control command issued mid-stream (e.g. `pause` during
RUNNING) is recognised at the next boundary between MSEG packets, not byte-wise
inside one. The data-plane magics all have bit 7 set (0xA?/0xB?) and the text
commands are lowercase ASCII, so the two never collide at a boundary.

**Boot sequence.** Cold boot, `reset` and an accepted `CFG_SET` all run it:
wipe (serial input discarded), a banner line, the bus sweep, the config's
default map, then an unrequested `ready` line. `ready` means the sequence is
done; the banner is informational. Send nothing between `reset` / `CFG_ACK` and
`ready`. USB stays up throughout.

---

## Magic Bytes — Packet Type Dispatch (data plane)

| Constant | Value | Direction | Description |
|---|---|---|---|
| `MSEG_MAGIC` | `0xAB` | Host → Pico | MicroSegment — pre-computed step event |
| `JOG_MAGIC`  | `0xAE` | Host → Pico | Jog packet (separate from MSEG) |
| `BEZIER_MAGIC` | `0xAD` | Host → Pico | BEZIER record — one annotated cubic for the Pico's planner (56 B) |
| `TOOL_MAGIC` | `0xAC` | Host → Pico | ToolConfig — local production (future) |
| `MCFG_MAGIC` | `0x4D434647` (4B "MCFG") | Host → Pico | Job stream preamble (required_axes + config CRC32 in Phase 2) |
| `MSEG_ACK`   | `0xAA` | Pico → Host | ACK response |
| `MSEG_NACK`  | `0xBB` | Pico → Host | NACK response |
| `STATUS_REQ` | `0xA5` | Host → Pico | Binary status request (mirrors the `get` state keys) |
| `SEQRESET`   | `0xA8` | Host → Pico | Zero `expectedSeq`; replies `ACK(0)` |
| `STATUS_RSP` | `0xA7` | Pico → Host | Binary status response (30 B) |
| ~~`STATUS_RSP_V1`~~ | `0xA6` | — | Retired 9-byte frame; reserved, never emitted |

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

UPDATE: Phase 1 does not implement Job Stream Preamble. Well at least not as of 
08/07/2026

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
[21]     flags     uint8      — MSEG_FLAG_* bitmask
[22]     jogSeq    uint8      — 1-byte rolling duplicate guard (independent of MSEG seq)
[23..24] pad       uint8[2]
[25]     CRC8 over bytes [0..24]
```
Same 26-byte layout as MSEG (so one parser serves both), differing only in the
magic and in byte [22] carrying `jogSeq` instead of the stream `seq`. Accepted
in `STATE_IDLE` and `STATE_PAUSED`. No seqnum window — window-1 fire-and-wait;
jogSeq provides duplicate rejection only. A jog burst (host-computed move, e.g.
the return to `pausePos`) is one or more jog packets; the Pico runs
`runningReason = JOG` while emitting and returns to its prior state (IDLE, or
PAUSED when `PausedJobContext.active`) when the burst drains. The burst ends
when the ring drains, **not** on a flag — a sender may mark its last packet
`MSEG_FLAG_PATH_END`, but the firmware does not read it (see the flags table).

### BEZIER Record — `BEZIER_MAGIC` (0xAD, 56 bytes)
```
[0]      magic = 0xAD
[1]      flags      uint8      — BEZIER_FLAG_*: START 0x01, BREAK 0x02, END 0x04
[2]      seq        uint8      — rolling stream seq (the MSEG duplicate guard)
[3..34]  p0..p3     float32 LE — x, y each, work mm (the Pico adds the offsets)
[35..38] length     float32 LE — arc length, mm
[39..42] kappa_max  float32 LE — max |κ|, 1/mm
[43..46] dkappa_max float32 LE — max |dκ/ds|, 1/mm² (for A's acceleration limit; unread yet)
[47..54] c2, c3     float32 LE — t(s) = c1·s + c2·s² + c3·s³; the Pico derives c1
[55]     CRC8 over bytes [0..54]
```
One piece of a host-annotated contour (`web/src/toolpath/annotate.ts`). The
Pico does not analyse it: `checkBezier` (`lib/planner/bezier.cpp`) derives c1
and the end curvatures and refuses numbers that cannot belong to the control
points. Shares `expectedSeq`, the duplicate guard and the cumulative ACK with
MSEG, and its state gate (IDLE/RUNNING, `NACK_PAUSED` while paused); in
RUNNING only planner motion accepts more.

**Contour framing.** `START` opens a contour: if p0 is not where the last move
ends, the Pico first queues a travel line to it at the travel feed. Every
other record must start exactly where the last one ended (after the queue
drained to idle, within one step, snapped). `END` closes the contour. A
`START` inside an open contour, or a record without `START` outside one, is
`NACK_BAD_CURVE`. `seqreset` and abort forget an open contour. `BREAK` marks a
corner or cusp; joins run on the planner's junction limit whatever the flags
say — what a tool does at a flag is tool-profile scope.

**Frames.** Points are work coordinates: the Pico adds the work offset and
subtracts the selected head's offset (docs/plans/coordinate-system.md), a
translation that leaves the analysed fields as sent. With no head selected the
record is `NACK_BAD_STATE`; a point outside the soft range on a homed axis
with `softLimits` is `NACK_SOFT_LIMIT`.

Records are refused (`NACK_BAD_STATE`) until `feed` has set the cut and travel
feeds.

### ACK — `0xAA` (3 bytes)
```
[0]      0xAA
[1]      expectedSeq uint8   — cumulative accept point (next wire seq wanted)
[2]      0x00        uint8   — reserved
```
**Cumulative ACK.** `expectedSeq` is the next byte-[22] seq the Pico wants, i.e.
it has accepted every packet with a lower seq. The sender advances its Go-Back-N
window to this point rather than counting one-ACK-per-packet, so a lost or stale
ACK self-heals on the next one and a stale duplicate (same value) is idempotent.
The sender decodes the advance in 8-bit rolling space, clamped to the in-flight
window (≤16 « 128, so no wrap ambiguity). See "Duplicate guard" below.

### NACK — `0xBB` (3 bytes)
```
[0]      0xBB
[1]      reason   uint8   — see NACK reason tables below
[2]      0x00
```

### SEQRESET — `0xA8` (1 byte)
```
[0]      magic = 0xA8
```
Zeroes the Pico's `expectedSeq` and replies with a normal `ACK` carrying 0 —
exact, since "I expect seq 0 next" is precisely what an ACK means. Handled
synchronously at a packet boundary with no receive state.

Every stream must reset the seq before its first packet, because each session
stamps from 0. The text `seqreset` remains as a bring-up alias, but the binary
form keeps stream start on the data plane instead of dragging it through the
one-outstanding text plane.

The reply lands on the same sink as stream ACKs, so the caller issuing SEQRESET
must consume it. A session that opens with that ACK still queued would advance
its window against a packet it never sent.

### STATUS_REQ — `0xA5` (1 byte)
```
[0]      magic = 0xA5
```
Single byte; no payload, no CRC. Accepted in all machine states, including
RUNNING — the Pico handles it on Core 0 between MSEG packet boundaries so it
never interrupts step timing. The host may send one between any two MSEG/jog
packets by inserting the byte at a packet boundary.

### STATUS_RSP — `0xA7` (30 bytes)
```
[0]      magic         = 0xA7
[1]      machineState  uint8   — 0=IDLE 1=RUNNING 2=ESTOP 3=ALARM 4=PAUSED 5=HOMING
[2]      axes_enabled  uint8   — bitmask bit0=X bit1=Y bit2=Z bit3=A
[3]      axes_homed    uint8   — bitmask bit0=X bit1=Y bit2=Z bit3=A
[4]      alarmReason   uint8   — 0=NONE 1=ESTOP 2=CONFIG 3=SOFT_LIMIT 4=HOMING_FAIL 5=NODE_FAULT 6=LIMIT_LATCHED 7=PROBE_FAIL 8=BUS_DEGRADED
[5]      runningReason uint8   — 0=JOB 1=JOG 2=ABORT_DECEL 3=PLANNER (only meaningful while state=RUNNING)
[6..7]   bufCount      uint16 LE — MicroSegments queued in masterBuf; planner blocks while a planner job runs or is paused
[8..23]  pos[4]        int32 LE  — machinePos: x, y, z, a (steps)
[24]     expectedSeq   uint8   — next wire seq the data plane will execute
[25..28] queuedUs      uint32 LE — queued motion time, microseconds
[29]     CRC8 over bytes [0..28]
```
Carries `get`'s state keys **and** `pos` in one coherent sample; two text
round trips could disagree by tens of ms. Per the transport's per-transaction cost model the extra bytes are free: one
30-byte frame fits a single 64-byte USB packet.

`bufCount` counts segments, including the one Core 1 is mid-executing.
`queuedUs` is the sum of their durations (major-axis steps × interval), which is
what a jog source paces against — segment *count* says nothing about time when
segment durations vary by orders of magnitude. Whole-segment granularity: the
executing segment counts in full, with no subtraction of elapsed time, so the
figure over-reports by at most one segment.

`expectedSeq` is **informational**, for resynchronising after a timeout, abort or
reconnect. It is not flow control — ACKs remain the only window-advance
mechanism.

**The magic changed from `0xA6`.** A reader consumes fixed-length frames blind,
so a host expecting the old 9-byte v1 frame must fail on an unknown magic rather
than mis-parse 30 bytes as 9 and desync the stream. `0xA6` is retired and
reserved; it is never emitted. Host and firmware for this frame must be flashed
together.

The text `get` remains for the keys this frame has no room for, and for human/debug use.

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

One byte, one namespace. **Low bits (0x01–0x04) are wire/firmware semantics**;
**high bits (0x08, 0x10) are host planning hints** carried in the stream for
host-side analysis — the firmware masks them off (`flags & 0x07`).

| Constant | Value | Owner | Description |
|---|---|---|---|
| `MSEG_FLAG_NONE` | `0x00` | — | No flags |
| `MSEG_FLAG_PATH_END` | `0x01` | advisory | Last segment in a path. **Declarative only — excluded from `MSEG_FLAG_WIRE_MASK`, so Core 1 does not act on it.** Senders may set it, offline tools may read it; same bit and meaning as the planner's `MICRO_PATH_END`. |
| `MSEG_FLAG_ESTOP` | `0x02` | wire | Poison pill — flush and halt immediately |
| `MSEG_FLAG_PAUSE` | `0x04` | wire | Pause point — Core 1 drains and enters PAUSED. **Sender-inserted** at a single-head tool-change boundary; the planner never sets it. |
| `MICRO_LIFT` | `0x08` | host | Z raise/lower segment (planning hint; firmware ignores) |
| `MICRO_JOG` | `0x10` | host | Travel move between subpaths (planning hint; firmware ignores) |

`MICRO_JOG` was `0x04` historically — that aliased every travel move onto
`MSEG_FLAG_PAUSE`, so it moved to `0x10`. Firmware must mask to the low 3 bits
before interpreting; setting `PAUSE` on a jog/lift packet leaves its host hint
bits intact.

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
| `MSEG_NACK_BAD_CURVE` | `0x08` | BEZIER record failed `checkBezier`, or broke contour framing or the chain |
| `MSEG_NACK_SOFT_LIMIT` | `0x09` | BEZIER record point outside the soft range on a homed axis, after the work offset |

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
| `pingnode` | `[all\|<id>]` | `node <id> ok` / `node <id> timeout` | Relay an RS485 CMD_PING to a bus node; report presence |
| `busstat` | `<id>` | `node <id> ferr <n> ovf <n> crc <n>` / `node <id> timeout` | A node's receive-error counters (CMD_BUS_STATS): framing errors, buffer overflows, CRC failures. Wrapping 16-bit counts since the node powered on; diff successive reads |
| `makesafe` | `<id>` | `node <id> en <0\|1> datum <0\|1> slot <n\|->` / `node <id> timeout` | Relay CMD_MAKE_SAFE: the node de-energises, clears its datum, drops its stream slot (stepper, probe vacuum) and closes its servos (vacuum). The line is the node's own report after the release; `slot -` = the node has none. The Pico's slot binding is not changed |
| `get` | `[<key> …]` | `<key>=<value> …` / `keys <key> …` / `err too_many_keys` | Read keys from one snapshot, in the order asked (see below). Bare: the keys this build knows. At most 32 keys |
| `enable` | `[all\|<id>]` | `ok` / `err <reason>` | Energise motors (per allowed-state matrix). Bare / `all` energises every present node; `enable <id>` relays CMD_ENABLE to that node only (mirrors `pingnode <id>`) |
| `disable` | `[all\|<id>]` | `ok` / `err <reason>` | De-energise. Bare / `all` de-energises every node and clears `axes_homed`/`axisBounds` for all axes; `disable <id>` relays CMD_DISABLE to that node only and clears homing/bounds for that axis alone |
| `setorigin` | `<node>:<steps> [<node>:<steps> …]` | `ok` / `err usage` / `err node <id> not_in_config\|not_stepper\|no_datum\|<rpc>` / `err estop` / `err busy` | Set each node's datum: it stands at machine position `<steps>` (wire frame). Every pair is tried; `err node` names the first that failed, the others keep their datum. Clears no alarm. Between homing legs an `ok` also ends the session → IDLE, or `ALARM_LIMIT_LATCHED` if a switch is held; `err busy` while any leg runs |
| `dummy_leg` | `<0\|1> [ms]` | `ok` / `err busy` / `err bad_state` / `err usage` / `err range` | Testing primitive: a homing leg with no node or motion, admitted where a leg is. After `ms` (default 1000, max 600000) `1` ends as a successful leg (`homing=1`), `0` as `ALARM_HOMING_FAIL` with `homefail=7`. Touches no latch and no origin |
| `home_end` | — | `ok` / `err busy` / `err bad_state` | End a homing session between legs without a datum (each leg already dropped its node's origin) → IDLE, or `ALARM_LIMIT_LATCHED` if a switch is held. `err busy` while a leg runs (`stop` aborts one); `err bad_state` outside a session |
| `home` | `[only] [<node> …]` | `ok` / `err usage` / `err busy` / `err bad_state` / `err node <id> not_in_config\|not_homeable\|mute\|excluded\|<rpc>` / `err <leg refusal>` | Controller: home the nodes from the homing config (homing.md §2.8); none = every homeable node. Without `only`, earlier cycles are cleared first. `ok` = started; progress in `homing=`/`homecycle=`, failure as `ALARM_HOMING_FAIL`. Raw `leg`, `dummy_leg`, `setorigin` and `home_end` answer `err busy` until it ends |
| `home_unhomed` | — | as `home` | `home` of every homeable node with no origin; `ok` with no motion if none |
| `home_cycle` | `<k>` | as `home` | `home` of cycle k's nodes; an empty cycle is `err usage` |
| `home_head` | `<n>` | as `home` | `home only` of `heads[n]`'s Z and A |
| `axes_map` | `[<x> <y> <z> <a>]` | `ok` / `err degraded` / `err unconfigured` / `err node <id> not_in_config\|not_stepper\|timeout\|<rpc>` / `err fenced <t> <t> <t> <t>` / `err dup` / `err bad_node` | The axis request: `-`/`0` = no axis. Each id must be a stepper axis node the config marks present; applied as a `slot_map` of the same ids once all are confirmed. A silent node stays pending, its slot parked, and the machine goes `ALARM_NODE_FAULT`. No-arg: `axes_map <t> <t> <t> <t>`, `t` = `-`, `n` or `?n` (pending). See engage_and_axis_map.md §5.4 |
| `slot_map` | `[<n0> <n1> <n2> <n3>]` | `ok` / `err degraded` / `err node <id> <rpc>` / `err fenced <t> <t> <t> <t>` / `err dup` / `err bad_node` | Console primitive: bind any node to stream slots 0..3, no config. A node that does not engage leaves the request unmet (`ALARM_NODE_FAULT`). `err fenced` names the node of each requested fenced slot whose make-safe went unconfirmed, `-` elsewhere. No-arg: `slot_map <t> <t> <t> <t>`, `t` = `-`, `n` or `!n` (fenced). See engage_and_axis_map.md §5.5 |
| `pause` | — | `ok` / `err <reason>` | Request pause of the running job (Core 0 sets flag, Core 1 drains). Planner motion brakes to a hold and keeps its queue |
| `resume` | — | `ok` / `err <reason>` | Continue a paused job (gated on `axes_homed & required_axes`). A held planner job continues where it stopped: `err moved` if X or Y is no longer at the held position |
| `cancel` | — | `ok` | Abandon the paused job → IDLE |
| `stop` | — | `ok` | Emergency stop — flush, ALARM(ESTOP); always available |
| `unstop` | — | `ok` / `err unconfirmed <ids>` / `err bad_state` | Leave `ALARM_ESTOP`: make safe every touched node (energised or holding a slot) that is not excluded; refused until all confirm. Forgets the requested maps → IDLE (or `ALARM_BUS_DEGRADED`), unmapped, de-energised, un-homed |
| `bus_exclude` | `<id> …` | `ok` / `err not_mute` / `err bad_node` / `err usage` | Run without mute nodes (those the boot sweep could not make safe). Commands to an excluded node then answer `excluded` (make-safe exempt) until the next `reset`. With no unexcluded mute node left, `ALARM_BUS_DEGRADED` → IDLE, unmapped. Any state |
| `uncfg` | — | `ok` / `err bad_state` | Builds with `PICO_ALLOW_UNCONFIGURED` only. Leave `ALARM_CONFIG` → IDLE (or the next alarm), unconfigured; the stored file is kept and ignored until power-off. The other way out is a `CFG_SET` commit |
| `unalarm` | — | `ok` / `err <reason>` | Clear ALARM → IDLE (when the cause is resolved). `err estop` in `ALARM_ESTOP`: use `unstop` |
| `line` | `<x> <y> <feed>` | `ok <depth>` / `err usage\|bad_state\|unconfigured\|no_limits\|full\|no_head\|soft_limit x\|soft_limit y` | Bring-up only: queue a planner line to work mm (x, y) at `feed` mm/s, planned and run on the Pico (runningReason 3). `<depth>` = blocks queued. IDLE, or while planner motion runs. Refused if X or Y has `maxFeed` or `maxAccel` 0, with no head selected (`no_head`), or outside the soft range on a homed axis with `softLimits`. No homing check |
| `bez` | `<p1x> <p1y> <p2x> <p2y> <p3x> <p3y> <feed>` | `ok <depth>` / `err usage\|bad_state\|unconfigured\|no_limits\|full\|bad_curve\|no_head\|soft_limit x\|soft_limit y` | Bring-up only: queue a cubic Bézier from where the last move ends (machinePos on an empty, idle queue) through handles p1, p2 to p3, work mm, at `feed` mm/s; analysed on the Pico. `bad_curve` = a handle on its endpoint, a cusp, or an arc-length fit that runs backwards. Same states and checks as `line`, on every handle |
| `select` | `<head>\|anchor` | `ok` / `err usage\|bad_state\|degraded\|no_head` / as `axes_map` | Controller command: the controlled point. A head commits its Z and A as the axes map (`axes_map`'s replies) and turns the laser off; `anchor` is the laser tip (laser on; maps X and Y if either is unmapped, Z and A unchanged) or, without a laser, the head at (0, 0). Never moves. IDLE/PAUSED/ALARM, not while probing. The selection is derived from the axes map: a head needs X, Y and its Z and A mapped, the laser X and Y, else none. A raw `axes_map` that binds a different head selects it (laser off) |
| `wzero` | `[x] [y] [z]` | `ok` / `err usage\|bad_state\|no_head\|not_homed <axis>` | Controller command: the work offset becomes the selected tip's machine position on the named axes (bare: x, y, and z when a head's Z is bound). Z is per head. Volatile: reboot or a config commit restores the config's `work` block. IDLE/PAUSED/ALARM |
| `wset` | `<axis> <v> …` | `ok` / `err usage\|bad_state\|no_head` | Controller command: set the work offset in machine units, per axis (x, y, z) |
| `wclear` | — | `ok` / `err usage\|bad_state` | Controller command: the work offset back to the config's `work` block |
| `feed` | `<cut> <travel>` | `ok` / `err usage` | Testing primitive: the feeds (mm/s) BEZIER records run at — cut for the curves, travel for the line to a `START` record's p0. Held until reboot; records are refused until it is sent. The `TOOL` record replaces it |
| `seqreset` | — | `seq reset` | Data-plane support: zero the duplicate-guard seq (`expectedSeq`), which is also the cumulative ACK value, and forget an open BEZIER contour. Host sends this before each MSEG/jog/BEZIER stream so packet index 0 lines up. See "Duplicate guard" below. |

### `get` keys

`get` answers `key=value` for each key asked, space-separated, in the order
asked, all read in one snapshot. In place of a value: `?` the read failed, `-`
the key does not apply now, `!` the key is not recognised (older firmware). A
request line is at most 255 characters.

```
state=<s>     machineState    0=IDLE 1=RUNNING 2=ESTOP 3=ALARM 4=PAUSED 5=HOMING
enabled=<hex> axes_enabled    bitmask, bit0=X bit1=Y bit2=Z bit3=A — energised axes
homed=<hex>   axes_homed      bitmask, bit0=X bit1=Y bit2=Z bit3=A (e.g. 0x0f = all)
alarm=<a>     alarmReason     0=NONE 1=ESTOP 2=CONFIG 3=SOFT_LIMIT 4=HOMING_FAIL 5=NODE_FAULT 6=LIMIT_LATCHED 7=PROBE_FAIL 8=BUS_DEGRADED
running=<r>   runningReason   0=JOB 1=JOG 2=ABORT_DECEL 3=PLANNER  (only meaningful while state=RUNNING)
latched=<hex> homingLatched   bitmask per slot, an axis held by a latched limit
homefail=<f>  homingFailWhy   LEGFAIL_* of the leg that failed  (only with alarm=HOMING_FAIL)
homenode=<n>  homingFailNode  bus id of that leg's node, 0 for a dummy leg  (with homefail)
cfgerr=<r>    boot config     absent | fs | file | <decode error>  (only with alarm=CONFIG)
homing=<h>    homingReason    0=LEG 1=WAIT  (only while state=HOMING; LEG = any leg runs, WAIT = none)
probing=<p>   probingReason   probe session phase  (only while PROBING or with alarm=PROBE_FAIL)
probe=<c>     last leg cause  PROBE_*  (with probing)
retries=<n>   last leg retries  (with probing)
psteps=<n>    last leg steps  (with probing)
probed=<0|1>  a probe height is held for the Z in slot 2
pz=<z>        that height in steps  (only with probed=1)
homecycle=<k> homingHeld      the cycle a `home` run is in  (only while one runs)
nodehomed=<hex> originMask    bitmask, bit n = bus node n holds an origin, mapped or not
pos=<x>,<y>,<z>,<a>  machinePos  steps, slot order; never a sentinel, gate on homed
mpos=<x>,<y>,<z>,<a> machine position, units (mm, deg), + physical; `-` for an unbound Z/A
wpos=<x>,<y>,<z>,<a> work position: selected tip − work offset; A folded to (−180, 180]; X/Y `-` with no head
head=<n|anchor>      the controlled point: head index, `anchor` (the laser)  (`-` with none)
texp= tmeas= twall=           DEBUG_TIMING builds only: last burst's expected, measured and wall time (us)
```
Pre-flight checks every required axis is **present** (pingnode), **enabled**
(this mask), and **homed** — a present-but-disabled axis would drop steps.

This is the read the host polls during pre-flight and the PAUSE choreography to
know what is blocking a resume. (It is the Phase 1 subset of what the Phase 2
`CMD_HANDSHAKE` also reports — minus `config_crc32`.) Fields are key=value so
the host parser tolerates later additions.

---

### Duplicate guard (MSEG / jog / BEZIER seq)

Each MSEG/jog packet carries a rolling 8-bit seq in byte [22], a BEZIER record
in byte [2]. The Go-Back-N
sender, on a NACK, rewinds to `base` and resends packets that were in flight
behind the rejected one — packets the Pico may have already accepted. The Pico
tracks `expectedSeq` (the next seq it will execute); a packet whose seq it has
already consumed is skipped — **not executed again** (re-executing = a permanent
position offset) — while a matching one is executed and bumps `expectedSeq`.

`expectedSeq` doubles as the **cumulative ACK value**: every ACK carries the
current `expectedSeq` (see ACK format above), so an executed packet reports an
advanced point and a skipped duplicate/gap reports the unchanged point (a
duplicate ACK). The host advances its window to the reported point, so lost or
stale ACKs self-heal without a timeout. `seqreset` zeroes `expectedSeq` at the
start of each stream so both sides agree where seq 0 is; the host issues it
before every `send_stream` (one per operation in a multi-tool plan).

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
| `STATUS_REQ` (binary) | ✓ | ✓ | ✓ | ✓ | ✓ |
| `ping` / `get` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `pingnode` / `busstat` / `nodestat` | ✓ | ✗ | ✓ | ✓ | between legs |
| `enable` / `disable` / `makesafe` | ✓ | ✗ | ✓ | ✓ | ✗ |
| `setorigin` | ✓ | ✗ | ✓ | ✓ | between legs |
| `leg` / `dummy_leg` | ✓ | ✗ | ✗ | `LIMIT_LATCHED` only | ✓ (one per node) |
| `leg_abort` | ✓ | ✗ | ✓ | ✓ | ✓ |
| `home_end` | ✗ | ✗ | ✗ | ✗ | between legs |
| `home` / `home_unhomed` / `home_cycle` / `home_head` | ✓ | ✗ | ✗ | `LIMIT_LATCHED` only | ✗ |
| `axes_map` / `slot_map` | ✓ | ✗ | ✓ | ✓ | ✗ |
| `pause` | ✗ | ✓ | ✗ | ✗ | ✗ |
| `resume` / `cancel` | ✗ | ✗ | ✓ | ✗ | ✗ |
| `stop` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `unalarm` | ✗ | ✗ | ✗ | ✓ | ✗ |
| `unstop` (`ALARM_ESTOP` only) | ✗ | ✗ | ✗ | ✓ | ✗ |
| `uncfg` (`ALARM_CONFIG` only) | ✗ | ✗ | ✗ | ✓ | ✗ |
| `bus_exclude` | ✓ | ✓ | ✓ | ✓ | ✓ |

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
