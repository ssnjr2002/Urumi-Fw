# Host API: the web library's public surface

Open. No branch started.

The web library (`web/`, package `urumi-host`) exports about 150 names from
one root, `src/index.ts`. Its one real user, the demo console, imports 22 of
them and reaches past the API for the rest: it scans the namespace for
`NACK_` constants to name errors, encodes and checksums a config itself
before pushing it, and reads link internals (`sinks`, `textDesyncs`). The
README's job example no longer matches the code.

This plan writes the API we want as a reference first, then changes the code
to match it, one subpath per branch. Each branch moves its subpath's
reference to `docs/reference/host-api/`, checked against that branch's
commit; once all have, this plan gets its "Current behaviour" line.

## Decisions

* **Flat subpaths, no root.** Every import names its layer:
  `urumi-host/link`, not `urumi-host`. Each subpath is a folder under
  `web/src/` with an `index.ts`; what that file doesn't export is private.
  Nesting was rejected: `config` and `packets` are used with no link (the
  flashed-config script, `prepareBezierJob`), and a transport needs only
  the interface.
* **Six subpaths**, each depending only on those listed after it:

  | Subpath | Depends on |
  |---|---|
  | `transport` | `packets` (sim only) |
  | `packets` | nothing |
  | `config` | `packets` |
  | `toolpath` | `packets`, `config` |
  | `link` | `transport`, `packets`, `config` |

  `transport` has three more entries, one per transport:
  `transport/webserial`, `transport/node`, `transport/sim`.
* **Names.** "Backend" becomes `transport`, the name its interface and
  classes already have. "Format" becomes `packets`, matching
  `reference/packets.md`, which documents the same layouts. Rejected:
  `wire` (the wire page covers the link too), `protocol` (the protocol's
  seq and ACKs live in `link`), `codec`.
* **The caller owns the port.** A transport opens it; `new Link(transport)`
  wraps it, the same in the browser and in Node. `openNodeLink()`, which
  returns a `Link` from the transport layer, goes.
* **Written top-down, read bottom-up.** The page lists the subpaths in
  dependency order, `transport` first. They are written in the demo's
  order: `link` first, since the demo's friction is there and its
  decisions set what the lower layers expose; `transport` and `packets`
  last.
* **Done** when the demo, rewritten from the reference alone, imports only
  names the reference lists.

Modelled on libraries in the same position (TypeScript, a byte link to
hardware, browser and Node): Tango (ya-webadb) for a protocol core with
transports as separate entries; esptool-js for the caller passing in a
transport and policy; node-serialport for a published transport interface
that a simulator implements; node-mavlink for packet encoding kept apart
from the stream.

## Layout

Where today's files go. Each folder's `index.ts` is new.

```
web/src/
├── transport/      ← wire/link/backends/, wire/link/transport.ts
│   ├── index.ts        the Transport interface
│   ├── webserial.ts
│   ├── node.ts         opens a port; no longer imports Link
│   └── sim.ts          takes its own type instead of link's Demux
├── packets/        ← wire/format/ (less names.ts)
│   ├── constants.ts, crc.ts, packet.ts
│   └── bezier.ts, layer.ts, cjog.ts, cfg.ts, status.ts
├── link/           ← wire/link/
│   ├── link.ts, commands.ts, session.ts
│   ├── names.ts        ← wire/format/names.ts
│   └── demux.ts, writer.ts, sink.ts, settled.ts   private
├── toolpath/
│   ├── svg.ts          ← svg/load.ts
│   ├── job.ts
│   └── clean.ts, annotate.ts, heading.ts, geometry.ts   private
└── config/         ← machine/, with json/ flattened into it
    ├── schema.ts, defaults.ts, tools.ts, resolve.ts, runtime.ts
    ├── load.ts, validate.ts, save.ts, blob.ts, image.ts, mesh.ts
    ├── runtimeHeader.ts   for scripts/runtime-fields.ts
    └── f32.ts             private
```

`src/index.ts` is deleted; `package.json`'s `exports` lists the subpaths.

## Where the API fails its user

Found by reading the demo (`web/demo/index.js`) against the library. Each
item is what the demo does today, why that is the library's fault, and the
change. Line numbers are the demo's at `4613119`.

### Building a job

1. **The demo assembles the job itself** (`buildJob()`, line 725). For each
   layer row it looks up the tool profile from the pulled config, checks a
   plunging tool has a depth, checks no tool sits on two heads, calls
   `prepareBezierJob()`, and builds a `LAYER_HDR` by hand (`layer: i`,
   `record: 0`, chamber, feed, `p`, thickness). Every caller would repeat
   this, and the header's fields are the wire's, not the app's.

   **Change:** a `toolpath` function that takes the SVG, the config, and
   one entry per layer (key, head, feed, depth) and returns the job's
   layers or a list of errors. The head and plunge checks move into it;
   `JobSource` keeps its own tool-on-two-heads check as the last guard.
   The SVG is parsed once, not once per layer.
2. **`toolLayer(key, toolNames())` and `loadSvgPaths(svg, { tools })`** take
   the tool names, which the demo derives from the pulled config each time
   (line 560). **Change:** they take the config, or the names come from one
   place in it.
3. **`new JobSource(job.layers, 0, { manual })`** (line 818): the `0` is the
   job id, positional, and the demo never sets it. **Change:** the id moves
   into the options object.
4. **`await link.resetSeq()` before every job** (line 817). `link.stream()`
   resets the seq itself; `link.session()` doesn't, so a caller who forgets
   opens on a stale ACK. **Change:** `link.session()` resets it, and
   `resetSeq()` becomes private.

### Following a job

5. **The demo works out "done" itself** (`jobDry()`, line 704): job context,
   `BUFFERING`, an empty buffer, every emitted record acked. **Change:**
   the session reports it, as a state the caller reads (`session.phase`
   or the like: running, held with its hold, waiting for release, done).
6. **Status updates come from `link.sinks.status`** (`sample`,
   `waitUpdate()`, line 794), a demux internal. **Change:** a public way to
   wait for or subscribe to status, on `Link`.
7. **Holds are bare strings** (`'place'`, `'tool'`, …) that the demo maps
   to hints. That is fine; the change is only that the reference lists
   them.

### Errors and names

8. **NACK names come from scanning the namespace** for `NACK_` and
   `CFG_NACK_` constants (lines 281, 411). **Change:** `nackName()` and
   `cfgNackName()`, beside `stateName()` and `faultNames()`.
9. **A config refusal is found by duck typing** (`e?.reason`, line 422).
   `ConfigTransferError` exists and is exported, but nothing says so.
   **Change:** none in code; the reference names the error, and its name
   helper is item 8.

### Config

10. **Push takes a blob, pull returns a blob.** The demo calls
    `encodeConfigBlob()` before `pushConfig()` and `decodeConfigBlob()`
    after `pullConfig()` (lines 435, 456). Its `crc32()` call is only for
    its log. **Change:** `pushConfig(config)` and `pullConfig()` take and
    return the config; the blob forms stay in `config` for the image
    scripts and for a caller that wants the bytes.
11. **Loading a file is two calls:** `loadConfig(stripJsonComments(text))`
    (line 429). **Change:** `loadConfig()` accepts JSONC.

### The link

12. **Jog replies come from `link.sinks.jog`** (line 308). **Change:** a
    public way to read them, like status in item 6.
13. **`stop` goes through `link.send()`** (line 151), because it must
    never wait behind a pending command and gets no reply. **Change:**
    `link.stop()`; `send()` stays public only if something else needs a
    reply-less line.
14. **`link.textDesyncs`** (line 157) is a counter the demo shows. **Change:**
    keep it, as a documented diagnostic, or fold it into a `link.stats()`.
15. **`Link` exposes `sinks`, `demux` and `writer`** as public fields.
    **Change:** private, once items 6 and 12 give the demo another way in.

### The README

16. **The README's job example** builds `JobSource` from `{ hdr, tool,
    head, bbox, packets }` with `hdr`, `tool` and `head` left for the
    reader to invent, the same work as item 1. **Change:** rewritten from the
    reference once items 1–4 land.

Out of the demo's reach and not judged here: the tests and
`web/scripts/` use the inner stages (`cleanSubpath`, `annotate`, the packet
encoders). Their exports are decided per subpath under Method.

## Method

For each subpath, in writing order (`link`, `toolpath`, `config`,
`transport`, `packets`):

1. **Inventory:** every name its files export today, and who uses it (the
   demo, the tests, `web/scripts/`, nothing).
2. **Decide** each name: keep, rename, merge, hide or add.
3. **Write** the reference entries for what is kept.
4. **List** the changes under Changes.

## The reference

The API as planned. Each subpath's section is a draft for its page; its
branch moves it out.

**Pages.** One page per subpath, as Node.js and MDN do, in
`docs/reference/host-api/`: `README.md` (the index, units, errors,
waiting), `transport.md`, `packets.md`, `config.md`, `toolpath.md`,
`link.md`. One page was rejected: the `link` section alone is about 500
lines.

**Style.** The reference conventions (`docs-restructure.md`, "Conventions:
reference pages") with MDN's anatomy, settled on the `link` draft:

* A class has a description, then Constructor, Properties (a table) and
  Methods (a table linking each method).
* A method or function with something to say has its own entry under
  bold labels: **Syntax**, **Parameters**, **Return value**,
  **Exceptions**, **Example**. A one-liner stays a row in its class's
  Methods table.
* Constants, enums, name helpers and encoders are tables.
* Three heading levels per page, all of them used: `#` the page intro
  and each class or function, `##` its Constructor, Properties or
  Methods, `###` a member's entry. Deeper levels render too small to tell
  apart, and a page title at `#` over everything would waste one. The
  intro's title must not share a class's anchor (`Transports`, not
  `Transport`).

