# Wire Protocol

**Branch:** `phase1-impl`
**Date:** 2026-06-28
**Status:** Phase 1 contract FROZEN — data plane (MSEG/JOG/MCFG, ACK/NACK) and control plane (text) defined below. Config commands (CMD_GET_CONFIG, CMD_SET_CONFIG, CMD_HANDSHAKE) deferred to Phase 2.

Single source of truth for all USB CDC framing between host and Pico.
All other docs cross-reference here rather than defining constants.

## Two planes on one USB pipe

Phase 1 splits the host↔Pico link into two planes that share the USB CDC pipe:

- **Data plane — binary, magic-dispatched.** High-rate streaming: MSEG, BEZIER, CJOG,
  the MCFG preamble. Pico replies ACK/NACK. Each binary packet is a fixed
  length keyed by its magic byte.
- **Control plane — text lines.** Low-rate commands the operator/host issues:
  `get`, `enable`, `disable`, `pause`, `cancel`,
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
| `JOG_MAGIC`  | `0xAE` | Host → Pico | Retired jog burst: always NACKed |
| `CJOG_MAGIC` | `0xAF` | Host → Pico | Continuous jog — deadman packet for a held XY, Z or A jog (7 B) |
| `CJOG_STOP_MAGIC` | `0xB3` | Host → Pico | Stop a continuous jog (1 B, no reply) |
| `BEZIER_MAGIC` | `0xAD` | Host → Pico | BEZIER record — one annotated cubic for the Pico's planner (62 B) |
| `LAYER_HDR_MAGIC` | `0xA2` | Host → Pico | Layer header — opens or resumes a job, sets the layer's tool, head, feed and plunge (22 B) |
| `TOOL_MAGIC` | `0xAC` | Host → Pico | ToolConfig — local production (future) |
| `MCFG_MAGIC` | `0x4D434647` (4B "MCFG") | Host → Pico | Job stream preamble (required_axes + config CRC32 in Phase 2) |
| `MSEG_ACK`   | `0xAA` | Pico → Host | ACK response |
| `MSEG_NACK`  | `0xBB` | Pico → Host | NACK response |
| `JOG_ACK`    | `0xB6` | Pico → Host | Continuous jog started (1 B) |
| `JOG_NACK`   | `0xB7` | Pico → Host | Continuous jog refused (2 B) |
| `STATUS_REQ` | `0xA5` | Host → Pico | Binary status request |
| `SEQRESET`   | `0xA8` | Host → Pico | Zero `expectedSeq`; replies `ACK(0)` |
| `STATUS_RSP` | `0xA0` | Pico → Host | Binary status response (38 B) |
| ~~`STATUS_RSP_V4`~~ | `0xA3` | — | Retired 37-byte frame; reserved, never emitted |
| ~~`STATUS_RSP_V3`~~ | `0xA4` | — | Retired 36-byte frame; reserved, never emitted |
| ~~`STATUS_RSP_V1`~~ | `0xA6` | — | Retired 9-byte frame; reserved, never emitted |
| ~~`STATUS_RSP_V2`~~ | `0xA7` | — | Retired 31-byte frame; reserved, never emitted |

All single-byte magics have bit 7 set, keeping them disjoint from the lowercase
ASCII that begins every control-plane line.

---

## Packet Formats

