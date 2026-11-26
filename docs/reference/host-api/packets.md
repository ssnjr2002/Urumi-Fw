# Packets

The bytes of the binary records in [reference/packets.md](../packets.md): encoders,
the decoder for the status reply, the checksums, the codes, and their
names. A `Link`
uses them; a caller needs them only to inspect bytes, build a job by hand,
or write a simulator.

A packet, a record and a frame are the same thing: one binary message,
magic byte first. This page says record, as [reference/packets.md](../packets.md)
does.

```ts
import { packLayerHdr, crc8, MachineState, Nack, names } from "urumi-host/packets";
```

**Checked against `e511050`.** To see what may have changed since:

```bash
git diff e511050 -- web/src/packets/
```

Every encoder returns one whole record, ending in its CRC8. A record that
carries a sequence number takes it as `seq`, default 0;
[`stampSeq()`](#stampseq) changes it later.

# Records

## Functions

| Function | Record |
|---|---|
| [`packBezier()`](#packbezier) | `BEZIER` |
| [`packJobHdr()`](#packjobhdr-packtoolhdr-packlayerhdr) | `JOB_HDR` |
| [`packToolHdr()`](#packjobhdr-packtoolhdr-packlayerhdr) | `TOOL_HDR` |
| [`packLayerHdr()`](#packjobhdr-packtoolhdr-packlayerhdr) | `LAYER_HDR` |
| [`stampSeq()`](#stampseq) | any of the four, with a new seq |
| [`packCjog()`](#packcjog) | `CJOG` |
| [`packCfgSetHeader()`](#packcfgsetheader) | `CFG_SET`'s header |

### `packBezier()`

**Syntax**

```ts
packBezier(b: BezierRecord, seq?: number): Uint8Array
```

**Parameters**

* `b`: the piece and its annotations, a `BezierRecord`:

  | Field | Type | |
  |---|---|---|
  | `curve` | `{ p0, p1, p2, p3 }`, each `{ x, y }` | the control points, mm |
  | `flags` | `number` | bit 1 START, 2 BREAK, 4 END |
  | `length` | `number` | arc length, mm |
  | `kappaMax` | `number` | largest curvature, 1/mm |
  | `dkappaMax` | `number` | largest curvature change per mm |
  | `ts` | `[c1, c2, c3]` | t(s) = s·(c1 + s·(c2 + s·c3)) |
  | `nextBreakDist` | `number` | on a START or BREAK piece, mm to the next break, at most 65535; else 0 |
  | `sweepMin`, `sweepMax` | `number` | the heading range to the next break, whole degrees from p0's heading |

  The toolpath's `AnnotatedBezier` is one.
* `seq`: default 0.

**Return value**

The 62-byte record.

### `packJobHdr()`, `packToolHdr()`, `packLayerHdr()`

**Syntax**

```ts
packJobHdr(j: JobHdr, seq?: number): Uint8Array
packToolHdr(t: ToolHdr, seq?: number): Uint8Array
packLayerHdr(l: LayerHdr, seq?: number): Uint8Array
```

**Parameters**

* `j`, a `JobHdr`: `recordCount` and `toolCount`, required; `id`;
  `thickness`, mm.
* `t`, a `ToolHdr`: `tool` (the config tool's id), `head`, and `bbox`, a
  `ToolBox` `{ minX, minY, maxX, maxY }` of the control points, mm.
* `l`, a `LayerHdr`, every field optional: `chamber`, `layer`, `record`,
  `feed` (mm/s), `p` (plunge depth below the top, mm; default
  `thickness`), `thickness` (mm, to 65.535).
* `seq`: default 0.

**Return value**

The record: 17, 21 or 20 bytes.

### `stampSeq()`

**Syntax**

```ts
stampSeq(record: Uint8Array, seq: number): Uint8Array
```

**Parameters**

* `record`: a `BEZIER`, `JOB_HDR`, `TOOL_HDR` or `LAYER_HDR` record.
* `seq`: the new sequence number, taken mod 256.

**Return value**

A copy of `record` with `seq` and its CRC8 rewritten. `record` is unchanged.

**Exceptions**

* `Error` if `record` is none of the four.

### `packCjog()`

**Syntax**

```ts
packCjog(x: -1 | 0 | 1, y: -1 | 0 | 1, speed?: number, z?: -1 | 0 | 1, a?: -1 | 0 | 1): Uint8Array
```

**Parameters**

* `x`, `y`, `z`, `a`: the direction on each axis; `z` and `a` default 0.
* `speed`: a multiple of the config's jog feed, default 1, up to
  `CJOG_SPEED_MAX`.

**Return value**

The 7-byte record. The controller stops the jog unless the next arrives
within `CJOG_DEADMAN_MS`.

### `packCfgSetHeader()`

**Syntax**

```ts
packCfgSetHeader(length: number, crc32: number): Uint8Array
```

**Parameters**

* `length`: the config blob's size in bytes, 1 to `CFG_MAX_BYTES`.
* `crc32`: the blob's [`crc32()`](#crc8-crc32).

**Return value**

The 9-byte header. The blob follows once the controller answers `CFG_RDY`.

# `JobLayer`

```ts
interface JobLayer {
  readonly hdr: LayerHdr;
  readonly tool: number;
  readonly head: number;
  readonly bbox: ToolBox;
  readonly records: readonly Uint8Array[];
}
```

One layer of a job, ready to send: its header, the config tool's id and the
head it runs on, the box its control points fill, and its `BEZIER` records,
record n being `records[n - hdr.record]`. The toolpath makes these; a
`JobSource` sends them.

# `MachineStatus`

A status sample, from `STATUS_RSP` or a `get` reply. A field the sample's
source can't carry is `undefined`, never zero.

## Properties

| Property | Type | |
|---|---|---|
| `state` | `MachineState` | the state |
| `context` | `MachineContext` | what is held between commands |
| `reason` | `number` | the state's reason: a `JobReason`, `JoggingReason`, `HomingReason` or `ProbingReason`; 0 for none |
| `faults` | `number` | `Fault` bits |
| `stateName`, `reasonName`, `faultNames` | `string` | `state`, `reason` and `faults` in words, by [`names`](#names) |
| `wpos` | `[x, y, z, a]` | work position, mm and degrees; `NaN` where unknown |
| `layer`, `record` | `number` | where a job is, or its resume point; `layer` is `NO_LAYER` without a job. `STATUS_RSP` only. |
| `bufCount` | `number` | blocks queued |
| `busy` | `boolean` | a step or sequence runs: a command that needs nothing running is refused `busy`. `false` in a paused job. |
| `placed` | `boolean` | the job is in PLACE with `place set` standing, until `place ok` or the job leaves PLACE |
| `selection` | `number` | the controlled head, or a `Selection` value |
| `tools` | `number[]` | the tool id on each head, 0 for none |
| `nodeHomed`, `nodeEnabled` | `number` | bit n = bus node n |
| `nodeLatched` | `number` | bus nodes on a limit switch. `get` only. |
| `unlocked` | `number` | bit h = head h's holder can release now. `get` only. |
| `homeFail`, `homeNode`, `homeCycle` | | why and on which node a home failed, and the cycle a home is in |
| `probing`, `probeCause`, `probeZ` | | the probe session; `probeZ` is `null` when not probed |
| `seq`, `seqFail` | | the running or failed sequence, and what refused it |

## Methods

### `MachineStatus.hasFault()`

**Syntax**

```ts
status.hasFault(mask: number): boolean
```

**Return value**

`true` if any of `mask`'s `Fault` bits is set.

# Status reply

## Functions

### `parseStatusRsp()`

**Syntax**

```ts
parseStatusRsp(record: Uint8Array): MachineStatus
```

**Parameters**

* `record`: a whole `STATUS_RSP`, `STATUS_RSP_SIZE` bytes.

**Return value**

The sample. The fields `STATUS_RSP` doesn't carry are `undefined`.

### `packStatusRsp()`

The inverse, for a simulator or a test.

**Syntax**

```ts
packStatusRsp(s: PackStatusRspOptions): Uint8Array
```

**Parameters**

* `s`: `state`, `faults` and `reason`, required; `nodeEnabled`,
  `nodeHomed`, `selection` (default `Selection.NONE`), `tools` (missing
  heads 0), `wpos` (default all `NaN`), `bufCount`, `layer` (default
  `NO_LAYER`), `record`, `context` (default `MachineContext.IDLE`).

**Return value**

The `STATUS_RSP` record.

## Enums

As const objects, each with a type of the same name for its values. The
values are the firmware's; [`names`](#names) has the words.

| Enum | |
|---|---|
| `MachineState` | the state |
| `MachineContext` | what is held between commands |
| `JobReason` | the reason in JOB |
| `JoggingReason` | the reason in JOGGING: `STEP`, `CONTINUOUS`, `GOTO` |
| `HomingReason` | the reason in HOMING: `LEG`, `WAIT` between legs |
| `ProbingReason` | the reason in PROBING: `LEG`, and between legs `CLEAR` or `CONTACT` |
| `Fault` | fault bits |
| `HomeFail` | why a home failed |
| `ProbeCause` | the probe session's last leg's outcome |
| `Selection` | the controlled point when it isn't a head |
| [`Nack`](#nack) | why a record or a jog was refused |
| `CfgNack` | why a config push or pull was refused |

`NO_LAYER` is the `layer` with no job.

# Refusals

The controller refuses a job record with a `NACK`, and a jog with a
`JOG_NACK`. Both carry a [`Nack`](#nack) code; a `NACK` also carries a
short text.

## Functions

### `parseNack()`

**Syntax**

```ts
parseNack(record: Uint8Array): Refusal
```

**Parameters**

* `record`: a whole `NACK` or `JOG_NACK`.

**Return value**

A `Refusal`:

| Property | Type | |
|---|---|---|
| `code` | `Nack` | why |
| `text` | `string` | the first check that failed, in words, such as `"y high, tool 1"` or `"chamber 9"`. Empty on a `JOG_NACK` and on the codes that carry none. For display: match `code`, never the words. |

**Exceptions**

* `Error` if `record` is neither, or its CRC8 doesn't match.

### `packNack()`

The inverse, for a simulator or a test.

**Syntax**

```ts
packNack(code: Nack, text?: string): Uint8Array
packJogNack(code: Nack): Uint8Array
```

**Parameters**

* `code`: the reason.
* `text`: default `""`, at most `NACK_TEXT_MAX` bytes of printable ASCII.

**Return value**

The record.

**Exceptions**

* `RangeError` if `text` is too long or not printable ASCII.

## `Nack`

Which record each refuses, what a job [`Session`](session.md) does with
it on the stream, and whether a `NACK` carries text. A `NACK` carries text
exactly when the session ends: the text explains a failure someone must
fix, and a code the session waits out needs none.

| Code | Value | `NACK` text | Refused because | Sent on | `Session` |
|---|---|---|---|---|---|
| `CRC` | `0x01` | no | the CRC8 doesn't match | both | resends |
| `FULL` | `0x02` | no | the planner queue is full | stream | waits, resends |
| `PAUSED` | `0x04` | no | the job is paused or pausing | stream | holds |
| `BAD_STATE` | `0x06` | yes | the machine is in the wrong state | both | ends |
| `ABORTING` | `0x07` | no | an abort is under way | stream | waits, resends |
| `BAD_CURVE` | `0x08` | yes | the curve fails its checks or breaks its contour | stream | ends |
| `MIXED_AXES` | `0x0A` | — | a jog in more than one axis set | jog | — |
| `BUSY` | `0x0B` | no | a step or sequence holds the machine: `MachineStatus.busy` | both | waits, resends |
| `TOOL_LIST` | `0x0C` | yes | a tool twice in preflight, or one preflight didn't declare | stream | ends |
| `TOOL_UNKNOWN` | `0x0D` | yes | a tool id the config doesn't have | stream | ends |
| `TOOL_CHANGE` | `0x0F` | no | the job holds for a tool change | stream | holds |
| `TOOL_UNAVAILABLE` | `0x10` | yes | the head, its A, its nodes or the tool's power can't run the tool | stream | ends |
| `OUT_OF_RANGE` | `0x11` | yes | outside the travel: no origin fits, a box or curve past the soft range, or the mesh takes Z out | both | ends |
| `NOT_HOMED` | `0x12` | yes | an axis it needs isn't homed | both | ends |
| `NO_CONFIG` | `0x13` | yes | no valid config, or the config lacks a value this needs | both | ends |
| `ORDER` | `0x14` | yes | a record out of the job's order | stream | ends |
| `BAD_FIELD` | `0x15` | yes | a field out of range or malformed | both | ends |
| `NO_RESUME` | `0x16` | no | a resume header off the resume point; the status has the point | stream | holds |

`0x03`, `0x05`, `0x09` and `0x0E` are unused. A refused jog doesn't
start, whatever its code, and a `JOG_NACK` never carries text.

# Names

The codes in words, for a display or a log. A name is the enum key, the
word the firmware logs and the wire docs use, so a person reading the UI
and one reading a capture see the same word.

```ts
import { names } from "urumi-host/packets";

show(`${names.nack(r.code)} ${r.text}`);   // "OUT_OF_RANGE y high, tool 1"
```

A value this build doesn't know prints as the enum and the number, such
as `STATE(12)`, never `""` or `undefined`: a newer firmware may send it.

| Function | Names | Unknown |
|---|---|---|
| `names.state(s: number): string` | a `MachineState` | `STATE(n)` |
| `names.reason(state: number, r: number): string` | the reason byte, read against the state: a `JobReason` in JOB, `JoggingReason` in JOGGING, `HomingReason` in HOMING, `ProbingReason` in PROBING; `""` in a state without one | `REASON(n)` |
| `names.context(c: number): string` | a `MachineContext` | `CONTEXT(n)` |
| `names.faults(mask: number): string` | the set `Fault` bits, joined with `\|`; `"NONE"` for 0 | the bit in hex, such as `0x200` |
| `names.nack(code: number): string` | a [`Nack`](#nack), from a `NACK` or a `JOG_NACK` | `NACK(n)` |
| `names.cfgNack(code: number): string` | a `CfgNack` | `CFG_NACK(n)` |
| `names.axes(mask: number): string` | an axis mask as letters in `xyza` order, such as `"xz"`; `"—"` for none | ignored |

A `NACK`'s text is already words: show it after the name, as above.
`MachineStatus` has the first three ready as `stateName`, `reasonName`
and `faultNames`.

# `crc8()`, `crc32()`

```ts
crc8(bytes: ArrayLike<number>, start?: number, end?: number): number
crc32(bytes: ArrayLike<number>, start?: number, end?: number, crc?: number): number
```

The controller's checksums, each over `bytes[start..end)`, by default the
whole of `bytes`. `crc8` seals every record. `crc32` guards a config or
mesh blob, and makes a job's id; pass an earlier result as `crc` to carry
it on over more bytes.

# Constants

The names of [reference/packets.md](../packets.md), with the same values.

| Names | |
|---|---|
| `MAGIC_*` | each record's first byte, retired status layouts excepted |
| `*_SIZE` | each fixed-size record's length in bytes |
| `NACK_TEXT_MAX` | the longest text a `NACK` carries, 48 bytes |
| `CJOG_SPEED_MAX` | the largest `packCjog()` speed |
| `CJOG_DEADMAN_MS` | how long the controller waits for the next `CJOG` |
| `CFG_MAX_BYTES` | the largest config blob |
