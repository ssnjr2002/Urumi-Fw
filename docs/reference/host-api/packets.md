# Packets

The bytes of the binary records in [reference/packets.md](../packets.md): encoders,
the decoder for the status reply, the checksums, and the codes. A `Link`
uses them; a caller needs them only to inspect bytes, build a job by hand,
or write a simulator.

A packet, a record and a frame are the same thing: one binary message,
magic byte first. This page says record, as [reference/packets.md](../packets.md)
does.

```ts
import { packLayerHdr, crc8, MachineState, NACK_FULL } from "urumi-host/packets";
```

**Checked against `dc9f304`.** To see what may have changed since:

```bash
git diff dc9f304 -- web/src/packets/
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
  readonly packets: readonly Uint8Array[];
}
```

One layer of a job, ready to send: its header, the config tool's id and the
head it runs on, the box its control points fill, and its `BEZIER` records,
record n being `packets[n - hdr.record]`. The toolpath makes these; a
`JobSource` sends them.

# `MachineStatus`

A status sample, from `STATUS_RSP` or a `get` reply. A field the sample's
source can't carry is `undefined`, never zero.

## Properties

| Property | Type | |
|---|---|---|
| `state` | `MachineState` | the state |
| `context` | `MachineContext` | what is held between commands |
| `reason` | `number` | the state's reason; a `JobReason` in a job; 0 for none |
| `faults` | `number` | `Fault` bits |
| `wpos` | `[x, y, z, a]` | work position, mm and degrees; `NaN` where unknown |
| `layer`, `record` | `number` | where a job is, or its resume point; `layer` is `NO_LAYER` without a job. `STATUS_RSP` only. |
| `bufCount` | `number` | blocks queued |
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
values are the firmware's; [Names](link.md#names) has the words.

| Enum | |
|---|---|
| `MachineState` | the state |
| `MachineContext` | what is held between commands |
| `JobReason` | the reason in a job |
| `Fault` | fault bits |
| `HomeFail` | why a home failed |
| `Probing`, `ProbeCause` | the probe session's phase, and its last leg's outcome |
| `Selection` | the controlled point when it isn't a head |

`NO_LAYER` is the `layer` with no job.

# `crc8()`, `crc32()`

```ts
crc8(bytes: ArrayLike<number>, start?: number, end?: number): number
crc32(bytes: ArrayLike<number>): number
```

The controller's checksums. `crc8` over `bytes[start..end)` seals every
record; `crc32` a config or mesh blob.

# Constants

The names of [reference/packets.md](../packets.md), with the same values.

| Names | |
|---|---|
| `MAGIC_*` | each record's first byte, retired status layouts excepted |
| `*_SIZE` | each fixed-size record's length in bytes |
| `NACK_*` | a `NACK`'s reason byte |
| `NACK_DETAIL_*` | the detail bits on `NACK_OUT_OF_RANGE` |
| `CFG_NACK_*` | a `CFG_NACK`'s reason byte |
| `CJOG_SPEED_MAX` | the largest `packCjog()` speed |
| `CJOG_DEADMAN_MS` | how long the controller waits for the next `CJOG` |
| `CFG_MAX_BYTES` | the largest config blob |