### MicroSegment — `0xAB` (26 bytes)
```
[0]      magic = 0xAB
[1..4]   dx        int32 LE   — X axis steps, must be 0
[5..8]   dy        int32 LE   — Y axis steps, must be 0
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

### Jog Packet — `JOG_MAGIC` (0xAE, 26 bytes, retired)
Retired: jogs are `jog`, `goto` and continuous jog. The Pico still reads the
26 bytes (MSEG layout) so the stream stays in sync, and NACKs every one
`NACK_BAD_STATE`.

### Continuous Jog — `CJOG_MAGIC` (0xAF, 7 bytes)
```
[0]  magic = 0xAF
[1]  dirX   int8   — −1, 0 or +1
[2]  dirY   int8   — −1, 0 or +1
[3]  dirZ   int8   — −1, 0 or +1
[4]  dirA   int8   — −1, 0 or +1
[5]  speed  uint8  — jogFeed × speed/64 (64 = 1×), capped at maxFeed; 0 is refused
[6]  CRC8 over bytes [0..5]
```
A held jog: the host repeats the packet while the operator holds the
direction, faster than every 150 ms. One axis set moves per packet: X and Y,
Z alone or A alone (Z and A are the selected head's); directions in more than
one set are NACKed `NACK_MIXED_AXES`. The first one queues a line to the end
of travel (the soft-range end when homed with `softLimits`, else `maxTravel`;
a diagonal stops at the nearer end, at the slower moving axis's feed; A runs
at most one turn per hold) and runs as JOGGING with `jogging=2`. Replies: `JOG_ACK` when a jog starts, `JOG_NACK` when
refused (a bad CRC included), nothing for a repeat. They are never `ACK` or
`NACK`, so a job's stream cannot take a jog's reply for its own. 150 ms without a packet, all-zero directions, or
`CJOG_STOP_MAGIC` (0xB3) brake and stop. A new direction stops the jog and
starts the new one once at rest (its ACK comes then). A packet while step jogs
run stops them first, the same way; a step jog during a continuous jog is
refused. Anything else must admit a step (`K_STEP`), both for the packet and
for a start held for rest. Speed changes while holding are ignored. No seq; the abort barrier
does not apply.

`JOG_NACK` reasons: `NACK_CRC` (0x01) = corrupt packet;
`NACK_SOFT_LIMIT` (0x09) = no room left in that direction, or the bed
mesh would take Z out of its soft range at once (an X/Y jog otherwise ends
where Z would leave it, see "Bed mesh");
`NACK_MIXED_AXES` (0x0A) = directions in more than one axis set;
`NACK_BUSY` (0x0B) = a step or sequence holds the machine;
`NACK_BAD_STATE` (0x06) = a bad direction or speed, a job paused, a homing or
probe context, a latched fault, unhomed without `jogUnhomed`, or no config.

### BEZIER Record — `BEZIER_MAGIC` (0xAD, 62 bytes)
```
[0]      magic = 0xAD
[1]      flags      uint8      — BEZIER_FLAG_*: START 0x01, BREAK 0x02, END 0x04
[2]      seq        uint8      — rolling stream seq (the MSEG duplicate guard)
[3..34]  p0..p3     float32 LE — x, y each, work mm (the Pico adds the offsets)
[35..38] length     float32 LE — arc length, mm
[39..42] kappa_max  float32 LE — max |κ|, 1/mm
[43..46] dkappa_max float32 LE — max |dκ/ds|, 1/mm² (for A's acceleration limit; unread yet)
[47..54] c2, c3     float32 LE — t(s) = c1·s + c2·s² + c3·s³; the Pico derives c1
[55..56] nextBreakDist uint16 LE — on a START or BREAK record: mm from p0 to the
                      next BREAK or END of the contour, rounded up; 65535 = at
                      least that far; 0 = unknown. 0 and ignored on other records
[57..58] sweepMin   int16 LE   — the heading range from p0 to the next BREAK or
[59..60] sweepMax   int16 LE     END, whole degrees from p0's heading, unwrapped,
                      rounded outward, saturating
[61]     CRC8 over bytes [0..60]
```
One piece of a host-annotated contour (`web/src/toolpath/annotate.ts`). The
Pico does not analyse it: `checkBezier` (`lib/planner/bezier.cpp`) derives c1
and the end curvatures and refuses numbers that cannot belong to the control
points. Shares `expectedSeq`, the duplicate guard and the cumulative ACK with
MSEG. A record needs a job open (a layer header first), else `NACK_JOB`; it is
accepted while the job buffers or runs, `NACK_PAUSED` while paused. It runs at
the layer's cut feed.

**Contour framing.** `START` opens a contour: if p0 is not where the last move
ends, the Pico first queues a travel line to it at the travel feed. Every
other record must start exactly where the last one ended (after the queue
drained to idle, within one step, snapped). `END` closes the contour. A
`START` inside an open contour, or a record without `START` outside one, is
`NACK_BAD_CURVE`. The first record after a layer header is a `START` whatever
its flags. `seqreset` and abort forget an open contour. `BREAK` marks a
corner or cusp; joins run on the planner's junction limit whatever the flags
say. A tangential tool's A follows the curve's heading. It reaches the
start heading during a `START`'s travel, and at a `BREAK` swivels to it,
lifting for a turn past `cornerAngleDeg`. With a cable the swivel goes to the
equivalent mod 360/`periodCount` that centres the record's sweep (grown by
0.1°) in ±`aMaxDeg`, ties to the one nearest the current A; without, to the
nearest. A sweep no equivalent fits takes the one fitting the record's own
curve nearest the edge the sweep turns away from. At a smooth join whose
record does not fit from the current A, the tool lifts, A unwinds by whole
periods (the same pick), and it plunges at the same heading. A record whose
own curve no equivalent fits in ±`aMaxDeg` is `NACK_SOFT_LIMIT`.
A chamber layer turns A during each `START`'s travel to its chamber's
`chamberOffsets` angle (the equivalent mod 360 nearest the current A inside
±`aMaxDeg`, else `NACK_SOFT_LIMIT`) and holds it. A record of a tangential or
chamber layer with A not homed is `NACK_BAD_STATE`.

**Frames.** Points are work coordinates: the Pico adds the work offset and
subtracts the selected head's offset (docs/plans/coordinate-system.md), a
translation that leaves the analysed fields as sent. With no head selected the
record is `NACK_BAD_STATE`; a point outside the soft range on a homed axis
with `softLimits` is `NACK_SOFT_LIMIT`, as is a record (or its travel line)
along which the bed mesh would take Z out of its soft range. Points are the
blade tip: with a tool whose `offsetMm` is d mounted, the Pico runs the knife
axis d ahead of the tip along the edge heading (A + `aOffsetDeg`), in jobs
and jogs alike, and shrinks X's and Y's soft ranges by d.

### Layer Header — `LAYER_HDR_MAGIC` (0xA2, 22 bytes)
```
[0]      magic = 0xA2
[1]      chamber    uint8      — 1..chamberCount; 0 for a tool without chambers
[2]      seq        uint8      — rolling stream seq, shared with BEZIER
[3]      tool       uint8      — the config tool's `id`
[4]      head       uint8      — the head the tool goes on
[5..6]   layer      uint16 LE
[7..10]  record     uint32 LE  — the next BEZIER's number within the layer
[11..14] feed       float32 LE — cut, mm/s; 0 = the tool's cutFeed, then the machine's (`feed`, else `feed.cutFallback`)
[15..18] k          float32 LE — plunge scale 0..2
[19..20] t          uint16 LE  — material thickness, µm
[21]     CRC8 over bytes [0..20]
```
**Heights.** Work Z is the tip on the material top. Every contour's START
moves Z to `machine.zClearance` over work Z, travels to its p0 and plunges to
k·t under work Z (0 for a tool with `plunge: false`) at the tool's
`plungeFeed`, else `feed.plungeFallback`; its END lifts back to clearance at
`feed.liftFeed`. A height outside Z's soft range is `NACK_SOFT_LIMIT`. A head
with no work Z set (its tool changed by `mount`, until a Z zero) stops for a
tool change (below).

**Powered tools.** A tool with a `power` block is switched by the Pico, at
rest: after a START's travel it waits out the tool's duty if due (JOB.DWELL:
the tool has been on `minOnS` or more and off less than `dwellS`), switches
the tool on, waits `settleS` and plunges; after an END's lift it switches the
tool off. A pause or abort lifts with the tool on, then switches it off. A
switch that fails 3 times alarms with the TOOL fault; a failed off also sends
the node `CMD_MAKE_SAFE`. Records are accepted in JOB.DWELL as in RUNNING, and
`pause` there lands PAUSED with no lift.

Opens a job (JOB.BUFFERING) from IDLE; in a buffering or running job it starts
the next layer. Layers may come in any order; the BEZIERs after a header are
numbered on from its `record`.

**Pause and resume.** `pause` brakes to a hold, drops the queue, lifts Z to
clearance where it stopped (still JOB.HOLDING) and then lands JOB.PAUSED,
keeping where the job stopped: the record, and how far into its
curve (`get resume_layer resume_record`). The host resumes by sending a layer
header naming that layer and record, then the records from there on; any other
header is `NACK_JOB`. The Pico cuts that record from 2 mm before where it
stopped, travelling there first. A record stopped in its travel runs whole. A job ends only by `cancel` or
abort; an abort brakes, lifts Z to clearance (JOB.ABORTING throughout) and
then goes IDLE.

**Heads and tool change.** The BEZIERs after a header run with its tool on its
`head`. With the tool mounted there (`mount`), its Z zeroed and the head
selected, the header is ACKed. With the tool mounted and zeroed and another
head selected, IDLE and a resume select it (as `select`). Otherwise the header
is `NACK_TOOL_CHANGE`, not consumed, and the job holds for a change; until it
is back every record, and every header outside JOB.TOOL_WAIT, is
`NACK_TOOL_CHANGE`. A running job finishes its ring first. Then:

* the tool mounted and zeroed, another head selected: the head is selected,
  and back (JOB.BUFFERING, or JOB.PAUSED for a change made from a pause);
* otherwise JOB.TOOL_CHANGE, the stop (`get seq=6`): the head is selected
  and unmounted (no tool, its work Z cleared), every head's Z parks, and with
  an `unlockRange` that head's A turns to machine 0, unlocking its holder
  (unwinding what it turned); other heads' As stay, so their holders stay
  locked. Then a line to the stored `mount` position. Then JOB.TOOL_WAIT: the
  operator mounts and unmounts tools on any head (`mount`, `unmount`), jogs (`jog`, continuous jogs), moves a
  tool's tip over its switch (`probe_park`) and zeroes Z (`wzero z` on the
  bed, `wzero z_switch` on the switch).

Only a header leaves JOB.TOOL_WAIT. With its tool mounted on its head and
zeroed, and nothing moving, the job goes back and the header is taken as any
other; otherwise it is `NACK_TOOL_CHANGE` again and the job stays. So the
operator may mount and zero the next layer's tool in the same stop, or leave
it: a later header whose tool is mounted but not zeroed stops again. The host
resends by itself while the job is in JOB.TOOL_CHANGE, and on the operator's
word in JOB.TOOL_WAIT.

`pause` is refused in both; `cancel` ends the job from TOOL_WAIT, `stop` in
either. A refusal after the NACK (a Z or A not homed or not answering, no
`mount` position, a failed park) alarms with F_SEQUENCE or F_HOMING and ends
the job.

Refusals: `NACK_TOOL_UNKNOWN` = the id is not in the config; `NACK_JOB` = a
head the config lacks, inside an open
contour, a bad chamber, feed or k, a tangential or chamber tool on a head without A,
or a paused job's header not naming where it stopped; `NACK_BAD_STATE` = no config, the select failed, or the ring
holds jogs (JOGGING, or jogs queued in IDLE); `NACK_PAUSED` while pausing.

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

### JOG_ACK — `0xB6` (1 byte), JOG_NACK — `0xB7` (2 bytes)
```
JOG_ACK:  [0] 0xB6
JOG_NACK: [0] 0xB7  [1] reason uint8 — a NACK reason code
```
Continuous-jog replies only. No seq: a jog is held by repeating its packet,
not by its replies.

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

### STATUS_RSP — `0xA0` (38 bytes)
```
[0]      magic         = 0xA0
[1]      machineState  uint8   — 0=IDLE 2=ESTOP 3=ALARM 5=HOMING 6=PROBING 7=JOGGING 8=TRAVERSE 9=JOB (1 and 4 retired)
[2]      reason        uint8   — the current state's reason: `job=`, `jogging=`, `homing=` or `probing=`; 0 otherwise
[3..4]   faults        uint16 LE — fault mask (below)
[5..6]   nodeEnabled   uint16 LE — bit n = bus node n enabled, mapped or not (`get nodeenabled`)
[7..8]   nodeHomed     uint16 LE — bit n = bus node n holds an origin (`get nodehomed`)
[9]      selection     uint8   — the controlled point: head index, 0xFE anchor, 0xFF none (`get head`)
[10..11] tools[2]      uint8   — the tool id each head has mounted, head order; 0 none or no such head (`get tools`)
[12..27] wpos[4]       float32 LE — work position x, y, z, a, mm and degrees; NaN with no value (`get wpos`)
[28..29] bufCount      uint16 LE — planner blocks queued (`get buf`)
[30..31] layer         uint16 LE — the job's executing record: its layer
[32..35] record        uint32 LE   and record (below); 0xFFFF and 0 with no job
[36]     context       uint8   — machineContext: 0=IDLE 1=JOB 2=PROBE 3=HOMING (`get ctx`)
[37]     CRC8 over bytes [0..36]
```
One coherent sample, in the node frame: the host is told nodes and the
selection, never slots. The slot masks and MPos stay on `get`. A controller
has at most two heads (`CFG_MAX_HEADS`); a config with more is refused. Per the transport's per-transaction cost model the bytes are free:
one frame fits a single 64-byte USB packet.

`layer`/`record` say how far the ring has run, which the host cannot know from
what it sent: the tag of the block Core 1 runs (an inserted travel, lift,
plunge or swivel carries the record it leads into); while PAUSED, the resume
point (`resume_layer`/`resume_record`); in a job before any block runs, the
oldest queued record, or the next one expected.

`context` is what is held between commands; `machineState` is what runs now.
A jog inside a paused or tool-waiting job reports JOGGING with the context
still JOB. The context and the layer are read under one lock: the layer is
set exactly while the context is JOB.

The fault mask (`get faults=` too): 0x0001=BUS 0x0002=CONFIG 0x0004=UNMAPPED 0x0008=LATCHED 0x0010=HOMING 0x0020=PROBE 0x0040=FACTS 0x0080=SEQUENCE 0x0100=TOOL 0x8000=ESTOP. Held faults (BUS, CONFIG,
UNMAPPED, LATCHED) clear by themselves when their cause goes; event faults
(HOMING, PROBE, FACTS, SEQUENCE, TOOL) stay until `unalarm`. ESTOP is the estop latch, not a
fault, cleared by `unstop`. The state is ALARM at rest while any bit is set,
IDLE otherwise; several bits may be set at once.

**Each layout has its own magic.** A reader consumes fixed-length frames blind,
so a host expecting another layout must fail on an unknown magic rather than
mis-parse it and desync the stream. `0xA6` (9 B), `0xA7` (31 B, slot masks,
`pos` in steps, `expectedSeq`, `queuedUs`), `0xA4` (36 B, the selected
head's tool only) and `0xA3` (37 B, no context) are retired and reserved; they are never emitted. Host and firmware for this frame must be flashed together.

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
| `MSEG_NACK_ABORTING` | `0x07` | Record sent during an abort, its lift included — retry once IDLE |
| `MSEG_NACK_BAD_CURVE` | `0x08` | BEZIER record failed `checkBezier`, or broke contour framing or the chain |
| `MSEG_NACK_SOFT_LIMIT` | `0x09` | BEZIER record point outside the soft range on a homed axis, after the work offset, or a record the bed mesh would take Z out of range on; a continuous jog with no room left in its direction |
| `MSEG_NACK_MIXED_AXES` | `0x0A` | Continuous jog with directions in more than one axis set (XY, Z, A) |
| `MSEG_NACK_BUSY` | `0x0B` | Continuous jog while a step or sequence holds the machine (a job, homing, a leg, a traverse) |
| `MSEG_NACK_JOB` | `0x0C` | BEZIER with no job open; layer header inside a contour, with a bad chamber, feed or k, or not naming where a paused job stopped |
| `MSEG_NACK_TOOL_UNKNOWN` | `0x0D` | Layer header whose tool id is not in the config |
| `MSEG_NACK_TOOL_CHANGE` | `0x0F` | Layer header whose tool is not mounted on its head or not zeroed, or whose head is not selected, not consumed; any record, and any header outside TOOL_WAIT, while the job holds for a tool change. Resend from the header: by itself in TOOL_CHANGE, on the operator's word in TOOL_WAIT |

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
| `get` | `[<key>\|<group> …]` | `<key>=<value> …` / `keys <key> …` / `err too_many_keys` | Read keys from one snapshot, in the order asked (see below). A group asks for its keys. Bare: the keys this build knows (groups not listed). At most 32 keys after expansion |
| `?` | — | as `get status` | Human shorthand for the `status` group |
| `enable` | `[all\|<id>]` | `ok` / `err <reason>` | Energise motors (per allowed-state matrix). Bare / `all` energises every present node; `enable <id>` relays CMD_ENABLE to that node only (mirrors `pingnode <id>`) |
| `disable` | `[all\|<id>]` | `ok` / `err <reason>` | De-energise. Bare / `all` de-energises every node and clears `axes_homed`/`axisBounds` for all axes; `disable <id>` relays CMD_DISABLE to that node only and clears homing/bounds for that axis alone |
| `setorigin` | `<node>:<steps> [<node>:<steps> …]` | `ok` / `err usage` / `err node <id> not_in_config\|not_stepper\|no_datum\|<rpc>` / `err estop` / `err busy` | Set each node's datum: it stands at machine position `<steps>` (wire frame). Every pair is tried; `err node` names the first that failed, the others keep their datum. Clears no alarm. Between homing legs an `ok` also ends the session → IDLE, or ALARM with LATCHED if a switch is held; `err busy` while any leg runs |
| `dummy_leg` | `<0\|1> [ms]` | `ok` / `err busy` / `err bad_state` / `err usage` / `err range` | Testing primitive: a homing leg with no node or motion, admitted where a leg is. After `ms` (default 1000, max 600000) `1` ends as a successful leg (`homing=1`), `0` as the HOMING fault with `homefail=7`. Touches no latch and no origin |
| `home_end` | — | `ok` / `err busy` / `err bad_state` | End a homing session between legs without a datum (each leg already dropped its node's origin) → IDLE, or ALARM with LATCHED if a switch is held. `err busy` while a leg runs (`stop` aborts one); `err bad_state` outside a session |
| `home` | `[only] [<node> …]` | `ok` / `err usage` / `err busy` / `err bad_state` / `err node <id> not_in_config\|not_homeable\|mute\|excluded\|<rpc>` / `err <leg refusal>` | Controller: home the nodes from the homing config (homing.md §2.8); none = every homeable node. Without `only`, earlier cycles are cleared first. `ok` = started; progress in `homing=`/`homecycle=`, failure as the HOMING fault. Raw `leg`, `dummy_leg`, `setorigin` and `home_end` answer `err busy` until it ends |
| `home_unhomed` | — | as `home` | `home` of every homeable node with no origin; `ok` with no motion if none |
| `home_cycle` | `<k>` | as `home` | `home` of cycle k's nodes; an empty cycle is `err usage` |
| `home_head` | `<n>` | as `home` | `home only` of `heads[n]`'s Z and A |
| `home_xy` | — | as `home` | `home` of the config's X and Y nodes, so earlier cycles (the Zs by default) park or home first; `err usage` when neither is homeable |
| `axes_map` | `[<x> <y> <z> <a>]` | `ok` / `err degraded` / `err unconfigured` / `err node <id> not_in_config\|not_stepper\|timeout\|<rpc>` / `err fenced <t> <t> <t> <t>` / `err dup` / `err bad_node` | The axis request: `-`/`0` = no axis. Each id must be a stepper axis node the config marks present; applied as a `slot_map` of the same ids once all are confirmed. A silent node stays pending, its slot parked, and UNMAPPED is set. No-arg: `axes_map <t> <t> <t> <t>`, `t` = `-`, `n` or `?n` (pending). See engage_and_axis_map.md §5.4 |
| `slot_map` | `[<n0> <n1> <n2> <n3>]` | `ok` / `err degraded` / `err node <id> <rpc>` / `err fenced <t> <t> <t> <t>` / `err dup` / `err bad_node` | Console primitive: bind any node to stream slots 0..3, no config. A node that does not engage leaves the request unmet (UNMAPPED). `err fenced` names the node of each requested fenced slot whose make-safe went unconfirmed, `-` elsewhere. No-arg: `slot_map <t> <t> <t> <t>`, `t` = `-`, `n` or `!n` (fenced). See engage_and_axis_map.md §5.5 |
| `pause` | — | `ok` / `err <reason>` | Pause a buffering or running job: planner motion brakes to a hold, lands PAUSED and drops its queue. A layer header resumes it (see "Layer Header") |
| `cancel` | — | `ok` | Abandon the paused job → IDLE |
| `stop` | — | `ok` | Emergency stop — flush, ALARM(ESTOP); always available |
| `unstop` | — | `ok` / `err unconfirmed <ids>` / `err bad_state` | Clear the estop latch, once ALARM is published: make safe every touched node (energised or holding a slot) that is not excluded; refused until all confirm. Forgets the requested maps → IDLE (or ALARM on a held fault), unmapped, de-energised, un-homed |
| `bus_exclude` | `<id> …` | `ok` / `err not_mute` / `err bad_node` / `err usage` | Run without mute nodes (those the boot sweep could not make safe). Commands to an excluded node then answer `excluded` (make-safe exempt) until the next `reset`. With no unexcluded mute node left, BUS clears (unmapped). Any state |
| `uncfg` | — | `ok` / `err bad_state` | Builds with `PICO_ALLOW_UNCONFIGURED` only. Clear CONFIG, unconfigured; the stored file is kept and ignored until power-off. The other way out is a `CFG_SET` commit |
| `unalarm` | — | `ok faults=<hex>` / `err bad_state` / `err estop` / `err degraded` / `err unmapped` | In ALARM: clear the event faults, then fix what can be fixed at once, in order: BUS (`err degraded`: needs `bus_exclude`), then an unmet slot request (retried once; `err unmapped`). `ok` carries what is left (CONFIG, LATCHED). `err estop` with the latch set: use `unstop` |
| `line` | `<x> <y> <feed>` | `ok <depth>` / `err usage\|bad_state\|unconfigured\|no_limits\|full\|no_head\|soft_limit x\|soft_limit y` | Bring-up only: queue a planner line to work mm (x, y) at `feed` mm/s, planned and run on the Pico as a jog (state JOGGING). `<depth>` = blocks queued. IDLE, or JOGGING; `bad_state` while the ring holds a streamed job. Refused if X or Y has `maxFeed` or `maxAccel` 0, with no head selected (`no_head`), or outside the soft range on a homed axis with `softLimits`; `soft_limit z` = the bed mesh would take Z out of its soft range along the move. No homing check |
| `bez` | `<p1x> <p1y> <p2x> <p2y> <p3x> <p3y> <feed>` | `ok <depth>` / `err usage\|bad_state\|unconfigured\|no_limits\|full\|bad_curve\|no_head\|soft_limit x\|soft_limit y` | Bring-up only: queue a cubic Bézier from where the last move ends (machinePos on an empty, idle queue) through handles p1, p2 to p3, work mm, at `feed` mm/s; analysed on the Pico. `bad_curve` = a handle on its endpoint, a cusp, or an arc-length fit that runs backwards. Same states and checks as `line`, on every handle |
| `jog` | `<x\|y\|z\|a> <dist> [scale]` \| `xy <dx> <dy> [scale]` | `ok <depth>` / `err usage\|unconfigured\|no_head\|not_homed\|too_far\|no_feed\|busy\|bad_state\|soft_limit <axis> <mm>` (and `line`'s queue errors) | Step jog: move one axis by `dist` machine mm (A: degrees) from where the queued jogs end, or X and Y by (`dx`, `dy`) in one line; Z and A are the selected head's (`no_head` with none bound), as a jog (state JOGGING). Relative, so the same in either frame. Feed = the axis's `jogFeed` × `scale` (default 1), capped at `maxFeed`; `xy` takes the slower of X's and Y's. Clicks join the running jog; `busy` with 4 blocks queued. Unhomed: `not_homed` unless `jogUnhomed`, then `jogFeedUnhomed`, no soft range, and `too_far` past `maxTravel` (one turn on A). A has no soft range. `soft_limit <axis> <mm>` = the target is outside the soft range on a homed axis with `softLimits`; `<mm>` is the distance still free in that direction. Also in JOB.TOOL_WAIT |
| `goto` | `<m\|w> xy <x> <y> [scale]` \| `<m\|w> <z\|a> <pos> [scale]` \| `park\|load\|mount [scale]` \| `up\|unlock` | `ok` / `err usage\|unconfigured\|not_homed\|a_not_homed\|no_head\|no_work\|too_far\|no_position\|no_feed\|busy\|bad_state\|soft_limit <axis> <mm>\|node <n> excluded\|mute\|<rpc>` (and `line`'s queue errors) | Controller command, a sequence, published as JOGGING with `jogging=3` throughout. Feed = `jogFeed` × `scale` (default 1), capped at `maxFeed`; XY at the slower of X's and Y's. **XY forms** park every present head's Z (legs, in parallel), then run a straight line: `m xy` is the anchor's machine mm (as `mpos`), `w xy` work mm for the selected tip; `park` (where homing parks X and Y without a stored one), `load` and `mount` are stored anchor positions, `no_position` when the config has none; `mount` also turns the selected head's A to unlock (machine 0) along the line (`a_not_homed` unless A is homed). X, Y and every present head's Z must be homed. **`z\|a`** move the selected head's axis through the planner, no Z park; that axis must be homed. `m` is machine units; `w z` is work mm (the head's work Z and the bed mesh at the tip, `no_work` until `wzero z`), `w a` the edge heading in degrees, on the turn nearest the current A, kept within the mounted tool's `aMaxDeg` of machine 0 for a cabled tool (`too_far` with none). **`up`** parks every present head's Z, **`unlock`** turns every present head's A to machine 0 (legs, at the homing park speeds), each homed. IDLE only, and refused with any fault but a latch; `busy` while anything runs. `ok` once it starts, or at once with nothing to move; a failed park is `F_HOMING` (`homefail=8`), a line refused after the park `F_SEQUENCE` (`seqfail=soft_limit\|full\|no_limits\|unconfigured\|bad_state`); with no Z to park the line's refusal is the reply. Refuses at the soft range as `jog`. |
| `probe_park` | `<tool id>` | as `goto`, and `err no_tool` | Controller command: `goto` (an XY form) to the position that puts tool `<tool id>`'s tip on its head's probe switch, whether or not it is mounted: the head it is mounted on (selected first), else the selected head; the switch (machine XY, one per head) less the head's offset and the tool's `toolOffset`. A head with an A turns it to machine 0, unlocking the holder, along the line (`a_not_homed` unless A is homed). `no_head` with none selected, `no_position` when the head has no switch. Also in JOB.TOOL_WAIT |
| `select` | `<head>\|anchor` | `ok` / `err usage\|bad_state\|degraded\|no_head` / as `axes_map` | Controller command: the controlled point. A head commits its Z and A as the axes map (`axes_map`'s replies) and turns the laser off; `anchor` is the laser tip (laser on; maps X and Y if either is unmapped, Z and A unchanged) or, without a laser, the head at (0, 0). Never moves. IDLE/PAUSED/ALARM, not while probing. The selection is derived from the axes map: a head needs X, Y and its Z and A mapped, the laser X and Y, else none. A raw `axes_map` that binds a different head selects it (laser off) |
| `wzero` | `[x] [y] [z\|z_switch]` | `ok` / `err usage\|bad_state\|no_head\|no_switch_height\|not_homed <axis>` | Controller command: the work offset becomes the selected tip's machine position on the named axes (bare: x, y, and z when a head's Z is bound). Z is per head and its 0 is the bed: `z` with the tip on the bed, `z_switch` with it on the head's switch, the work Z then the tip's less the switch's `probeSwitch.z` (`no_switch_height` without one). Volatile: reboot or a config commit restores the config's `work` block. IDLE/PAUSED/ALARM, and JOB.TOOL_WAIT |
| `wset` | `<axis> <v> …` | `ok` / `err usage\|bad_state\|no_head` | Controller command: set the work offset in machine units, per axis (x, y, z) |
| `wclear` | — | `ok` / `err usage\|bad_state` | Controller command: the work offset back to the config's `work` block |
| `mesh` | `on\|off` | `ok` / `err usage\|bad_state` | Controller command: follow the bed mesh or not, from the next move started from rest; until reboot, when the config's `meshOn` applies again. IDLE/PAUSED/ALARM. See "Bed mesh" |
| `mount` | `<head> <tool id>` | `ok` / `err usage\|unconfigured\|bad_state\|busy\|no_head\|no_tool\|mounted\|not_homed\|a_not_homed\|node <n> excluded\|mute\|<rpc>` | Controller command: `head` takes tool `<tool id>` (`tools.<name>.id`, from 1). A head holds one tool: `mounted` while it has any, `unmount` first. On a head with `unlockRange` it is a sequence (as `goto`, `jogging=3`): that head's Z parks, then its A turns to machine `unlockRange` + 1°, locking the holder; no turn when A is already outside the range; Z and A must be homed. Without one it only records the tool. RAM only: kept across jobs and soft resets, cleared at boot and by a config push. Separate from `select`. IDLE (refused with any fault but a latch) and JOB.TOOL_WAIT; `busy` while anything runs. A new tool clears the head's work Z until `wzero z`. `get tools` reports it. The mounted tool's `toolOffset` adds to its head's offset in the tip, so work positions and the readout follow it |
| `unmount` | `<head>` | as `mount`, without `no_tool\|mounted` | Controller command, a sequence as `mount`: `head` has no tool (its work Z cleared), its Z parks, then with `unlockRange` its A turns to machine 0, unlocking the holder; each step skipped when already there. Admitted as `mount` |
| `feed` | `<cut> <travel>` | `ok` / `err usage` | Override of the config's `feed.cutFallback` and `feed.travelDefault` (mm/s, each at least 1), held until reboot: cut for the curves, travel for the line to a `START` record's p0. A layer header's feed, then its tool's `cutFeed`, overrides the cut |
| `seqreset` | — | `seq reset` | Data-plane support: zero the duplicate-guard seq (`expectedSeq`), which is also the cumulative ACK value, and forget an open BEZIER contour. Host sends this before each MSEG/jog/BEZIER stream so packet index 0 lines up. See "Duplicate guard" below. |

### `get` keys

`get` answers `key=value` for each key asked, space-separated, in the order
asked, all read in one snapshot. In place of a value: `?` the read failed, `-`
the key does not apply now, `!` the key is not recognised (older firmware). A
request line is at most 255 characters.

A group name stands for its keys, expanded in place; group and key names share
one namespace. A key asked for twice is printed once, where first asked, so
`get status homed` prints `homed` once.

```
status = state ctx homed enabled nodehomed nodeenabled head tools unlocked pos mpos wpos buf mute excluded touched mesh
```

```
state=<s>     machineState    0=IDLE 2=ESTOP 3=ALARM 5=HOMING 6=PROBING 7=JOGGING 8=TRAVERSE 9=JOB (1 and 4 retired)
ctx=<c>       machineContext  0=IDLE 1=JOB 2=PROBE 3=HOMING
enabled=<hex> axes_enabled    bitmask, bit0=X bit1=Y bit2=Z bit3=A — energised axes
homed=<hex>   axes_homed      bitmask, bit0=X bit1=Y bit2=Z bit3=A (e.g. 0x0f = all)
faults=<hex>  machineFaults   fault mask, as STATUS_RSP (e.g. 0x0004)
job=<j>       jobReason       0=BUFFERING 1=RUNNING 2=HOLDING 3=PAUSED 4=ABORTING 5=TOOL_CHANGE 6=TOOL_WAIT 7=DWELL  (only while ctx=1)
resume_layer=<l>   the layer a paused job resumes at  (only while job=3)
resume_record=<r>  the record within it  (only while job=3)
liftreason=<r>,…   the latest lift's reasons: end corner duty deadline unwind pause abort (dry reserved); `-` before the first  (only while state=JOB)
liftcount=<n>      lifts begun since the job opened  (only while state=JOB)
toolon=<0|1>       the powered tool's switch  (only while state=JOB, with a powered tool)
onms=<ms>          its duty on-time since the duty's last reset, the open span included  (as toolon)
latched=<hex> homingLatched   bitmask per slot, an axis held by a latched limit
homefail=<f>  homingFailWhy   LEGFAIL_* of the leg that failed  (only with the HOMING fault)
homenode=<n>  homingFailNode  bus id of that leg's node, 0 for a dummy leg  (with homefail)
seq=<s>       sequenceId      the running sequence, else the one that failed (with the SEQUENCE, HOMING or PROBE fault): 1=LEG 2=TRAVERSE 3=HOME 4=RECOVERY 5=GOTO 6=TOOL_CHANGE
seqfail=<r>   sequenceFailWhy what refused a sequence after its ok, one token (e.g. soft_limit)  (only with the SEQUENCE fault)
cfgerr=<r>    boot config     absent | fs | file | <decode error>  (only with the CONFIG fault)
jogging=<j>   ringOwner       1=STEP 2=CONT 3=GOTO  (only while state=JOGGING; STEP = jog/line/bez, CONT = a continuous jog, GOTO = goto)
homing=<h>    homingReason    0=LEG 1=WAIT  (only while state=HOMING; LEG = any leg runs, WAIT = none)
probing=<p>   probingReason   probe session phase  (only while PROBING or with the PROBE fault)
probe=<c>     last leg cause  PROBE_*  (with probing)
retries=<n>   last leg retries  (with probing)
psteps=<n>    last leg steps  (with probing)
probed=<0|1>  a probe height is held for the Z in slot 2
pz=<z>        that height in steps  (only with probed=1)
homecycle=<k> homingHeld      the cycle a `home` run is in  (only while one runs)
nodehomed=<hex> originMask    bitmask, bit n = bus node n holds an origin, mapped or not
nodeenabled=<hex> nodeEnabled bitmask, bit n = bus node n enabled, mapped or not
mute=<hex> excluded=<hex> touched=<hex>  bus id masks, bit n = node n (core0/ops/bus.h)
buf=<n>              planner blocks queued
queued=<us>          the motion time of the queued blocks not yet started, µs
expectedseq=<n>      the next wire seq the data plane will execute; informational, not flow control (ACKs advance the window)
pos=<x>,<y>,<z>,<a>  machinePos  steps, slot order; never a sentinel, gate on homed
mpos=<x>,<y>,<z>,<a> machine position, units (mm, deg), + physical; `-` for an unbound Z/A
wpos=<x>,<y>,<z>,<a> work position: selected tip (the blade tip with an `offsetMm`) − work offset; A the edge heading (MPos + the mounted tool's `aOffsetDeg`) folded to (−180, 180]; X/Y `-` with no head
head=<n|anchor>      the controlled point: head index, `anchor` (the laser)  (`-` with none)
tools=<id>,…         the tool id each head has mounted (`mount`), head order, `-` for none  (`-` without a config)
unlocked=<hex>       bit h = head h's holder can release now: IDLE or JOB.TOOL_WAIT, nothing running, its A homed and within `unlockRange`  (`-` with no head having one)
mesh=<m>             the bed mesh: <nx>x<ny> followed, `off` (loaded, not followed), `flat` (no /mesh.bin), `bad` (unreadable)
meshslope=<s>        its steepest slope between neighbouring points, mm/mm  (`-` without a loaded mesh)
late=<n>             planner blocks Core 1 found late, since boot; a job lands PAUSED, a jog holds and runs on
amiss=<n>            tangential curves whose tracked A ended over 0.5° from the planned A, since boot; A snapped to it
texp= tmeas= twall=           DEBUG_TIMING builds only: last burst's expected, measured and wall time (us)
```
A lift is Z rising out of a cut or a plunge for one of `liftreason`'s reasons,
with the A, tool events or XY it carries and the plunge back; one lift carries
every reason due at its spot (a BREAK's duty stop and corner swivel are one
lift, `duty,corner`). A START's travel is not a lift. `liftreason` stays on the
latest lift until the next one; the four keys reset as a job opens, not as it
resumes.

Pre-flight checks every required axis is **present** (pingnode), **enabled**
(this mask), and **homed** — a present-but-disabled axis would drop steps.

This is the read the host polls during pre-flight and the PAUSE choreography to
know what is blocking a resume. (It is the Phase 1 subset of what the Phase 2
`CMD_HANDSHAKE` also reports — minus `config_crc32`.) Fields are key=value so
the host parser tolerates later additions.

---

### Bed mesh

`/mesh.bin` (docs/config_storage.md) is a grid of bed heights the selected
tip's Z follows, once X, Y and Z are homed and `mesh on` (boot: the config's
`meshOn`). Every move follows it, jogs included.

* The offset is mesh(tip) − mesh(work origin XY), added to Z by Core 1 as X
  and Y move. Outside the grid the edge value carries on.
* `mpos` Z is the actual Z; `wpos` Z leaves the offset out, so it stays put
  over a flat-cut job. `wzero z` stores the flat Z.
* The tip, work origin and on/off are latched when a move starts from rest;
  `mesh`, `select` and `wzero` take effect from the next one.
* Each X/Y block is checked before it is queued: one along which Z would
  leave its soft range is refused (`err soft_limit z`, `NACK_SOFT_LIMIT`); a
  held jog instead ends there. The rest are slowed so Z keeps up with its
  `maxFeed` and `maxAccel`.

---

### Duplicate guard (MSEG / BEZIER seq)

Each MSEG packet carries a rolling 8-bit seq in byte [22], a BEZIER record
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
| `leg` | by intent (docs/homing.md §2.2) | ✗ | ✗ | `retract`, `unlatch`: `LIMIT_LATCHED` only | by intent (one per node) |
| `dummy_leg` | ✓ | ✗ | ✗ | `LIMIT_LATCHED` only | ✓ |
| `leg_abort` | ✓ | ✗ | ✓ | ✓ | ✓ |
| `home_end` | ✗ | ✗ | ✗ | ✗ | between legs |
| `home` / `home_unhomed` / `home_cycle` / `home_head` / `home_xy` | ✓ | ✗ | ✗ | `LIMIT_LATCHED` only | ✗ |
| `axes_map` / `slot_map` | ✓ | ✗ | ✓ | ✓ | ✗ |
| `pause` | ✗ | ✓ | ✗ | ✗ | ✗ |
| `cancel` | ✗ | ✗ | ✓ | ✗ | ✗ |
| `stop` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `unalarm` | ✗ | ✗ | ✗ | ✓ | ✗ |
| `unstop` (estop latch only) | ✗ | ✗ | ✗ | ✓ | ✗ |
| `uncfg` (CONFIG fault only) | ✗ | ✗ | ✗ | ✓ | ✗ |
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
| MSEG packets, CJOG packets | CRC8 polynomial `0x8C` | Existing implementation in `common.h` |
| CMD_SET_CONFIG, CMD_GET_CONFIG, stored flash checksum *(Phase 2)* | CRC32 | Stronger collision resistance for config correctness gate; same value used for flash storage, GET response, and MCFG header |

*(Phase 2)* Both the connect-time handshake and the job stream MCFG header compare against
the identical stored flash CRC32 — same algorithm, same byte region
(`MachineConfigFlash` struct), same reference value.
