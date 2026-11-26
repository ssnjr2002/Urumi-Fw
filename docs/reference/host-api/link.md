# Link

A connection to the controller ([wire](../../system/wire.md)). Its calls
come in two kinds, by whether they wait their turn:

| Kind | Is | Through |
|---|---|---|
| **in turn** | text commands from [commands.md](../commands.md), one at a time in call order, each answered by one reply line | [`commands()`](#linkcommands) |
| **at once** | requests that never wait behind a command: the stops, the status, a continuous jog, the config, a job's records | [`estop()`](#linkestop), [`abort()`](#linkabort), [`onStatus()`](#linkonstatus), [`cjog()`](#linkcjog), [`pushConfig()`](#linkpushconfig), [`session()`](#linksession), … |

The kinds don't follow the wire's planes: `estop()` is the text line
`stop`, sent ahead of any command waiting. Both kinds can be in flight at
once; each waits for its own replies.

The link asks for the machine's status on its own, every `statusPollMs`,
from construction until the port closes, so [`status`](#properties) is
never more than one poll old.

```ts
import { Link } from "urumi-host/link";
```

**Checked against `e511050`.** To see what may have changed since:

```bash
git diff e511050 -- web/src/link/
```

| Name | Kind |
|---|---|
| [`Link`](#link-1) | class |
| [`CJog`](#cjog) | class |
| [`ConfigTransferError`](#configtransfererror) | error |

The text commands (`Commands`, `CommandError`) have their own page:
[commands](commands.md); the job session (`JobSource`, `Session`) too:
[session](session.md).

# `Link`

One connection to the controller over a transport. It reads from the
transport and polls the status from construction until the port closes.
One `Link` per port.

## Constructor

```ts
new Link(transport: Transport, options?: { statusPollMs?: number })
```

**Parameters**

* `transport`: an open [`Transport`](transport.md#transport).
* `options.statusPollMs`: how often to ask for the status, ms. Default
  100. The poll can't be turned off: while a job is open, the controller
  pauses it after `HOST_TIMEOUT_MS` (3000) without a word from the host.

**Exceptions**

* `RangeError` if `statusPollMs` isn't 10 to 1000, at most a third of
  `HOST_TIMEOUT_MS`, so one lost request can't pause a job.

**Example**

```ts
const link = new Link(await WebSerialTransport.request());
```

## Properties

| Property | Type | |
|---|---|---|
| `closed` | `boolean` | the port has closed; every later call fails |
| `status` | `MachineStatus \| null` | the latest status sample, at most one poll old; `null` before the first |
| `verbose` | `boolean` | sessions made after setting it log each ACK, NACK, resend and fatal to `console.debug`. Default false |

## Methods

| Method | |
|---|---|
| [`commands()`](#linkcommands) | the text commands |
| [`estop()`](#linkestop) | e-stop |
| [`abort()`](#linkabort) | get the machine to IDLE |
| [`freshStatus()`](#linkfreshstatus) | wait for a status sample from after the call |
| [`onStatus()`](#linkonstatus) | call a function on every status sample |
| [`cjog()`](#linkcjog) | start a continuous jog |
| [`pushConfig()`](#linkpushconfig), [`pullConfig()`](#linkpullconfig) | store or fetch the config |
| [`session()`](#linksession) | a session for a job: a new one, or the same one paused on the controller |
| [`stats()`](#linkstats) | counters for diagnostics |
| [`close()`](#linkclose) | close the port |

### `Link.commands()`

The link's text commands, as methods: [commands](commands.md).

**Syntax**

```ts
commands(): Commands
```

**Return value**

The link's one [`Commands`](commands.md#commands-1): the same object on
every call.

**Example**

```ts
const cmd = link.commands();
await cmd.home({ form: "xy" });
```

### `Link.estop()`

The e-stop: motion stops at once, without lifting. Sends the `stop`
command, out at once, never behind a pending command.

**Syntax**

```ts
estop(): void
```

**Return value**

None. The controller answers `ok`, which the link drops: a command
waiting for its own reply doesn't read it, and it isn't counted in
`textDesyncs`. The status shows the e-stop take hold (`ESTOP`, then
`ALARM`).

### `Link.abort()`

Gets the machine to IDLE, position kept. It brakes the planner's motion to
rest and drops what is queued: a job's, a `goto`'s or a step jog's. In a
job it also lifts the tool once, switches it off, and closes the job; a
job session ends this way through its own method, which also waits for the
job to close. Sends the `ABORT` frame, which never waits behind a command.

**Syntax**

```ts
abort(): void
```

**Return value**

None. No reply; the status shows the machine settle. Records sent before
IDLE are refused with `Nack.ABORTING`.

### `Link.freshStatus()`

Waits for a status sample that arrives after the call. It sends nothing:
the link's poll brings one within `statusPollMs`.

**Syntax**

```ts
freshStatus(): Promise<MachineStatus>
```

**Return value**

The sample ([`MachineStatus`](packets.md#machinestatus)). For the first
sample after connecting, or the machine's answer to something just done.

Each call sees only samples after it, so samples between two calls are
missed. To watch the status, use [`onStatus()`](#linkonstatus).

**Exceptions**

* `Error` if the port closes first.

**Example**

```ts
const link = new Link(transport);
const s = await link.freshStatus();
if (s.state === MachineState.ALARM) show(s.faultNames);
```

### `Link.onStatus()`

Calls `listener` with every status sample, until `off()` or the port
closes. Any number of listeners.

**Syntax**

```ts
onStatus(listener: (s: MachineStatus) => void): () => void
```

**Parameters**

* `listener`: called once per sample, in arrival order.

**Return value**

`off()`, which stops this listener and no other.

**Example**

```ts
const off = link.onStatus((s) => {
  stateOut.textContent = s.stateName;
  if (s.state === MachineState.ESTOP) jog?.stop();
});
```

### `Link.cjog()`

Starts a continuous jog: the machine moves in a direction until told to
stop. The returned [`CJog`](#cjog) repeats the packet while it runs, so
the controller's deadman (`CJOG_DEADMAN_MS`) doesn't stop it.

**Syntax**

```ts
cjog(dir: CJogDirection, options?: { speed?: number }): CJog

interface CJogDirection {
  x?: -1 | 0 | 1;
  y?: -1 | 0 | 1;
  z?: -1 | 0 | 1;
  a?: -1 | 0 | 1;
}
```

**Parameters**

* `dir`: the direction on each axis; absent is 0. One axis set per jog:
  XY, Z or A.
* `options.speed`: a multiple of the config's jog feed, up to
  `CJOG_SPEED_MAX`. Default 1. Fixed for the jog: to change it, stop and
  start a new one.

**Return value**

A [`CJog`](#cjog). Its `started` says whether the controller took it.

The jog runs to the end of travel at most: the soft limit when homed,
else `maxTravel`; A one turn. It stops there without a word from the
controller, and `running` stays true; the status shows the machine at
rest.

**Exceptions**

* `RangeError` if `dir` moves more than one axis set, or none, or `speed`
  isn't above 0 or past `CJOG_SPEED_MAX`.

**Example**

Arrow keys and PageUp/PageDown jog; releasing them all stops.

```ts
let jog: CJog | null = null;

async function steer(dir: CJogDirection | null) {
  if (!dir) {
    jog?.stop();
    jog = null;
    return;
  }
  let reply: CJogReply;
  if (jog?.running) {
    reply = await jog.turn(dir);   // change direction: comes to rest, then sets off the new way
  } else {
    jog = link.cjog(dir, { speed: 0.5 });
    reply = await jog.started;
  }
  if (!reply.ok) show(reply.reason == null ? "no answer" : `jog refused: ${names.nack(reply.reason)}`);
}

onkeychange = () => steer(dirFromHeldKeys());   // {x: 1}, {x: 1, y: -1}, {z: 1}, or null
```

### `Link.pushConfig()`

Encodes the config and stores it on the controller.

**Syntax**

```ts
pushConfig(config: ControllerConfig): Promise<{ bytes: number; crc32: number }>
```

**Parameters**

* `config`: a config from [`loadConfig()`](config.md#loadconfig) or built
  in code.

**Return value**

The encoded config's size and CRC32, once the controller has written it to
flash and read it back.

**Exceptions**

* [`ConfigTransferError`](#configtransfererror) if the encoded config is
  too large, or on a refusal or no reply.

**Example**

```ts
const r = loadConfig(await file.text());
if (r.ok) await link.pushConfig(r.config);
```

### `Link.pullConfig()`

Fetches the stored config.

**Syntax**

```ts
pullConfig(): Promise<ControllerConfig | null>
```

**Return value**

The config, or `null` when none is stored.

**Exceptions**

* [`ConfigTransferError`](#configtransfererror) on no reply, or stored
  bytes that fail their CRC or don't decode.

### `Link.session()`

Makes a session for a job ([session](session.md)). Nothing is sent until
its [`run()`](session.md#sessionrun), which sends the job to an idle
controller or takes over the same job paused there.

**Syntax**

```ts
session(job: JobSource): Session
```

**Parameters**

* `job`: the job's frames, a [`JobSource`](session.md#jobsource).

**Return value**

A [`Session`](session.md#session-1).

**Example**

```ts
const session = link.session(new JobSource(job.layers));
const result = await session.run();
```

### `Link.stats()`

**Syntax**

```ts
stats(): LinkStats

interface LinkStats {
  readonly textDesyncs: number;
  readonly frames: number;
  readonly textLines: number;
  readonly unknownBytes: number;
  readonly overruns: number;
}
```

**Return value**

Counters since construction, for diagnostics:

* `textDesyncs`: reply lines that arrived with no command waiting for them.
  It should stay 0; anything else means a firmware reply with extra lines.
  The e-stop's `ok` and `reset`'s start-up lines aren't counted.
* `frames`, `textLines`: frames and text lines received.
* `unknownBytes`: bytes that started neither a frame nor a line. Above 0
  means a firmware and host of different versions, or a desync.
* `overruns`: lines too long to buffer, dropped.

### `Link.close()`

**Syntax**

```ts
close(): Promise<void>
```

**Return value**

Resolves once the port is closed.

# `CJog`

A continuous jog, from [`Link.cjog()`](#linkcjog). While it runs it sends
the jog packet more often than `CJOG_DEADMAN_MS`, on a clock that keeps
time in a background tab.

## Properties

| Property | Type | |
|---|---|---|
| `started` | `Promise<CJogReply>` | the controller's answer to the start |
| `running` | `boolean` | the jog is being kept alive; false after `stop()`, a refusal, or the link closing |

```ts
type CJogReply = { ok: true } | { ok: false; reason: Nack | null }
```

`reason` is a [`Nack`](packets.md#nack) code, named by
[`names.nack()`](packets.md#names): `NOT_HOMED`, `NO_CONFIG`, `BUSY` (a command is
running), `OUT_OF_RANGE` (no room that way), `BAD_STATE` (a state that takes
no continuous jog). `null` means no answer in time. A refusal stops the jog: `running` turns false.

## Methods

| Method | |
|---|---|
| [`turn()`](#cjogturn) | change direction |
| [`stop()`](#cjogstop) | stop |

### `CJog.turn()`

Changes the direction of a running jog. The machine comes to rest first,
then sets off the new way, so the answer arrives once it has stopped.

**Syntax**

```ts
turn(dir: CJogDirection): Promise<CJogReply>
```

**Parameters**

* `dir`: as for [`cjog()`](#linkcjog). The speed stays the jog's.

**Return value**

The controller's answer to the new direction. A refusal stops the jog.

**Exceptions**

* `RangeError` as for `cjog()`; `Error` if the jog isn't running.

### `CJog.stop()`

Stops the jog at once and stops repeating.

**Syntax**

```ts
stop(): void
```

**Return value**

None. No reply; the status shows the machine at rest.

# `ConfigTransferError`

Thrown by [`pushConfig()`](#linkpushconfig) and
[`pullConfig()`](#linkpullconfig).

| Property | Type | |
|---|---|---|
| `reason` | `CfgNack \| null` | why the controller refused; `null` when it didn't answer or the failure is the host's |
| `message` | `string` | what failed |

**Example**

```ts
try {
  await link.pushConfig(config);
} catch (e) {
  if (!(e instanceof ConfigTransferError)) throw e;
  show(e.reason == null ? e.message : names.cfgNack(e.reason));
}
```

