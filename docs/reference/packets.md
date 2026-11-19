# Packets

Every binary record between the host and the controller over USB: the
records the host sends, the replies the controller sends back, and how the
two share one serial stream with the text commands of
[commands.md](commands.md).

**Checked against `f1f4c59`.** To see what may have drifted since:

```bash
git diff f1f4c59 -- src/rp2350/core0/usb_protocol.h src/rp2350/core0/data_plane.cpp src/rp2350/core0/core0.cpp src/rp2350/core0/status.cpp src/rp2350/core0/ops/cjog.cpp web/src/wire/format web/src/wire/link/demux.ts
```

The controller's side is `src/rp2350/core0/usb_protocol.h`; the host's,
`web/src/wire/format/constants.ts`. The two are kept equal byte for byte.
Strings are written as the [notation](../README.md#notation) says.

## Framing

### One stream, two kinds of traffic

Text lines and binary records share the USB serial stream in both
directions. Each is whole: a record is never split by a line, nor a line by
a record.

The controller reads the stream a byte at a time. Between records, a byte
that is a known magic starts a record; any other byte, including an unknown
one with bit 7 set, is part of a text line. Inside a record every byte is
the record's, so a one-byte record (`STATUS_REQ`, `ABORT`) sent mid-record is
read as its body. Every magic has bit 7 set and every command starts with
lowercase ASCII, so the two never collide.

The host reads the controller's replies the same way (`demux.ts`): a reply
magic starts a fixed-length reply, any other byte a text line.

`0xB3` is two records: `CJOG_STOP` from the host, `CFG_ACK` from the
controller. Each side reads only the other's, so they never meet.

### Records

Every record is little-endian, magic first. The job stream's records, `CJOG`
and `STATUS_RSP` end in a CRC8 over every byte before it; the short replies,
the one-byte records and the config headers have none.

| CRC | Polynomial | Init | Used by |
| --- | --- | --- | --- |
| CRC8 | `0x8C`, reflected | `0x00`, no final XOR | the job stream, `CJOG`, `STATUS_RSP` |
| CRC32 | `0xEDB88320`, reflected | `0xFFFFFFFF`, final XOR `0xFFFFFFFF` | the config payload |

A record started and not finished within 50 ms is dropped with no reply; a
config transfer stalled for 2 s is refused with `CFG_NACK_TIMEOUT`.

### The job stream's seq and ACK

The four job-stream records (`JOB_HDR`, `TOOL_HDR`, `LAYER_HDR`, `BEZIER`)
carry a rolling 8-bit seq at byte 2, and the controller keeps `expectedSeq`,
the next seq it will take.

* **A record with the expected seq** is checked and either taken
  (`expectedSeq` moves on, an ACK is due) or refused (a NACK, `expectedSeq`
  stays).
* **A record with any other seq** is a resend of one already taken: it is not
  run again, and the controller answers at once with an ACK of the unchanged
  `expectedSeq`.
* **The ACK is cumulative:** `ACK(n)` means every record before seq n is
  taken. One ACK can confirm several records; the controller holds up to 8
  before sending one, and sends any it holds when its input runs dry, and
  always before a NACK, so an ACK never lands after the NACK that follows it.
* **`SEQRESET`** sets `expectedSeq` to 0 and answers `ACK(0)` at once. The
  host sends it before every stream.

The host's job session keeps up to 16 records unconfirmed. After 200 ms with
no reply, `NACK_CRC`, `NACK_FULL` or `NACK_ABORTING`, it resends from the
oldest unconfirmed record. On `NACK_PAUSED`, `NACK_TOOL_CHANGE` or a resume
header's `NACK_JOB` it holds, and resends when the job can take records
again. Any other NACK ends the session.

### Which check refuses first

A complete record is checked in this order; the first that fails answers.

1. The CRC: `NACK_CRC` (`JOG_NACK` with `NACK_CRC` for `CJOG`).
2. `CJOG` leaves here for its own checks ([Continuous jog](#continuous-jog)).
3. An abort under way: `NACK_ABORTING` until the machine is IDLE.
4. A job holding for a tool change: `NACK_TOOL_CHANGE`, except a header while
   it waits for the tool.
5. A paused job: `NACK_PAUSED` for anything but a `LAYER_HDR`. Any state but
   IDLE or a job: `NACK_BAD_STATE`.
6. The seq ([above](#the-job-streams-seq-and-ack)).
7. The record's own checks, in its section below.

## Replies

| Record | Bytes | Layout | Sent for |
| --- | --- | --- | --- |
| `ACK` `0xAA` | 3 | `[0xAA] [expectedSeq] [0]` | the job stream and `SEQRESET` |
| `NACK` `0xBB` | 3 | `[0xBB] [reason] [detail]` | the job stream |
| `JOG_ACK` `0xB6` | 1 | `[0xB6]` | a continuous jog started |
| `JOG_NACK` `0xB7` | 2 | `[0xB7] [reason]` | a continuous jog refused |

`ACK` and `NACK` answer only the job stream, `JOG_ACK` and `JOG_NACK` only
the continuous jog, so neither reads the other's reply as its own.

### NACK reasons

`NACK` and `JOG_NACK` share one set of reasons.

| Reason | Code | Means | Sent for |
| --- | --- | --- | --- |
| `NACK_CRC` | `0x01` | the CRC8 doesn't match | every record with one |
| `NACK_FULL` | `0x02` | the planner queue is full; resend later | `BEZIER` |
| `NACK_BAD_MAGIC` | `0x03` | reserved, never sent: an unknown byte is read as text | — |
| `NACK_PAUSED` | `0x04` | the job is paused or pausing | the job stream |
| `NACK_BAD_STATE` | `0x06` | the machine is in the wrong state for it | every record |
| `NACK_ABORTING` | `0x07` | an abort is under way; resend once IDLE | the job stream |
| `NACK_BAD_CURVE` | `0x08` | the curve fails its checks or breaks its contour | `BEZIER` |
| `NACK_SOFT_LIMIT` | `0x09` | outside the soft range or the work area, or the bed mesh would take Z out of its range | `BEZIER`, `CJOG` |
| `NACK_MIXED_AXES` | `0x0A` | directions in more than one axis set | `CJOG` |
| `NACK_BUSY` | `0x0B` | a step or sequence holds the machine | `CJOG` |
| `NACK_JOB` | `0x0C` | out of the job's order, or a bad field | the job stream |
| `NACK_TOOL_UNKNOWN` | `0x0D` | a tool id the config doesn't have | `TOOL_HDR`, `LAYER_HDR` |
| `NACK_TOOL_CHANGE` | `0x0F` | the job holds for a tool change; the header was not taken | the job stream |
| `NACK_TOOL_UNAVAILABLE` | `0x10` | the head, its A or the tool's power node can't run the tool, or the laser's node is mute or excluded | `JOB_HDR`, `TOOL_HDR` |
| `NACK_OUT_OF_RANGE` | `0x11` | no work origin keeps the job in range, or soft limits are off | `JOB_HDR`, `TOOL_HDR` |
| `NACK_NOT_HOMED` | `0x12` | an axis the job needs isn't homed | `JOB_HDR`, `TOOL_HDR` |

`0x05` and `0x0E` are unused.

A `NACK`'s byte 2 is 0, except on `NACK_OUT_OF_RANGE`:

| Bit | Set means |
| --- | --- |
| `0x01` | the Y axis (clear: X) |
| `0x02` | the high side (clear: low) |
| `0x04` | soft limits are off on that axis |

## Job stream

Host to controller. What each record does in a job, and when, is in
[system/jobs.md](../system/jobs.md); the order they come in is there too.
Each answers `ACK` or `NACK`.

| Record | Magic | Bytes |
| --- | --- | --- |
| `JOB_HDR` | `0xB8` | 17 |
| `TOOL_HDR` | `0xB9` | 21 |
| `LAYER_HDR` | `0xA1` | 20 |
| `BEZIER` | `0xAD` | 62 |

### `JOB_HDR`

Opens a job from IDLE.

| Bytes | Field | Type | Means |
| --- | --- | --- | --- |
| 0 | magic | u8 | `0xB8` |
| 1 | version | u8 | 1 |
| 2 | seq | u8 | |
| 3–6 | job id | u32 | unread |
| 7–10 | record count | u32 | unread |
| 11–14 | t | f32 | material thickness, mm; unread (`LAYER_HDR` carries it) |
| 15 | tool count | u8 | 1–4: the `TOOL_HDR`s that follow |
| 16 | CRC8 | u8 | |

| Refusal | When |
| --- | --- |
| `NACK_BAD_STATE` | not IDLE, or no config |
| `NACK_NOT_HOMED` | X, Y or a present head's Z not homed |
| `NACK_TOOL_UNAVAILABLE` | a configured laser's node is mute or excluded |
| `NACK_OUT_OF_RANGE` | soft limits off on X or Y (detail `0x04`, and the axis) |
| `NACK_JOB` | a version other than 1, or a tool count outside 1–4 |

### `TOOL_HDR`

One per tool in preflight, then one after each new layer's `LAYER_HDR`.

| Bytes | Field | Type | Means |
| --- | --- | --- | --- |
| 0 | magic | u8 | `0xB9` |
| 1 | tool | u8 | the config tool's `id` |
| 2 | seq | u8 | |
| 3 | head | u8 | the head the tool runs on |
| 4–19 | xmin, xmax, ymin, ymax | f32 × 4 | the box of the tool's control points, work mm at the tip |
| 20 | CRC8 | u8 | |

| Refusal | When |
| --- | --- |
| `NACK_PAUSED` | a pause is under way |
| `NACK_JOB` | no job open; a box not finite or with min above max; in preflight, a tool already sent; after it, no layer held, or a tool not sent in preflight |
| `NACK_TOOL_UNKNOWN` | a tool id the config doesn't have |
| `NACK_TOOL_UNAVAILABLE` | the head doesn't exist, or has no A for a tangential or chamber tool, or the tool's power node is mute or excluded |
| `NACK_NOT_HOMED` | in preflight, the head's A not homed where the tool or a tool change needs it |
| `NACK_OUT_OF_RANGE` | in preflight, no origin keeps this and the earlier tools in range; after it, the current origin doesn't (detail: axis and side) |
| `NACK_TOOL_CHANGE`, `NACK_BAD_STATE` | after preflight, as `LAYER_HDR`'s layer opening |

A refusal in preflight ends the job.

### `LAYER_HDR`

Starts a layer, held until its `TOOL_HDR`; or, in a paused job, resumes it.

| Bytes | Field | Type | Means |
| --- | --- | --- | --- |
| 0 | magic | u8 | `0xA1` |
| 1 | chamber | u8 | 1 to the tool's chamber count; 0 for a tool without chambers |
| 2 | seq | u8 | |
| 3–4 | layer | u16 | |
| 5–8 | record | u32 | the number of the next `BEZIER` in the layer |
| 9–12 | feed | f32 | cut feed, mm/s; 0 for the tool's, then the machine's |
| 13–16 | p | f32 | plunge depth below the material top, mm |
| 17–18 | t | u16 | material thickness, µm |
| 19 | CRC8 | u8 | |

| Refusal | When |
| --- | --- |
| `NACK_PAUSED` | a pause is under way |
| `NACK_JOB` | no job open, in preflight or placing; a bad chamber, feed, t or p; a layer already held, or inside a contour; resuming, not naming where the job stopped |
| `NACK_TOOL_UNKNOWN` | resuming, the current tool is no longer in the config |
| `NACK_TOOL_CHANGE` | the layer's tool is not mounted and zeroed on its head; the job holds for a change and the header is not taken |
| `NACK_BAD_STATE` | the head could not be selected |

`0xA2`, an older `LAYER_HDR` with a plunge scale in place of `p`, is retired
and read as a text byte.

### `BEZIER`

One cubic piece of a contour.

| Bytes | Field | Type | Means |
| --- | --- | --- | --- |
| 0 | magic | u8 | `0xAD` |
| 1 | flags | u8 | `0x01` START: opens a contour; `0x02` BREAK: a corner before this piece; `0x04` END: closes the contour |
| 2 | seq | u8 | |
| 3–34 | p0, p1, p2, p3 | f32 × 8 | x, y of each control point, work mm at the tip |
| 35–38 | length | f32 | arc length, mm |
| 39–42 | kappa_max | f32 | the largest curvature, 1/mm |
| 43–46 | dkappa_max | f32 | the largest rate of change of curvature, 1/mm² |
| 47–54 | c2, c3 | f32 × 2 | t(s) = c1·s + c2·s² + c3·s³; the controller derives c1 |
| 55–56 | nextBreakDist | u16 | on a START or BREAK piece, mm from p0 to the contour's next BREAK or END, rounded up; 65535 at least that far; 0 unknown, and on other pieces |
| 57–58 | sweepMin | i16 | the heading range from p0 to the next BREAK or END, whole degrees from p0's heading, unwrapped, rounded outward, saturating |
| 59–60 | sweepMax | i16 | |
| 61 | CRC8 | u8 | |

How the planner checks and runs a piece is in
[system/planner.md](../system/planner.md).

| Refusal | When |
| --- | --- |
| `NACK_PAUSED` | a pause is under way |
| `NACK_JOB` | no layer open, a layer held, or in preflight |
| `NACK_SOFT_LIMIT` | a control point outside the work area or the soft range, or the bed mesh would take Z out of its range |
| `NACK_BAD_STATE` | no head selected, or the planner refuses it for state, config or limits |
| `NACK_BAD_CURVE` | the curve fails its checks or breaks its contour's framing |
| `NACK_FULL` | the planner queue is full |

### `ABORT`

`[0xA9]`, one byte, no reply. Stops the job: the machine brakes, lifts and
goes IDLE, which status shows. Until then every job-stream record answers
`NACK_ABORTING`. It also ends an open contour.

### `SEQRESET`

`[0xA8]`, one byte. Sets `expectedSeq` to 0, forgets an open contour and
answers `ACK(0)` at once. The text command `seqreset` does the same and
answers a line.

## Continuous jog

Host to controller. A held jog: the host repeats `CJOG` while the operator
holds a direction. What a jog does is in
[machine/positioning.md](../machine/positioning.md).

| Record | Bytes | Layout | Reply |
| --- | --- | --- | --- |
| `CJOG` `0xAF` | 7 | below | `JOG_ACK` when a jog starts, `JOG_NACK` when refused, nothing for a repeat |
| `CJOG_STOP` `0xB3` | 1 | `[0xB3]` | none |

| Bytes | Field | Type | Means |
| --- | --- | --- | --- |
| 0 | magic | u8 | `0xAF` |
| 1–4 | dirX, dirY, dirZ, dirA | i8 × 4 | −1, 0 or +1 each; Z and A are the selected head's |
| 5 | speed | u8 | the axis's `jogFeed` (unhomed: `jogFeedUnhomed`) × speed / 64, capped at `maxFeed`; 0 is refused |
| 6 | CRC8 | u8 | |

* Directions in one axis set only: X and Y, Z alone, or A alone.
* A gap of 150 ms with no `CJOG`, all-zero directions, or `CJOG_STOP` stops
  the jog.
* A new direction stops the jog and starts the new one at rest; its
  `JOG_ACK` comes then.
* No seq; the abort barrier doesn't apply.

| Refusal (`JOG_NACK`) | When |
| --- | --- |
| `NACK_CRC` | the CRC8 doesn't match |
| `NACK_MIXED_AXES` | directions in more than one axis set |
| `NACK_BUSY` | a step or sequence holds the machine |
| `NACK_SOFT_LIMIT` | no room left in that direction, or the bed mesh would take Z out of its range |
| `NACK_BAD_STATE` | a direction outside −1 to +1 or a speed of 0; no config; a state that doesn't admit a step; unhomed without `jogUnhomed`; while placing a job, anything but X and Y, or any jog after `place set` |

## Status

| Record | Bytes | Layout |
| --- | --- | --- |
| `STATUS_REQ` `0xA5` | 1 | `[0xA5]`, host to controller, answered in every state |
| `STATUS_RSP` `0xA0` | 38 | below, controller to host |

`STATUS_RSP` is one sample, read at one instant. Each field mirrors a `get`
key ([wire_protocol.md](../wire_protocol.md#get-keys)).

| Bytes | Field | Type | Means | Key |
| --- | --- | --- | --- | --- |
| 0 | magic | u8 | `0xA0` | |
| 1 | state | u8 | 0 IDLE, 2 ESTOP, 3 ALARM, 5 HOMING, 6 PROBING, 7 JOGGING, 8 TRAVERSE, 9 JOB | `state` |
| 2 | reason | u8 | the state's reason (below); 0 in other states | `job`, `jogging`, `homing`, `probing` |
| 3–4 | faults | u16 | the fault mask (below) | `faults` |
| 5–6 | enabled | u16 | bit n: bus node n enabled | `enabled` |
| 7–8 | homed | u16 | bit n: bus node n holds an origin | `homed` |
| 9 | selection | u8 | the controlled point: a head index, `0xFE` the anchor, `0xFF` none | `head` |
| 10–11 | tools | u8 × 2 | the tool id mounted on each head, head order; 0 for none or no such head | `tools` |
| 12–27 | wpos | f32 × 4 | work position x, y, z, a, mm and degrees; NaN with no value | `wpos` |
| 28–29 | buf | u16 | planner blocks queued | `buf` |
| 30–31 | layer | u16 | the layer of the record the job is running; `0xFFFF` with no job | |
| 32–35 | record | u32 | that record; 0 with no job | |
| 36 | context | u8 | 0 IDLE, 1 JOB, 2 PROBE, 3 HOMING | `ctx` |
| 37 | CRC8 | u8 | | |

| State | Reason values |
| --- | --- |
| JOB | 0 BUFFERING, 1 RUNNING, 2 HOLDING, 3 PAUSED, 4 ABORTING, 5 TOOL_CHANGE, 6 TOOL_WAIT, 7 DWELL, 8 PREFLIGHT, 9 PLACE |
| JOGGING | 1 a step jog (`jog`, `line`, `bez`), 2 a continuous jog, 3 a `goto` |
| HOMING | 0 a leg runs, 1 between legs |
| PROBING | 0 a leg runs, 1 between legs with the switch closed, 2 with it open |

| Fault bit | Fault | Clears |
| --- | --- | --- |
| `0x0001` | BUS | by itself, once no mute node is left unexcluded |
| `0x0002` | CONFIG | by itself, once a config is usable |
| `0x0004` | UNMAPPED | by itself, once the slot request is met |
| `0x0008` | LATCHED | by itself, once no axis stands on its switch |
| `0x0010` | HOMING | `unalarm` |
| `0x0020` | PROBE | `unalarm` |
| `0x0040` | FACTS | `unalarm` |
| `0x0080` | SEQUENCE | `unalarm` |
| `0x0100` | TOOL | `unalarm` |
| `0x8000` | ESTOP: the e-stop latch, not a fault | `unstop` |

`layer` and `record` say how far the job has run, which the host can't tell
from what it sent: the record the running block belongs to (a travel, lift,
plunge or swivel carries the record it leads into); while paused, where the
job resumes; before any block runs, the oldest queued record or the next one
expected.

`0xA3`, `0xA4`, `0xA6` and `0xA7` are older status layouts, retired and never
sent. A reader expecting another layout fails on its magic rather than
misread the frame.

## Config

Moves the config blob between the host and the controller's flash. What the
blob is, and what a push does, is in [system/config.md](../system/config.md).

| Record | Magic | Direction | Bytes |
| --- | --- | --- | --- |
| `CFG_SET` | `0xB0` | host to controller | 9, then the payload |
| `CFG_GET` | `0xB1` | host to controller | 1 |
| `CFG_RDY` | `0xB2` | controller to host | 1 |
| `CFG_ACK` | `0xB3` | controller to host | 1 |
| `CFG_NACK` | `0xB4` | controller to host | 2: `[0xB4] [reason]` |
| `CFG_DATA` | `0xB5` | controller to host | 9, then the payload |

`CFG_SET` and `CFG_DATA` share a header:

| Bytes | Field | Type | Means |
| --- | --- | --- | --- |
| 0 | magic | u8 | `0xB0` or `0xB5` |
| 1–4 | length | u32 | payload bytes; at most 32768 |
| 5–8 | crc32 | u32 | CRC32 of the payload |

**Push.** The host sends `CFG_SET`'s header and waits. The controller answers
`CFG_RDY`, or `CFG_NACK` with `CFG_NACK_TOO_BIG` or `CFG_NACK_BAD_STATE`. On
`CFG_RDY` the host sends the payload; the controller checks the CRC, decodes
the blob, writes it to flash, answers `CFG_ACK` and soft-resets: its next
line is `ready`. Send nothing between `CFG_ACK` and `ready`.

**Pull.** The host sends `CFG_GET`. The controller answers `CFG_DATA`'s
header and the stored payload; a length of 0 means no config is stored. A
read that fails partway is padded with zeros, so the host's CRC check
refuses it.

Neither goes through the seq or the abort barrier.

| `CFG_NACK` reason | Code | When |
| --- | --- | --- |
| `CFG_NACK_CRC` | `0x01` | the payload's CRC32 doesn't match the header's |
| `CFG_NACK_TOO_BIG` | `0x02` | a length of 0 or over 32768 |
| `CFG_NACK_BAD_STATE` | `0x03` | not at rest in the idle context (faults are allowed) |
| `CFG_NACK_FLASH` | `0x04` | the filesystem is unmounted, or the write or its readback failed |
| `CFG_NACK_TIMEOUT` | `0x05` | 2 s without a byte during the transfer |
| `CFG_NACK_SCHEMA` | `0x06` | the blob doesn't decode or fails the controller's checks |

## Boot

A cold boot, `reset` and an accepted `CFG_SET` run the same sequence: input
is discarded, a banner line, the bus sweep, the config's default map (unless
a node is mute), then an unrequested `ready` line. `ready` means the sequence is done. Send nothing
between `reset` or `CFG_ACK` and `ready`. USB stays up throughout.
