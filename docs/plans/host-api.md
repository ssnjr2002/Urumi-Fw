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
its page to these. Only `link` and `transport` are in the new style yet.

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
   **Status:** done at `5219bb2`, not merged.

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
   items. Depends on 1. **Status:** not started.
4. `feature/web-config`: `loadConfig()` reads JSONC (11); `encodeConfig`,
   `decodeConfig`, `configToFile`, `configImage`, `meshImage`; the resolve
   helpers go private. Depends on 1. **Status:** not started.
5. `feature/web-toolpath`: `buildJob()` (1), `loadSvg(svg, config)` (2),
   `prepareBezierJob()`'s result shape; the inner stages go private; the
   README's job example uses `buildJob()` (16, begun). Depends on 3 and 4.
   **Status:** not started.
6. `feature/web-link`: the `Link` class and the names.
   `nextStatus()` (6), `nackName()` and `cfgNackName()` (8),
   `ConfigTransferError`'s entry (9), `pushConfig(config)` and
   `pullConfig()` (10), `nextJogReply()` (12), `stop()` (13), `stats()`
   (14); the sinks, demux, writer, `send()` and `resetSeq()` go private
   (15). Depends on 2–5. **Status:** not started.
7. `feature/web-session`: the job session. `JobSource`'s options (3),
   `run()` resetting the seq (4), `Session.phase` (5), the holds tabled
   (7), `fatalName()` and `SessionResult`; the README's job example
   finished (16). Depends on 5 and 6. **Status:** not started.

Branches 2, 3 and 4 touch separate folders and can run in parallel; each
edits the demo, so they merge one at a time. The name helpers stay in
branch 6 rather than a branch of their own: `names.ts` is already in
`link/` after branch 1, and a separate branch would be a merge for about
20 lines.