In this plan the drafts keep their old, deeper levels; each branch lifts
its page to these. Only `link`, `transport`, `packets`, `config` and `toolpath` are in the new style yet.

### Index

| Subpath | Holds | Depends on |
|---|---|---|
| [`urumi-host/transport`](#urumi-hosttransport) | the `Transport` interface | — |
| [`urumi-host/transport/webserial`](#urumi-hosttransportwebserial) | `WebSerialTransport` | `urumi-host/transport` |
| [`urumi-host/transport/node`](#urumi-hosttransportnode) | `NodeSerialTransport` | `urumi-host/transport` |
| [`urumi-host/transport/sim`](#urumi-hosttransportsim) | `SimTransport` | `urumi-host/transport`, `urumi-host/packets` |
| [`urumi-host/packets`](#urumi-hostpackets) | packet layouts, encoders and decoders, the CRCs, `MachineStatus` and its enums | — |
| [`urumi-host/config`](#urumi-hostconfig) | the controller config: load, validate, save, encode, the flashed images | `urumi-host/packets` |
| [`urumi-host/toolpath`](#urumi-hosttoolpath) | an SVG to a job's layers | `urumi-host/packets`, `urumi-host/config` |
| [`urumi-host/link`](#urumi-hostlink) | `Link`, job sessions, names for codes, text-command helpers, waiting for the machine | `urumi-host/transport`, `urumi-host/packets`, `urumi-host/config` |

No subpath depends on one below it in this table, except the transports
on `urumi-host/packets`. There is no root import: every import names its
subpath.

```ts
import { WebSerialTransport } from "urumi-host/transport/webserial";
import { loadConfig } from "urumi-host/config";
import { buildJob } from "urumi-host/toolpath";
import { Link, JobSource } from "urumi-host/link";
```

### Shared

#### Units

| Quantity | Unit |
|---|---|
| Position, length, depth, thickness | mm |
| Feed | mm/s |
| Angle | degrees, except where a field says radians |
| Time | ms |
| Coordinates | machine work coordinates, Y up |

#### Errors

Two kinds, and each function says which:

* **Results** for input a caller should expect to be wrong (a file, an SVG,
  a job's layers): `{ ok: true, … }` or `{ ok: false, errors }`, where
  `errors` is a list of strings for a person to read.
* **Throws** for a refusal from the machine, a timeout, or a programming
  mistake. A refusal from the controller throws an error carrying its code;
  [Names](#names) turns the code into a word.

#### Waiting

A `Link` call that needs a reply returns a promise and takes a timeout
(`timeoutMs`). A call that needs none (`stop()`, `abort()`, `cjog()`)
returns at once. Nothing on this page uses callbacks: a caller that follows
the machine awaits the next sample ([`nextStatus()`](#nextstatus)).

### `urumi-host/transport`

#### `Transport`

```ts
interface Transport {
  write(bytes: Uint8Array): Promise<void>;
  read(): AsyncIterable<Uint8Array>;
  close(): Promise<void>;
}
```

A byte link to the controller. `read()` yields chunks as they arrive, in
order, and ends when the port closes. Implement it to run a `Link` over
anything else (a socket, a recording).

### `urumi-host/transport/webserial`

#### `WebSerialTransport`

```ts
WebSerialTransport.request(baudRate = 115200): Promise<WebSerialTransport>
WebSerialTransport.fromPort(port: SerialPort): WebSerialTransport
```

`request()` opens the browser's port picker, then the port. It must run
from a user gesture (a click). `fromPort()` wraps a port already open.

### `urumi-host/transport/node`

#### `NodeSerialTransport`

```ts
NodeSerialTransport.open(path: string, baudRate = 115200): Promise<NodeSerialTransport>
```

Opens a serial port in Node through the `serialport` package, which the
caller installs; the import fails with a message naming it otherwise.

### `urumi-host/transport/sim`

#### `SimTransport`

```ts
new SimTransport(options?: SimOptions)
```

A simulated controller: answers status, config, jogs and the job stream
as the firmware does, with no hardware. For tests and offline work. Its
options (`busNodes`, ring size, frame time) are listed on the type.

```ts
const link = new Link(new SimTransport());
```

### `urumi-host/packets`

Encoders and decoders for the records in [packets.md](../reference/packets.md). A
`Link` uses them; a caller needs them only to inspect bytes or to write
another transport. Every encoder returns one frame with seq 0, ending in its
CRC.

#### Frames

| Function | Packet |
|---|---|
| `packBezier(b: AnnotatedBezier): Uint8Array` | `BEZIER` |
| `packJobHdr(j: JobHdr, seq = 0): Uint8Array` | `JOB_HDR` |
| `packToolHdr(t: ToolHdr, seq = 0): Uint8Array` | `TOOL_HDR` |
| `packLayerHdr(l: LayerHdr, seq = 0): Uint8Array` | `LAYER_HDR` |
| `packCjog(x, y, speed, z?, a?): Uint8Array` | `CJOG` |
| `packCfgSetHeader(len, crc32): Uint8Array` | `CFG_SET` header |
| `parseStatusRsp(frame: Uint8Array): MachineStatus` | `STATUS_RSP` |
| `packStatusRsp(s: MachineStatus): Uint8Array` | `STATUS_RSP`, for a simulator |
| `stampSeq(frame: Uint8Array, seq: number): void` | writes seq and the CRC in place |

#### `crc8`, `crc32`

```ts
crc8(bytes: Uint8Array, start = 0, end = bytes.length): number
crc32(bytes: Uint8Array): number
```

The controller's checksums: `crc8` seals every record, `crc32` a config or
mesh blob.

#### `JobLayer`

```ts
interface JobLayer {
  readonly hdr: LayerHdr;
  readonly tool: number;          // the config tool's id
  readonly head: number;
  readonly bbox: ToolBox;
  readonly packets: readonly Uint8Array[];
}
```

One layer of a job, ready to send: its header, the tool and head it runs on,
the box its control points fill, and its `BEZIER` records.
[`buildJob()`](#buildjob) makes these; [`JobSource`](#jobsource) sends
them.

#### `MachineStatus`

A status sample, from `STATUS_RSP` or a `get` reply. A field the sample's
source can't carry is `undefined`, never zero.

| Field | Type | Holds |
|---|---|---|
| `state` | `MachineState` | the state |
| `context` | `MachineContext` | what is held between commands |
| `reason` | `number` | the state's reason; a `JobReason` in a job |
| `faults` | `number` | `Fault` bits |
| `wpos` | `[x, y, z, a]` | work position; `NaN` where unknown |
| `layer`, `record` | `number` | where a job is; `layer` is `NO_LAYER` without one |
| `bufCount` | `number` | blocks queued |
| `selection` | `number` | the controlled head, or a `Selection` value |
| `tools` | `number[]` | the tool id on each head, 0 for none |
| `nodeHomed`, `nodeEnabled`, `nodeLatched` | `number` | bit n = bus node n |
| `homeFail`, `homeNode`, `homeCycle` | | why and where a home failed, and its cycle |
| `probing`, `probeCause`, `probeZ` | | the probe session |
| `seq`, `seqFail` | | the running or failed sequence |

The enums, as const objects: `MachineState`, `MachineContext`,
`JobReason`, `Fault`, `HomeFail`, `Probing`, `ProbeCause`, `Selection`,
and `NO_LAYER`. Their values are the firmware's; [Names](#names) has the
words.

#### Constants

The magics, sizes and reply codes of [packets.md](../reference/packets.md), under the
same names: `MAGIC_*`, `*_SIZE`, `NACK_*`, `CFG_NACK_*`, `CJOG_SPEED_MAX`,
`CJOG_DEADMAN_MS`, `CFG_MAX_BYTES`.

### `urumi-host/config`

The controller's config ([config](../system/config.md)): a JSON or JSONC
file, a `ControllerConfig` in memory, and the blob the controller stores.

#### `loadConfig`

```ts
loadConfig(text: string): LoadResult

type LoadResult =
  | { ok: true; config: ControllerConfig; warnings: string[] }
  | { ok: false; errors: string[] }
```

Parses a JSON or JSONC file and validates it. A warning names something
legal but probably meant otherwise.

```ts
const r = loadConfig(await file.text());
if (!r.ok) return show(r.errors);
```

#### `validateConfig`

```ts
validateConfig(config: ControllerConfig): { errors: string[]; warnings: string[] }
```

The checks `loadConfig()` runs, for a config built or edited in code.

#### `configToFile`

```ts
configToFile(config: ControllerConfig): string
```

The config as JSON text, for saving; `loadConfig()` reads it back
unchanged.

#### `encodeConfig`, `decodeConfig`

```ts
encodeConfig(config: ControllerConfig): Uint8Array
decodeConfig(blob: Uint8Array): ControllerConfig
```

The blob the controller stores. `Link.pushConfig()` and `pullConfig()`
call these; a caller needs them only for the bytes themselves.
`decodeConfig()` throws on a blob that isn't one.

#### `configImage`, `meshImage`

```ts
configImage(text: string): Uint8Array
meshImage(text: string): Uint8Array
```

The controller's `/config.bin` and `/mesh.bin` from a config or mesh file,
for flashing (`web/scripts/`, run by `uploadfs`; see
[host build](../system/host-build.md)). Both throw on a bad file.

#### Types and tables

| Name | What |
|---|---|
| `ControllerConfig` | the whole config: `machine`, `toolProfiles`, `quality` |
| `MachineConfig`, `AxisConfig`, `HomingConfig`, `BusNode`, `ToolHead` | its parts |
| `ToolProfile`, `ToolType` | a tool, and its kind |
| `QualityConfig` | the toolpath's tolerances |
| `PEN`, `KNIFE`, `CREASE`, `REVOLVER_PEN`, `TOOL_PROFILES` | the built-in tool profiles |
| `DEFAULTS` | the values a file may leave out |

#### `renderRuntimeHeader`

```ts
renderRuntimeHeader(): string
```

The controller's `runtime_fields.h`, from the runtime field table. Used by
`pnpm gen:runtime` only.

### `urumi-host/toolpath`

An SVG to a job's layers ([toolpath](../system/toolpath.md)).

#### `loadSvg`

```ts
loadSvg(svgText: string, config: ControllerConfig): LoadedSvg

interface LoadedSvg {
  readonly layers: ToolLayer[];       // document order
  readonly warnings: string[];
}
interface ToolLayer {
  readonly key: string;               // the group's name, e.g. "revolver_pen/slot1"
  readonly tool: string;              // a toolProfiles name
  readonly chamber: number;           // 0 for a tool without chambers
}
```

The SVG's tool layers: each group whose name is one of the config's tools,
with its chamber. Throws on a malformed SVG. Loading again for each layer
isn't needed: `buildJob()` takes the text.

#### `buildJob`

```ts
buildJob(svgText: string, config: ControllerConfig, spec: JobSpec): JobResult

interface JobSpec {
  readonly thickness: number;          // material, mm
  readonly offset?: Pt;                // added to every point
  readonly layers: readonly LayerSpec[];
}
interface LayerSpec {
  readonly key: string;                // a ToolLayer key
  readonly head: number;
  readonly feed?: number;              // mm/s; 0 or absent = the tool's cutFeed
  readonly depth?: number;             // plunge below the top, mm; absent = thickness
}
type JobResult =
  | { ok: true; layers: JobLayer[]; bbox: BBox }
  | { ok: false; errors: string[] }
```

A job's layers, in the order given, ready for a `JobSource`. It checks
what a caller would otherwise check by hand:

* each key is a tool layer of the SVG and has geometry;
* a plunging tool has a depth above 0;
* no tool is on two heads;
* 1 to 4 tools in all.

Every problem is reported, not only the first. A refused curve
([toolpath](../system/toolpath.md)) is an error naming its layer.

```ts
const job = buildJob(svg, config, {
  thickness: 3,
  layers: [{ key: "knife", head: 0 }, { key: "pen", head: 1, feed: 40 }],
});
if (!job.ok) return show(job.errors);
const session = link.session(new JobSource(job.layers));
```

#### `prepareBezierJob`

```ts
prepareBezierJob(svgText: string, key: string, options?: BezierJobOptions):
  { ok: true; packets: Uint8Array[]; bbox: BBox } | { ok: false; errors: string[] }
```

One layer's `BEZIER` records with no header: the step `buildJob()` runs
per layer. For tools and tests that want the records alone.

### `urumi-host/link`

A connection to the controller ([wire](../system/wire.md)): the link, job
sessions, words for codes, and helpers over text commands.

| Name | Kind |
|---|---|
| [`Link`](#link) | class |
| [`JobSource`](#jobsource) | class |
| [`Session`](#session) | class |
| [`ConfigTransferError`](#configtransfererror) | error |
| [Names](#names) | functions |
| [Text-command helpers](#text-command-helpers) | functions |
| [`settle()`](#settle), [`waitAtRest()`](#waitatrest) | functions |

#### `Link`

One connection to the controller over a transport. It reads from the
transport from construction until the port closes. One `Link` per port.

##### Constructor

```ts
new Link(transport: Transport)
```

**Parameters**

* `transport`: an open [`Transport`](#transport).

**Example**

```ts
const link = new Link(await WebSerialTransport.request());
```

##### Properties

| Property | Type | |
|---|---|---|
| `closed` | `boolean` | the port has closed; every later call fails |
| `status` | `MachineStatus \| null` | the latest status sample, without asking; `null` before the first |

##### Methods

| Method | |
|---|---|
| [`command()`](#linkcommand) | send a text command, resolve to its reply |
| [`stop()`](#linkstop) | e-stop |
| [`abort()`](#linkabort) | soft stop to IDLE |
| [`getStatus()`](#linkgetstatus) | ask for a status sample |
| [`nextStatus()`](#linknextstatus) | wait for the next status sample |
| [`cjog()`](#linkcjog), [`cjogStop()`](#linkcjogstop) | continuous jog |
| [`nextJogReply()`](#linknextjogreply) | wait for the next jog reply |
| [`pushConfig()`](#linkpushconfig), [`pullConfig()`](#linkpullconfig) | store or fetch the config |
| [`session()`](#linksession) | a job session |
| [`stats()`](#linkstats) | counters for diagnostics |
| [`close()`](#linkclose) | close the port |

##### `Link.command()`

Sends one text command and resolves to its reply. Commands run one at a
time, in call order.

**Syntax**

```ts
command(line: string, timeoutMs?: number): Promise<string>
```

**Parameters**

* `line`: a command from [commands.md](../reference/commands.md), without
  the newline.
* `timeoutMs`: how long to wait for the reply. Default 1000.

**Return value**

The reply line, trimmed; `""` on a timeout.

**Exceptions**

* `Error` if the link is closed.

**Example**

```ts
const reply = await link.command("home xy");
if (reply !== "ok") show(reply);
```

##### `Link.stop()`

The e-stop. Goes out at once, never behind a pending command.

**Syntax**

```ts
stop(): void
```

**Return value**

None. `stop` has no reply; the status shows it take hold (`ESTOP`, then
`ALARM`).

##### `Link.abort()`

Ramps to rest, drops the queued motion, and lands IDLE with position kept.

**Syntax**

```ts
abort(): void
```

**Return value**

None. No reply; the status shows the machine settle. Records sent before
IDLE are refused with `NACK_ABORTING`.

##### `Link.getStatus()`

Asks for a status sample.

**Syntax**

```ts
getStatus(timeoutMs?: number): Promise<MachineStatus>
```

**Parameters**

* `timeoutMs`: default 1000.

**Return value**

The sample ([`MachineStatus`](#machinestatus)).

**Exceptions**

* `Error` on a timeout.

##### `Link.nextStatus()`

Waits for the next status sample to arrive, whoever asked for it. A
running job session asks on its own, so a page following a job only waits.

**Syntax**

```ts
nextStatus(timeoutMs: number): Promise<MachineStatus | null>
```

**Parameters**

* `timeoutMs`: how long to wait.

**Return value**

The sample, or `null` if none arrived in time.

**Example**

```ts
while (!link.closed) show(await link.nextStatus(500) ?? link.status);
```

##### `Link.cjog()`

Sends one continuous-jog packet. Repeat it more often than
`CJOG_DEADMAN_MS` while a direction is held; the machine stops when the
packets stop.

**Syntax**

```ts
cjog(x: JogDir, y: JogDir, speed?: number, z?: JogDir, a?: JogDir): void

type JogDir = -1 | 0 | 1
```

**Parameters**

* `x`, `y`, `z`, `a`: the direction on each axis. One axis set per call:
  XY, Z or A. All zeros stops.
* `speed`: a fraction of the jog speed, up to `CJOG_SPEED_MAX`. Default 1.

**Return value**

None. The controller's answer arrives through
[`nextJogReply()`](#linknextjogreply).

##### `Link.cjogStop()`

Stops a continuous jog at once.

**Syntax**

```ts
cjogStop(): void
```

**Return value**

None; no reply.

##### `Link.nextJogReply()`

Waits for the controller's next answer to a `cjog()`: it acknowledges a
start and refuses with a reason.

**Syntax**

```ts
nextJogReply(timeoutMs: number): Promise<JogReply | null>

type JogReply = { ok: true } | { ok: false; reason: number }
```

**Parameters**

* `timeoutMs`: how long to wait.

**Return value**

The reply, or `null` if none arrived in time. `reason` is a `NACK_*` code;
[`nackName()`](#names) names it.

##### `Link.pushConfig()`

Encodes the config and stores it on the controller.

**Syntax**

```ts
pushConfig(config: ControllerConfig): Promise<{ bytes: number; crc32: number }>
```

**Parameters**

* `config`: a config from [`loadConfig()`](#loadconfig) or built in code.

**Return value**

The stored blob's size and CRC32, once the controller has written it to
flash and read it back.

**Exceptions**

* [`ConfigTransferError`](#configtransfererror) on a refusal or no reply.

**Example**

```ts
const r = loadConfig(await file.text());
if (r.ok) await link.pushConfig(r.config);
```

##### `Link.pullConfig()`

Fetches the stored config.

**Syntax**

```ts
pullConfig(): Promise<ControllerConfig | null>
```

**Return value**

The config, or `null` when none is stored.

**Exceptions**

* [`ConfigTransferError`](#configtransfererror) on no reply or a blob that
  fails its CRC.

##### `Link.session()`

Opens a job session on this link. Nothing is sent until its `run()`.

**Syntax**

```ts
session(source: JobSource): Session
```

**Parameters**

* `source`: the job ([`JobSource`](#jobsource)).

**Return value**

A [`Session`](#session). Its `run()` resets the stream's seq first.

##### `Link.stats()`

**Syntax**

```ts
stats(): LinkStats
```

**Return value**

Counters for diagnostics; `textDesyncs` is the number of reply lines that
arrived with no command waiting for them, a sign of a firmware reply with
extra lines.

##### `Link.close()`

**Syntax**

```ts
close(): Promise<void>
```

**Return value**

Resolves once the port is closed.

#### `JobSource`

A job's frames, in order: `JOB_HDR`, one `TOOL_HDR` per tool, then each
layer's header and records; and the headers again for a resume.

##### Constructor

```ts
new JobSource(layers: readonly JobLayer[], options?: { id?: number; manual?: boolean })
```

**Parameters**

* `layers`: the layers, from [`buildJob()`](#buildjob).
* `options.id`: the job id the controller reports. Default 0.
* `options.manual`: hold each layer until `release()`. Default false.

**Exceptions**

* `RangeError` if a tool is on two heads, or there are not 1 to 4 tools.

##### Properties

| Property | Type | |
|---|---|---|
| `manual` | `boolean` | hold each layer until `release()`; may change mid-job |

##### Methods

| Method | |
|---|---|
| `release(): void` | let the next layer go, in manual mode |

#### `Session`

A job running on the link ([jobs](../system/jobs.md)). Made by
[`Link.session()`](#linksession).

##### Properties

| Property | Type | |
|---|---|---|
| `phase` | `Phase` | where the job is |
| `held` | `Hold \| null` | why it is held, while `phase` is `"held"` |

```ts
type Phase = "sending" | "held" | "waiting" | "finishing" | "done"
type Hold = "place" | "tool" | "paused" | "refused" | "silent"
```

| Phase | Means |
|---|---|
| `sending` | records are going out |
| `held` | the machine takes no records; `held` says why |
| `waiting` | manual mode: the next layer waits for `release()` |
| `finishing` | everything is sent; the machine is still cutting |
| `done` | the last layer is cut; the job stays open until `cancel()` or `end()` |

| Hold | Means | Leave by |
|---|---|---|
| `place` | the job waits to be placed | `place set`, then `place ok` ([commands.md](../reference/commands.md)) |
| `tool` | a tool change waits for the operator | mount and zero, then `resume()` |
| `paused` | paused | `resume()` |
| `refused` | a layer header was refused | `resume()` to retry |
| `silent` | no replies; the session is resending | nothing; it clears itself |

##### Methods

| Method | |
|---|---|
| [`run()`](#sessionrun) | send the job; resolve when it ends |
| `resume(): boolean` | leave a `paused`, `refused` or `tool` hold; false if not possible now |
| `pause(): Promise<string>` | send `pause`; resolves to the reply |
| `cancel(): Promise<string>` | send `cancel`, which ends a job at rest; resolves to the reply |
| `end(): void` | abort the job: brake, lift, close |
| `stats(): SessionStats` | `sent`, `acked`, `nacks`, `retries` so far |
| [`result()`](#sessionresult) | how it ended |

##### `Session.run()`

Resets the stream's seq, sends the job, and follows it until it closes. It
holds, and resolves only when the job leaves the machine.

**Syntax**

```ts
run(): Promise<boolean>
```

**Return value**

`true` when the job closed (by `cancel()`, `end()`, or the job ending);
`false` on a fatal error, named by `result().fatal`.

**Example**

```ts
const session = link.session(new JobSource(job.layers));
if (!await session.run()) show(fatalName(session.result().fatal!));
```

##### `Session.result()`

**Syntax**

```ts
result(): SessionResult
```

**Return value**

After `run()`: the stats, `truncated`, and `fatal`, a code, on failure.

#### `ConfigTransferError`

Thrown by [`pushConfig()`](#linkpushconfig) and
[`pullConfig()`](#linkpullconfig).

| Property | Type | |
|---|---|---|
| `reason` | `number \| null` | a `CFG_NACK_*` code; `null` when the controller didn't answer |
| `message` | `string` | what failed |

#### Names

A word for each code, for logs and displays. An unknown code comes back in
hex, never as a known word.

| Function | Names |
|---|---|
| `stateName(s: MachineState): string` | a state |
| `jobName(r: JobReason): string` | a job reason |
| `faultNames(faults: number): string` | the set fault bits, or `"none"` |
| `nackName(code: number): string` | a `NACK_*` code |
| `cfgNackName(code: number): string` | a `CFG_NACK_*` code |
| `fatalName(code: number): string` | a session's fatal code |

#### Text-command helpers

Typed wrappers over [`command()`](#linkcommand), one per command in
[commands.md](../reference/commands.md). Each takes the link first, parses
the reply, and throws when the reply is a refusal: `ping`, `get`,
`getStatus`, `enable`, `disable`, `unalarm`, `jog`, `setOrigin`, the leg
commands, the probe commands and the peripherals. Their parameters are on
their types.

```ts
const faults = await unalarm(link);
```

#### `settle()`

Polls the status until a condition holds. The controller acknowledges a
command before its motion ends, so a script waits with this.

**Syntax**

```ts
settle(link: Link, until: (s: MachineStatus) => boolean, options?: SettleOptions): Promise<MachineStatus>
```

**Parameters**

* `link`: the link to poll.
* `until`: the condition. `atRest`, `inState(s)` and `inAnyState(...s)`
  are ready ones.
* `options.pollMs`: the gap between polls. Default 150.
* `options.timeoutMs`: give up after this long; 0 waits forever. Default 0.

**Return value**

The sample that met the condition.

**Exceptions**

* `SettleError` on a timeout, or when an e-stop or alarm arrives first; its
  `failure` says which.

**Example**

```ts
await link.command("goto w xy 100 50");
await settle(link, atRest, { timeoutMs: 30000 });
```

#### `waitAtRest()`

`settle(link, atRest, options)`.

**Syntax**

```ts
waitAtRest(link: Link, options?: SettleOptions): Promise<MachineStatus>
```

## Changes

The reference above answers the failures as follows:

| Item | Answer in the draft |
|---|---|
| 1 | `toolpath.buildJob(svg, config, { thickness, layers })` returns `JobLayer`s or every error; `JobLayer` moves to `packets` so `toolpath` needn't import `link` |
| 2 | `loadSvg(svg, config)` takes the config and returns a list of `ToolLayer`s with their keys; `toolLayer()` and `toolLayerKey()` go private |
| 3 | `new JobSource(layers, { id, manual })` |
| 4 | `Session.run()` resets the seq; `resetSeq()` private |
| 5 | `session.phase`: `sending`, `held`, `waiting`, `finishing`, `done`; `JobSource.drained`, `waiting` and `hasLayer()` private |
| 6 | `link.nextStatus(timeoutMs)` |
| 7 | the holds tabled in the reference |
| 8 | `nackName()`, `cfgNackName()`, and `fatalReasonName()` renamed `fatalName()` |
| 9 | `ConfigTransferError` has an entry |
| 10 | `pushConfig(config)` resolves to `{ bytes, crc32 }`; `pullConfig()` resolves to the config |
| 11 | `loadConfig()` reads JSONC; `stripJsonComments()` private |
| 12 | `link.nextJogReply(timeoutMs)` resolving to `{ ok }` or `{ ok: false, reason }`; `JogAck` private |
| 13 | `link.stop()`; `send()` private |
| 14 | `textDesyncs` moves into `link.stats()` |
| 15 | `sinks`, `demux`, `writer`, `transport` private |
| 16 | the README follows the reference's `buildJob` example |

Renames the draft makes beyond the failures, for clearer names:

* `WebSerialTransport.requestAndOpen()` → `request()`; `fromOpen()` →
  `fromPort()`; `NodeTransport` → `NodeSerialTransport`.
* `encodeConfigBlob`/`decodeConfigBlob` → `encodeConfig`/`decodeConfig`;
  `modelToFile` → `configToFile`; `configImageFromText`/`meshImageFromText`
  → `configImage`/`meshImage`.
* `loadSvgPaths` → `loadSvg`; `prepareBezierJob` returns the `{ ok }`
  result shape and drops `contours` (used only by tests).
* `StreamResult` → `SessionResult`, with `fatal` for `fatalReason`.

Left out of the draft, so private unless a later decision keeps them:
the plain stream (`link.stream()`, `ListSource`, `PacketSource`,
`StreamContext`, `DEFAULT_WINDOW`), `Session.truncate()`, `Sink`,
`LatestSink`, `Writer`, `Demux` and its reply classes, `parseConfig()`,
`axisMap()` and `readAxisMap()` (the firmware no longer has `axis_map`),
the toolpath's inner stages (`cleanSubpath`, `annotate`, `analyzeBezier`,
`BezierFlag`) and the config's resolve helpers (`toolForLayer`,
`resolveTargets` …). Tests import those from their files.

Already decided:

* Move the files as Layout shows; delete `src/index.ts`; `package.json`
  exports the subpaths.
* `openNodeLink()` becomes a transport opener.
* `sim.ts` stops importing link's `Demux` type.
* The demo, the tests, `web/scripts/`, `web/README.md` and the `system/`
  pages' Code sections follow the new paths.

## Branches

Types follow `AGENTS.md`: `refactor/` where the library does the same
thing under new names or paths, `feature/` where it does something new.
A rename or a name made private counts as structure: the interface
changes, the behaviour doesn't. Refactors land first. No item is a
`fix/`: none is wrong behaviour, each is a missing or awkward API. Each
branch is tagged with the items of "Where the API fails its user" it
answers; together they cover 1–16.

Checks for each: `pnpm typecheck`, `pnpm lint`, `pnpm test` in `web/`, and
the demo runs. The demo, the tests and the subpaths above are updated in
the branch that changes what they call, so the demo works after every
merge. A `feature/` branch adds tests for its new behaviour.

1. `refactor/web-subpaths`: the structure, no API change. No items.

   * Move the files as Layout shows. Each folder's `index.ts` exports
     today's names unchanged; `src/index.ts` goes; `package.json` exports
     the subpaths.
   * `openNodeLink()` leaves the transport layer; `sim.ts` stops importing
     link's `Demux` type. The dependency table holds from here on.
   * Repoint the demo, the tests, `web/scripts/`,
     `test/packageExports.test.ts`, `web/README.md` and the `system/`
     pages' Code sections.
   * Write `docs/reference/host-api/README.md`: the index with Holds and
     Depends on, units, errors, waiting.

   Depends on nothing. **Status:** merged at `29e7621`.

   **Outcome.** As planned, with three differences: `config/index.ts`
   still re-exports the model with `export *` (branch 4 trims it); the
   tests stay in `test/wire/` and `test/machine/`, only repointed; and
   `Attachable` moved to `urumi-host/transport`, beside the new `ByteSink`
   the sim feeds. The index page describes today's names; branches 2–7
   update it.
   Missed: the `git diff` line atop `reference/packets.md` still names
   `web/src/wire/format` and `web/src/wire/link/demux.ts`; branch 3 fixes
   it.

Branches 2–7 each write their subpath's page from its draft above, change
the code to match it, and end with the page checked against their last
commit. They go bottom-up, so a branch updates callers no later branch
reworks.

2. `refactor/web-transport`: `request()`, `fromPort()`,
   `NodeSerialTransport`. No items. Depends on 1.
   **Status:** merged at `fdd9248`.

   **Outcome.** The refactor as planned, plus two features asked for in
   the branch, each its own commit: `fromPort()` throws on a closed port,
   and `NodeSerialTransport.open()` names a missing `serialport` and
   resolves only once the port is open (it used to resolve at once and
   drop an open failure). The `Writer`'s seams moved to `link/abort.ts`;
   the sim's hook to `transport/attach.ts`, which the subpath doesn't
   export. `SimOptions` is exported; its unused `fCpu` went.
   `NodeSerialTransport.openWith()` is public only for the tests' fake
   `serialport`; the page doesn't list it.
3. `refactor/web-packets`: `JobLayer` moves in; internals go private. No
   items. Depends on 1. **Status:** merged at `e2fda44`.

   **Outcome.** As planned, plus:
   * `packets` no longer imports `toolpath`: `packBezier()` takes a
     `BezierRecord`, the record's nine fields, defined in `packets`.
     `toolpath`'s `AnnotatedBezier` extends it, and `BezierAnalysis` takes
     its four shared fields from it, so each field is declared once.
   * The subpath now exports every `MAGIC_*`, `*_SIZE`, `NACK_*` and
     `NACK_DETAIL_*` the page's patterns name; four `NACK_*` codes and the
     header magics were missing. The retired `MAGIC_STATUS_RSP_V1`–`V4` went
     private.
   * The page differs from the draft where the code did: `stampSeq()`
     returns a copy, `packStatusRsp()` takes `PackStatusRspOptions`, and
     `MachineStatus` has `unlocked` and `hasFault()`. It says "record" for
     packet and frame alike, as `reference/packets.md` does.
   * `JogDir` stays exported; branch 6 decides it with `Link.cjog()`.
   * The `git diff` line atop `reference/packets.md` names the new paths
     (branch 1's miss).
4. `feature/web-config`: `loadConfig()` reads JSONC (11); `encodeConfig`,
   `decodeConfig`, `configToFile`, `configImage`, `meshImage`; the resolve
   helpers go private. Depends on 1. **Status:** merged at `ebf8290`.

   **Outcome.** The page names the config's three forms first (JSON,
   config, encoded config) and uses only those words; the code's names and
   comments in `config/` follow. Against the draft:
   * `configToFile` is `saveConfig`, the pair of `loadConfig`.
   * `configImage`, `meshImage` and `renderRuntimeHeader` are private, as
     `configBin`, `meshBin` and `renderRuntimeHeader`: only `web/scripts/`
     uses them, importing their files.
   * The factories, `parseConfig`, `CONFIG_BLOB_VERSION`, `MAX_HEADS` and
     `TOOL_PROFILES_BY_TYPE` are private too; every part type of
     `ControllerConfig` stays exported.
   * A second feature, its own commit: the `config.bin` builder runs
     `loadConfig()`, so `uploadfs` refuses a config the validator refuses.
     It used to parse only, and flash such a config.
   * "Blob" stays in `link/` and `packets/` (branch 6) and on
     `system/config.md`, whose function names alone were repointed.

5. `feature/web-toolpath`: `buildJob()` (1), `loadSvg(svg, config)` (2),
   `prepareBezierJob()`'s result shape; the inner stages go private; the
   README's job example uses `buildJob()` (16, begun). Depends on 3 and 4.
   **Status:** merged at `b5caa57`.

   **Outcome.** The page names three forms first (SVG, tool layer, job
   layer); `loadSvg()` and `buildJob()` are the only exports. Against the
   draft:
   * `ToolLayer` also carries `paths`, the layer's curves, so the demo
     can draw its preview from the public API.
   * `buildJob()` also checks the tool's `id` and its chamber (the demo's
     old checks), and returns a malformed SVG or a refused curve as an
     error naming the layer. `JobResult` has no job-wide `bbox`; each
     `JobLayer` keeps its own.
   * `JobLayer.packets` is `records`, the packets page's word.
   * `offset` is a `Point`; the draft's `Pt`, `BBox`, `AnnotatedBezier`
     and the inner stages are private. `prepareBezierJob()` is private
     too and keeps `{ error }`: only the tests call it.
   * The key example is `revolver_pen.2`, as the code reads it, not
     `revolver_pen/slot1`.
   * Follow-up: behind the API the toolpath still uses several words
     for one thing: `Subpath`, `CleanSubpath` and contour; piece,
     `AnnotatedBezier` and record; `Pt` against `Point`; `svgText`; `p`,
     depth and plunge. Not settled here.

6. `feature/web-link`: the `Link` class and the names. Depends on 2–5.
   **Status:** merged at `df2d3f2`. Settled in the read phase, against the draft:
   * **Status.** The link polls from construction until close, every
     `statusPollMs` (constructor option; default 100, 10 to 1000 ms, a
     third of the firmware's `HOST_TIMEOUT_MS`, else `RangeError`; no off
     switch), on the background-tab clock. `HOST_TIMEOUT_MS` joins the
     packets constants. Public: `status` (the latest sample, or `null`),
     `freshStatus()` (a sample from after the call; rejects if the port
     closes) and `onStatus(cb)` → `off()` (every sample, any number of
     listeners). This answers 6 instead of the draft's `nextStatus()`.
     Internal: an iterator that keeps a per-loop counter over the status
     sink, for `.settled`, the session and `CJog`; `getStatus()` goes
     `@internal` and is removed in branch 8.
   * **Stopping.** `estop()` replaces `stop()` and `send("stop")` (13).
     `abort()` keeps its name; its job is to get the machine to IDLE.
   * **Continuous jog.** `cjog(dir, { speed })` returns a `CJog`
     (`started`, `running`, `turn()`, `stop()`), which repeats the packet
     itself; `CJogDirection`, `CJogReply`. `cjogStop()` and the draft's
     `nextJogReply()` (12) go. From the firmware (`ops/cjog.cpp`): only a
     start or a refusal is answered, a change of direction is answered
     once the machine is at rest, a held jog ignores a new speed, and the
     end of travel is silent.
   * **Config.** `pushConfig(config)` → `{ bytes, crc32 }`, `pullConfig()`
     → the config or `null` (10); timeouts internal; a pulled config that
     doesn't decode is a `ConfigTransferError` with `reason: null` (9).
     "Blob" becomes "encoded config", the config page's word.
   * **Names.** `nackName()` and `cfgNackName()` (8), in the code's style:
     the constant's name without its prefix, `STATE(7)` for an unknown
     code, `"NONE"` for no faults. The `*_NAMES` tables and `maskStr` go
     private.
   * **Waiting.** `settle()` and `waitAtRest()` stay until branch 8, as
     they are (`waitAtRest()` resolves to a boolean). `atRest` is fixed to
     mean the firmware would take a command: outside a job, `IDLE` and
     nothing queued; today it reads only the planner's queue, so it holds
     mid-homing.
   * **Kept.** `command(line)` → the reply, until branch 8. `session()`
     and `attach()` stay apart (branch 7). `stats()`: `textDesyncs` and
     the demux's counters (14). `verbose`, `close()`.
   * **Private** (15): the sinks, demux, writer, `send()`, `resetSeq()`
     and `stream()` go `@internal` (the tests use them); `Link.open()` and
     the `transport` getter go; the index drops the demux, sink and writer
     types. The `stop` and `getStatus` helpers go.
   * **The page.** `link.md` sorts `Link` by whether a call waits its
     turn (text commands, one at a time) or goes at once (status, stops,
     jog, config, jobs), not by plane: `estop()` is a text line that
     skips the queue.
   * From [host-watch](host-watch.md): `link.md` takes in `Link.attach()`;
     the session half (`JobSource.jobId`, `jobIdOf()`,
     `FATAL_JOB_MISMATCH`, `lastLayer`, `lastRecord`) is branch 7's.

   **Outcome.** As settled above, with these differences:
   * The status listeners run off one internal loop over the status
     sink's counter; `settle()`, the session and `CJog` don't share it
     yet. `settle()` and the session still poll through `getStatus()`, so
     a job is polled twice until branch 7.
   * `CJog` treats no answer in time as a refusal (`reason: null`): it
     stops the jog. `turn()` to the direction already running resolves
     `{ ok: true }` at once, since the controller doesn't answer a repeat.
     A second `cjog()` stops the first one repeating.
   * The push and pull without the encoding stay as `@internal`
     `_pushEncoded()` and `_pullEncoded()`, for the tests.
   * `waitAtRest()` waits for `atRest`, `ALARM` or `ESTOP`, since the
     fixed `atRest` no longer holds in either.
   * The demo shows the status through `onStatus()` (its own poll and
     interval box go), jogs through `CJog`, and names refusals with
     `nackName()` and `cfgNackName()`.
7. `feature/web-session`: the job session. `JobSource`'s options (3),
   `run()` resetting the seq (4), `Session.phase` (5), the holds tabled
   (7), the fatal reason split and `SessionResult`; the README's job example
   finished (16). Depends on 5 and 6. **Status:** merged at `d79c9fa`.
   * `Link.attach()` folds into `Link.session()`: `run()` asks the
     controller first, sends a new job when it is idle, takes over a
     paused job with the source's id, and fails with `FATAL_JOB_MISMATCH`
     on another id. A new job can't land on an open one by accident: the
     controller refuses a `JOB_HDR` while a job is open.
   * `JobSource` and `Session` get their own page, `session.md`, still
     under `urumi-host/link`; `link.md` keeps `Link.session()` and links
     to it. It takes in host-watch's `JobSource.jobId`, `jobIdOf()`,
     `FATAL_JOB_MISMATCH`, `lastLayer` and `lastRecord`.
   * The session reads the link's poll instead of sending its own status
     requests; its `_pollStatus()` goes. Its request placed after each
     window fill gives way to the samples' freshness stamps.
   * The session's ending methods: `cancel()` is how a finished job is
     closed, which reads like giving up, and `end()` sends ABORT. Rename
     them as a pair, beside `Link.estop()` and `Link.abort()`.
   * The job examples (host-api README, toolpath.md, `web/README.md`)
     add coming back to a paused job.
   * `FATAL_*` never cross the wire: the session makes them up, numbered
     from 0xE0 only to share `fatalReason` with the controller's
     `NACK_*` bytes. Split the field: `{ by: "controller", nack, detail }`
     or `{ by: "host", why: "stall" | "crc-limit" | "job-lost" |
     "job-mismatch" }`. The nack is named with `names.nack()` (branch 9);
     the host's reason is a word already. `fatalReasonName()`, its third
     copy of the NACK names (with the prefix `nackName()` drops) and the
     `FATAL_*` constants go.

   **Outcome.** As settled above, with these differences:
   * No `Session.phase`: most of it repeats the controller's
     `status.reason`. The session shows only what the host knows: `hold`
     (with `release` for a manual layer waiting) and `drained` (every
     frame acknowledged). A job is cut when it is drained and the status
     is at rest.
   * `cancel()` and `end()` are `close()` and `abort()`. `run()`
     resolves to the `SessionResult`; `result()` goes.
   * The plain stream goes: the controller refuses a record outside a job
     (`NACK_JOB`), so `link.stream()`, `ListSource`, `PacketSource`,
     `StreamContext`, `truncate()` and the window have no use. The sim
     tests send raw frames with their own sender.
   * `JobSource` takes no `id`: its `jobId` is a hash of the frames, so
     rebuilding the same layers finds the paused job. `jobIdOf()` and the
     replay methods are `@internal`.
   * `run()` rejects if the link closes, leaving the job open. A job counts
     as open once a status sample arrives after the first ACK, so a
     `cancel` from elsewhere during placement is `job-lost`. `_drop()`,
     `@internal`, stands in for a reload in the tests.
8. `feature/web-commands`: the text commands. Depends on 6 and on the
   firmware's `busy` bit (below, merged).
   **Status:** merged at `357deec`.
   * `link.commands()` returns the link's one `Commands` (no public
     constructor). `send(line)` gives the raw reply; each other method
     sends its command, parses the reply, resolves on `ok` with the parsed
     value where there is one (`jog`'s depth, `unalarm`'s mask), and
     throws a `CommandError` (`command`, `reason`, `reply`) on `err` or a
     node failure, in place of today's mix of `false`, `null`, a raw
     string and a throw.
   * Nothing reports that a command finished: the host can know only
     whether a command was taken and whether the machine is busy. Methods
     return no handle.
   * `cmd.onReady` gives the same non-read methods, each waiting for a
     status sample with `busy` false, then sending once. It throws if the
     e-stop is latched; no timeout. The methods on `cmd` send at once.
     Reads (`get`, `ping` …) are on `cmd` only: always admitted.
   * Scope: what `commands.ts` has, checked against today's firmware,
     plus `place`. `goto`'s XY takes `"min"`/`"max"`. `axisMap` and
     `readAxisMap` (`axis_map`, gone from the firmware) are dropped for
     `axesMap` and `slotMap`. The demo's buttons move onto the methods;
     its console stays on `send()`.
   * `link.command()` goes private. The step jog is `cmd.jog()`, apart
     from `link.cjog()`. The unexported helpers (`goto`, `home`, `mount`,
     …) become methods; the free functions go.
   * `settle()`, `waitAtRest()`, `inState()`, `inAnyState()`,
     `SettleError` and `atRest` go: `atRest` was a host guess (IDLE or a
     paused or empty job) that hangs in ALARM, tool wait, placement and
     between legs, where the firmware admits commands. `getStatus()` goes.
   * No chaining; a `run()` for fixed sequences only if they turn out
     common.
   * The `placed` bit, firmware: STATUS_RSP flags bit 1 is `placed`
     (the placement's `isSet`: `place set` standing in JOB.PLACE), and
     `get placed` mirrors it; same layout, same magic. Host:
     `MachineStatus.placed` beside `busy`; the sim models it. The demo's
     `notePlace` guess goes: its hint reads `link.status.placed`, and a
     refusal's text comes from the `CommandError`. Not the session's to
     track: a `place set` from the console, another tab or before a
     takeover never passes through it.
   * The text plane's stray lines: `stop` replies `ok`, which
     `link.estop()` never reads, and `reset` replies `ok` then the boot
     banner (`RS485 controller …`, `ready`); both count as desyncs on the
     next command today. `estop()` drops the one `ok` it causes;
     `cmd.reset()` resolves on `ok` and drops the banner through `ready`;
     `send()` takes the same paths. `link.send` becomes `link._send`.
   * The sim moves from `axis_map` to `axes_map` and `slot_map`, and its
     tests with it. The tests move from `link.command()` and
     `link.getStatus()` to `cmd.send()` and `link.freshStatus()`.
   * Docs: a new `commands.md`, stamped. `link.md` loses `settle()`,
     `waitAtRest()` and the helper table, and points to it. The README adds
     commands to Done, a `CommandError` bullet to Errors, and rewrites
     Waiting: the controller acknowledges a command, not its end; the host
     sees only `busy`; `cmd.onReady` waits before sending. `wire.md` drops
     `settled.ts` and `getStatus()`. For `placed`: `packets.md` (the flags
     row), `commands.md` (`get placed`, the `status` group if it joins),
     `host-api/packets.md` (a `placed` row, a new stamp). `link.md`'s
     `estop()` says its `ok` is dropped; `commands.md` (host-api) says
     `reset()` waits out the banner.

   **Outcome.** As planned, in two commits: the firmware's `placed`
   (`3705312`) and the host (`e8de999`). Settled along the way:
   * One method per command, its forms in one argument: `leg({ form, id,
     … })` with the speeds as `ramp: { startUs, endUs, steps }` for the
     four leg helpers; `home(target?)` for the `home*` variants;
     `jog({ x?, y? } | { z } | { a })`, the direction shape `cjog()` takes,
     for `jog` and `jogXY`; `place("go", { x, y })`. `axesMap()` and
     `slotMap()` read with no argument and set with one.
   * `CommandError.reason` is one token: the word after `err`, a node's
     result, `timeout`, `bad_reply`, or `estop` from `onReady`.
   * `getState`, `getFrames`, `STATE_KEYS` and `GET_MAX_KEYS` go: the
     status covers them. `unstop`, `bus_exclude`, `laser`, `set`, `save`
     and the motion primitives stay on `send()`.
   * `placed` is `place set` standing, false once `place ok` leaves PLACE;
     it joined the `get status` group after `busy`.
   * The link keeps, in write order, who each awaited text line belongs
     to, so an e-stop's `ok` is dropped whether a command was waiting or
     not. The sim's option is `axesMap`, and its tests poll the status
     every 10 ms so `freshStatus()` sees a 60 ms leg running.

   Two bugs found and fixed: the leg helpers sent `leg <id> <form>`, the
   order the firmware doesn't read (the sim accepted it), and `probeLeg`
   sent `probe_leg`, which the firmware no longer has. The `placed` flag,
   `reset` and the leg order are tested against the sim only, not yet on
   the Pico.
9. `feature/web-names`: the names move to `urumi-host/packets`, beside
   the enums they name. Depends on 6, and on the NACK codes (below).
   **Status:** merged at `dfc74f7`.
   * The namers move from `link/names.ts` to `packets`, under one module
     namespace, `export * as names from "./names.js"`: `names.state()`,
     `names.reason()`, `names.context()`, `names.faults()`,
     `names.nack()`, `names.cfgNack()` and `names.axes()`. The namespace
     is the language's grouping, so a bundler still drops what a page
     doesn't call. `link` stops exporting them; the tables stay private.
   * `names.reason(state, r)` replaces `jobName()`: the reason byte means
     something different in each state, so it is read against the state.
     `JoggingReason` and `HomingReason` join `JobReason`, and `Probing`
     becomes `ProbingReason`, the firmware's names. `names.axes()` is the
     old `maskStr()`. `names.nack()` covers `JOG_NACK` too: one set of
     codes.
   * Only wire codes are named there: the session's fatal reasons stop
     being codes in branch 7, and a NACK's text is already words.
     `HomeFail`, `ProbeCause` and `Selection` get names if a display
     needs them.
   * Codes stay numbers. `MachineStatus` gains `stateName`, `reasonName`
     and `faultNames` getters beside `state`, `reason` and `faults`. An
     object per code was weighed and dropped: `===`, `switch`,
     `includes()` and `Set` keys compare it by identity, and JavaScript
     has no operator to overload. A `text()` returning every field named
     was dropped too: a parallel copy of the status, mostly the same.
   * `packets.md` gains a Names section after Refusals; `link.md` loses
     its own; `link.md`, `session.md` and `commands.md` examples use the
     getters and `names`.

   **Outcome.** As above. `status.ts` imports `names` for the getters and
   `names.ts` reads its enums, so the tables are built on first use. An
   unknown fault bit prints in hex, other unknowns as `KIND(n)`. The
   reference stamps move to `e511050`. `web/demo/index.js` still imports
   the old names from `link` and doesn't load; it is updated in the next
   pass.

Firmware, before branch 8: the `busy` bit. Admission refuses with
`err busy` while a step or sequence is live (`machineMoving()`: a paused
job's parked ring is not); the host can't see that today. `STATUS_RSP`
gains a `flags` byte before the CRC (39 bytes, a new magic; `0xA0` joins
the retired layouts), bit 0 `busy = machineMoving()`, and `get busy`
mirrors it. On the host, `MachineStatus.busy` (flat, as `context` and
`layer` are). Named for the refusal it predicts: "ready" would overclaim,
since a machine in ALARM is not busy yet refuses `home`. The sim emits
the new layout and computes `busy` the same way. Docs: `packets.md` (the
frame, the retired list), `commands.md` (`get busy`, and a line under
"When a command runs"), `controller.md` (Admission publishes it),
`wire.md`; `host-api/packets.md` gains `busy` and a new stamp.
**Status:** merged at `7ae8e22`.
**Outcome.** As planned, with the magic `0xBA` and the key also in the
`get status` group, after `buf`. `wire.md` needed no change: it points to
`packets.md` for the fields. The sim's `busy`: a leg or jog, a tool change,
or a job's ring holding blocks unless paused. Tested on the Pico with
`dummy_leg`: `busy` 1 and a second leg `err busy` while it runs, 0 between
legs in HOMING; the two reference frames are repacked in Python.

Firmware and host, before branch 9: the NACK codes. Two codes cover most
refusals, so a NACK often says little about what went wrong, and in one
case the host reacts to the wrong thing. The fix has two parts. The codes
split, so each one tells the host what to do. And a NACK carries a short
text saying exactly what failed, for a person to read. Branch 9 then names
the final set once.
**Status:** merged at `cdde579`.

**`NACK_JOB` today: 17 send sites** (`data_plane.cpp`).

| Line | Record | Refused because | Cause |
|---|---|---|---|
| 227 | `JOB_HDR` | version not 1, or tool count outside 1–4 | bad field |
| 284 | `TOOL_HDR` | the same tool twice in preflight | preflight |
| 345 | `LAYER_HDR` | `LAYER_BAD`: head or chamber out of range, a plunge past `maxOvercut`, or a tangential or chamber tool on a head without A | bad field, or the tool can't run there |
| 371 | `LAYER_HDR` | the planner's `PQ_BAD_CURVE` from `plannerLayerCheck`: a layer inside a contour | order |
| 372 | `LAYER_HDR` | `PQ_NO_RESUME`: a resume header off the resume point | resume |
| 383 | `TOOL_HDR` | no layer held | order |
| 386 | `TOOL_HDR` | its tool wasn't in preflight | preflight |
| 413 | `TOOL_HDR` | not in a job | order |
| 413 | `TOOL_HDR` | no valid config | config |
| 421–422 | `TOOL_HDR` | bbox not finite, or min > max | bad field |
| 431 | `BEZIER` | during preflight | order |
| 433 | `BEZIER` | not in a job, a layer held, or no layer open | order |
| 470 | `BEZIER` | `PQ_NO_JOB` | order |
| 482 | `LAYER_HDR` | during preflight | order |
| 484 | `LAYER_HDR` | not in a job, or in PLACE | order |
| 484 | `LAYER_HDR` | no valid config | config |
| 499 | `LAYER_HDR` | `layerValid()` fails: chamber, feed, thickness or p | bad field |
| 507 | `LAYER_HDR` | a layer already held, or inside a contour | order |

**`NACK_BAD_STATE` today: 20 send sites.**

| Where | Refused because | Cause |
|---|---|---|
| `data_plane.cpp:161` | any record outside IDLE or a job | state |
| `data_plane.cpp:205` | `JOB_HDR` not in IDLE | state |
| `data_plane.cpp:205` | `JOB_HDR` with no valid config | config |
| `data_plane.cpp:230` | `plannerOpenJob()` fails | state |
| `data_plane.cpp:322` | `plannerJobSwap()` fails at the end of preflight | shouldn't happen |
| `data_plane.cpp:361` | a layer needs a tool change and `toolChangeBegin()` refuses | ? |
| `data_plane.cpp:366` | selecting the layer's head fails | ? |
| `data_plane.cpp:373` | `plannerLayerCheck`'s other results: `PQ_BAD_STATE`, `PQ_NO_CONFIG`, `PQ_NO_LIMITS`, `PQ_NO_FEED` | four causes |
| `data_plane.cpp:458` | `framesToMachine()` fails: no head selected | ? |
| `data_plane.cpp:471` | `plannerQueueRecord`'s other results: the same four | four causes |
| `cjog.cpp:38` | admission refuses, not `busy` | state |
| `cjog.cpp:50` | no valid config | config |
| `cjog.cpp:54` | in PLACE, the dot is off, or Z or A asked | state |
| `cjog.cpp:56` | `plannerJogFrom()`: another kind of motion owns the ring | busy |
| `cjog.cpp:67` | the axis isn't configured | config |
| `cjog.cpp:69` | the axis isn't homed and `jogUnhomed` is off | not homed |
| `cjog.cpp:81` | the jog feed works out to 0 | config |
| `cjog.cpp:88` | the planner refuses the line, not `PQ_SOFT_LIMIT` | four causes |
| `cjog.cpp:97` | a direction outside −1 to 1 | bad field |
| `cjog.cpp:98` | speed 0 | bad field |

**What this costs today.**

* **A bug in the session.** After preflight, the session reads every
  `NACK_JOB` as a resume header off the resume point and holds
  (`session.ts:492`, Hold `"refused"`). A layer with a bad field, or a
  frame out of order, mid-job holds the session instead of ending it.
* **The planner knows more than the wire says.** `PlannerQueueResult`
  already tells `PQ_NO_CONFIG`, `PQ_NO_LIMITS` and `PQ_NO_FEED` apart
  from `PQ_BAD_STATE`; the wire folds all four into `BAD_STATE`. The
  text commands already say `no_feed`, `no_limits`, `no_head` and
  `not_homed`.
* **No config has three answers.** `BAD_STATE` on `JOB_HDR` and `CJOG`,
  `NACK_JOB` on `TOOL_HDR` and `LAYER_HDR`.
* **The sim differs in places.** A `BEZIER` with no job open is
  `BAD_STATE` in the sim (`sim.ts:699`) and `NACK_JOB` on the Pico.

**The other codes.** Each means one thing to the host, though some cover
several cases a person would want told apart:

* `SOFT_LIMIT` and `OUT_OF_RANGE` both mean "this would leave the
  travel". One checks a header's box before motion, the other a curve or
  jog as it arrives; the refused record already says which.
* `OUT_OF_RANGE`'s detail bits can't tell "no origin fits this tool" from
  "off the low side": both leave the side bit clear.
* `TOOL_UNAVAILABLE` covers the laser's node mute or excluded, a tool that
  doesn't fit its head, and a missing power node.
* `NOT_HOMED` covers X or Y, a head's Z, and a head's A, and doesn't say
  which.

**The frame.** `NACK` gets a new layout and a new magic; `0xBB` joins
the retired magics. `JOG_NACK` stays `[0xB7] [reason]`: a jog is
refused while the user holds a direction, so the user already knows the
axis and side, the code says the rest, and the deadman repeats would send
the same text again and again.

```
[magic] [reason] [len] [text: len bytes] [crc8]
```

* `len` is at most 48. The text is the first failure the check found, in
  the text plane's words: `OUT_OF_RANGE Y high`, `NOT_HOMED head 1 z`,
  `TOOL_UNAVAILABLE power 7 mute`, `BAD_FIELD LAYER_HDR byte 13`.
* The text is for display. A program matches the code, never the words.
* A `NACK` carries text exactly when the session ends on it. The codes it
  waits out or holds on carry none (`len` 0): `CRC`, `FULL`, `ABORTING`,
  `BUSY`, `PAUSED`, `TOOL_CHANGE`, `NO_RESUME` (the status has the resume
  point). The wait codes can repeat for every record in the window; a
  refusal with text comes once, because the records behind it fail the
  seq guard and are ACKed as stale.
* The detail byte goes, and `NACK_DETAIL_*` with it: the text says which
  axis, side, head or node.
* A CRC, which the old 3-byte frame lacked.

**The codes.** Flat: a new code where the cause and its fix differ. New
values from `0x13`; `0x0C` and `0x11` keep theirs, and `0x09` is retired.

| Code | Means | Takes |
|---|---|---|
| `NACK_BAD_STATE` | the machine is in the wrong state, only | 161, 205 (state), 230, `PQ_BAD_STATE`, `cjog` 38 and 54 |
| `NACK_NO_CONFIG` *(new)* | no valid config, or the config lacks a value this needs: limits, a feed, the axis | 205, 413, 484 (config), `PQ_NO_CONFIG`, `PQ_NO_LIMITS`, `PQ_NO_FEED`, `cjog` 50, 67 and 81 |
| `NACK_ORDER` *(new)* | a frame out of the job's order | 371, 383, 413, 431, 433, 470, 482, 484, 507 |
| `NACK_BAD_FIELD` *(new)* | a field out of range or malformed | 227, 421–422, 499, `cjog` 97 and 98 |
| `NACK_NO_RESUME` *(new)* | a resume header off the resume point: the host holds | 372 |
| `NACK_JOB` | a tool twice in preflight, or a tool not in it | 284, 386 |
| `NACK_OUT_OF_RANGE` | as now, plus every `SOFT_LIMIT` case | the `SOFT_LIMIT` sites |
| `NACK_NOT_HOMED` | as now, plus a jog on an unhomed axis | `cjog` 69 |
| `NACK_BUSY` | as now, plus another motion owning the ring, on the stream too | `cjog` 56; 230 when `plannerOpenJob()` fails because a jog owns the ring (a `JOB_HDR` during a step jog in IDLE ends the session today); check 361 the same way |

**Firmware.** Each send site writes its words into a fixed buffer. Most
already hold the axis, head or node. A few helpers return a bare `bool`
or a lumped result and must return a reason instead, the way
`machineAdmit()` and `framesToMachine()` return a `const char*` today:
`layerHeadFits()`, `powerAvailable()`, `layerValid()`, `layerResolve()`'s
`LAYER_BAD`, `toolOrigins()` and `configAxisRange()`. `PlannerQueueResult`
maps to a code and words through a small table. Check what `Serial.write`
does when the USB buffer is full: `sendNack()` writes from core 0's data
plane, and could stall it.

**Settled cases.**

* `LAYER_BAD` (345) splits in `layerResolve()`: a head beyond
  `headCount`, or a tool that doesn't fit the head, is `TOOL_UNAVAILABLE`;
  a missing or too-deep plunge `p`, or a chamber out of range, is
  `BAD_FIELD`.
* 361: `toolChangeBegin()` refuses only when the job can't hold for a
  tool, so `BAD_STATE`; check it against `busy` too. 366: the head's nodes
  won't map (`axesMapApply()`), so `TOOL_UNAVAILABLE`, with the mapping's
  reason as the text. 458: no head selected, so `BAD_STATE`, text
  `no_head`.
* 322 should be unreachable: the job left PREFLIGHT or the planner moved
  between records. No code of its own: `BAD_STATE`, with text that names
  it.
* `NACK_JOB` keeps two cases, a tool twice in preflight (284) and a tool
  preflight didn't declare (386, after preflight), so it is renamed
  `NACK_TOOL_LIST`.
* The host-api names: the codes become an enum, `Nack`, and `CfgNack` for
  the config's, in place of the `NACK_*` and `CFG_NACK_*` constants;
  `parseNack()` returns a `Refusal { code, text }`, beside
  `parseStatusRsp()`; `packNack()` and `packJogNack()` for the sim.
  `BAD_MAGIC` goes: it is never sent. Drafted in `host-api/packets.md`.

**Host.** The demuxer and `parse` learn the new frames. The session holds
on `NO_RESUME` only (not on every `NACK_JOB`), and every new code but
`NO_RESUME` joins `FATAL_NACKS`. A preflight refusal still ends the job
whatever its code. `Ended` carries `text` in place of `detail`, from the
first refusal. The sim sends the same codes and words from the same
checks.

**Docs.** `usb_protocol.h` and `constants.ts` comments;
`reference/packets.md` (the frame, the reasons table, each record's
refusals, the resend rules, the retired magics); `system/wire.md` (the
demux reads `NACK` by its length now, and "a reason and a detail byte");
`system/jobs.md` (`SOFT_LIMIT`, the holds table's `refused`, what ends a
job, the detail byte); `system/planner.md` (the `PQ_*` to NACK table);
`host-api/packets.md`, `host-api/session.md` (Hold `refused`, `Ended`'s
`text`, the fatal codes) and `host-api/link.md` (the jog's codes), with
new stamps; `link/names.ts`, which branch 9 moves.

**Tests.** A sim test per new code and for the text; on the Pico, at
least `NO_RESUME`, `ORDER`, `BAD_FIELD`, and a `FULL` with no text.

**Considered and dropped.**

* A detail byte with a meaning per code, or a struct of fault fields per
  code: every reader needs a table per code, and the frame grows to the
  largest.
* Asking for the details afterwards, as SCSI's `REQUEST SENSE` does: a
  `get refusal` text command. It needs a round trip, the firmware must
  keep the first refusal until read, and the session would call into
  `Commands`. Carrying the text in the NACK has none of that.
* The text in a frame of its own after the NACK: the host would have to
  pair each one with its NACK.

**Flow control, not addressed.** On `NACK_FULL` the host waits a fixed
50 ms and resends its whole window of 16. The records behind the refused
one fail the seq guard and are ACKed as stale and dropped, so once the
ring is full, each round sends about 16 records to land one or two, and
gets about 15 duplicate ACKs back. It costs little (about 1 KB per round
over USB, a CRC per wasted record), so it stays as it is. The fix
considered was credit-based, as TCP's receive window: the ACK's unused
third byte carries the room left, and the host sends no more than that.
The room would be approximate, since one `BEZIER` can become several
blocks, so `FULL` and the resend would stay as the fallback.

**Outcome.** As planned, but for four places. Soft limits off on X or Y is
`NO_CONFIG`, not `OUT_OF_RANGE`: a config value the job needs. The hold
keeps the name `refused`, now meaning `NO_RESUME`, so the public API
doesn't change. The text for no config is `unconfigured`, the text plane's
word. A `JOB_HDR` during a jog is `BAD_STATE` `not idle`, since the state
is JOGGING; `BUSY` comes only when the state is back to IDLE while the jog
still holds the planner. The bench added one: a record in a state other
than IDLE or a job was `BAD_STATE` with no text, and now carries the
state's name, such as `HOMING`. Tested on the Pico: `ORDER`, `BAD_FIELD`,
`NOT_HOMED` and `BAD_STATE` with their text; `CRC`, `FULL`,
`TOOL_CHANGE`, `PAUSED` and `NO_RESUME` with none, in a job run with the
pen mounted by command; and every `JOG_NACK` but `NO_CONFIG`. Frames
from `buildJob` and `JobSource`, sent from Python.

Firmware, a branch and plan of its own, apart from these: `abort`'s job
is to get the machine to IDLE. Today `jobAbort()` is reachable only as the
`ABORT` frame, and outside a job it only brakes the planner; a homing or
probe sequence (`sequenceAbort()`) is reachable from neither plane. Add a
text `abort` command, and have `ABORT` end whatever runs (a job, a
sequence, planner motion) at IDLE.

Branches 2, 3 and 4 touch separate folders and can run in parallel; each
edits the demo, so they merge one at a time. The name helpers stay in
branch 6 rather than a branch of their own: `names.ts` is already in
`link/` after branch 1, and a separate branch would be a merge for about
20 lines.
