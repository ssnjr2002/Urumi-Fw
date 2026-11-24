# Link

A connection to the controller ([wire](../../system/wire.md)). Its calls
come in two kinds, by whether they wait their turn:

| Kind | Is | Through |
|---|---|---|
| **in turn** | text commands from [commands.md](../commands.md), one at a time in call order, each answered by one reply line | [`command()`](#linkcommand), the [text-command helpers](#text-command-helpers) |
| **at once** | requests that never wait behind a command: the stops, the status, a continuous jog, the config, a job's records | [`estop()`](#linkestop), [`abort()`](#linkabort), [`onStatus()`](#linkonstatus), [`cjog()`](#linkcjog), [`pushConfig()`](#linkpushconfig), [`session()`](#linksession), … |

The kinds don't follow the wire's planes: `estop()` is the text line
`stop`, sent ahead of any command waiting. Both kinds can be in flight at
once; each waits for its own replies.

The link asks for the machine's status on its own, every `statusPollMs`,
from construction until the port closes, so [`status`](#properties) is
never more than one poll old.

```ts
import { Link, stateName } from "urumi-host/link";
```

**Checked against `cfb8be9`.** To see what may have changed since:

```bash
git diff cfb8be9 -- web/src/link/
```

| Name | Kind |
|---|---|
| [`Link`](#link-1) | class |
| [`CJog`](#cjog) | class |
| [`ConfigTransferError`](#configtransfererror) | error |
| [Names](#names) | functions |
| [`settle()`](#settle), [`waitAtRest()`](#waitatrest) | functions |
| [Text-command helpers](#text-command-helpers) | functions |

The job session (`JobSource`, `Session`) is described in
[jobs](../../system/jobs.md) until it has a page of its own.

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
| [`command()`](#linkcommand) | send a text command, resolve to its reply |
| [`estop()`](#linkestop) | e-stop |
| [`abort()`](#linkabort) | get the machine to IDLE |
| [`freshStatus()`](#linkfreshstatus) | wait for a status sample from after the call |
| [`onStatus()`](#linkonstatus) | call a function on every status sample |
| [`cjog()`](#linkcjog) | start a continuous jog |
| [`pushConfig()`](#linkpushconfig), [`pullConfig()`](#linkpullconfig) | store or fetch the config |
| [`session()`](#linksession) | a job session for a new job |
| [`attach()`](#linkattach) | a job session for a job already paused on the controller |
| [`stats()`](#linkstats) | counters for diagnostics |
| [`close()`](#linkclose) | close the port |

### `Link.command()`

Sends one text command and resolves to its reply. Commands run one at a
time, in call order.

**Syntax**

```ts
command(line: string, timeoutMs?: number): Promise<string>
```

**Parameters**

* `line`: a command from [commands.md](../commands.md), without the
  newline.
* `timeoutMs`: how long to wait for the reply. Default 1000.

**Return value**

The reply line, trimmed; `""` on a timeout.

**Example**

```ts
const reply = await link.command("home xy");
if (reply !== "ok") show(reply);
```

### `Link.estop()`

The e-stop: motion stops at once, without lifting. Sends the `stop`
command, out at once, never behind a pending command.

**Syntax**

```ts
estop(): void
```

**Return value**

None. No reply; the status shows it take hold (`ESTOP`, then `ALARM`).

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
IDLE are refused with `NACK_ABORTING`.

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
if (s.state === MachineState.ALARM) show(faultNames(s.faults));
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
  stateOut.textContent = stateName(s.state);
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
  if (!reply.ok) show(reply.reason == null ? "no answer" : `jog refused: ${nackName(reply.reason)}`);
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

Opens a job session for a new job. Nothing is sent until its `run()`,
which resets the stream's seq first. A session lives through pauses:
`resume()` on the same `Session` carries on.

**Syntax**

```ts
session(source: JobSource): Session
```

**Parameters**

* `source`: the job.

**Return value**

A `Session` ([jobs](../../system/jobs.md)).

**Example**

```ts
const session = link.session(new JobSource(job.layers));
const ok = await session.run();
```

### `Link.attach()`

Opens a job session for a job already open and paused on the controller,
once the old session is gone: after the page reloads or the port is
replugged ([jobs](../../system/jobs.md) §7).

**Syntax**

```ts
attach(source: JobSource): Session
```

**Parameters**

* `source`: the same job, rebuilt from the same layers, so its id matches.

**Return value**

A `Session`. Its `run()` sends nothing but status requests and `get jobid`
until it has checked the controller is in a job with `source`'s id; it
fails with `FATAL_JOB_MISMATCH` otherwise. It waits for the job to land
paused, then holds `paused`, and `resume()` carries on from where the
controller stopped.

To start a paused job over from the top, `cancel()` it and open a new
session instead: the controller refuses a new job while one is open.

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
type CJogReply = { ok: true } | { ok: false; reason: number | null }
```

`reason` is a `NACK_*` code, named by [`nackName()`](#names): `BAD_STATE`
(not homed, no config, or a state that takes no continuous jog), `BUSY`
(a command is running), `SOFT_LIMIT` (no room that way). `null` means
no answer in time. A refusal stops the jog: `running` turns false.

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
| `reason` | `number \| null` | a `CFG_NACK_*` code; `null` when the controller didn't answer or the failure is the host's |
| `message` | `string` | what failed |

**Example**

```ts
try {
  await link.pushConfig(config);
} catch (e) {
  if (!(e instanceof ConfigTransferError)) throw e;
  show(e.reason == null ? e.message : cfgNackName(e.reason));
}
```

# Names

A word for each code, for logs and displays. The word is the constant's
name without its prefix, the same token the firmware logs and the wire
docs use: `stateName(MachineState.ALARM)` is `"ALARM"`, `nackName(NACK_FULL)`
is `"FULL"`. A code this build doesn't know comes back as the kind and the
number, `STATE(7)`, never as a known word.

| Function | Names |
|---|---|
| `stateName(s: number): string` | a `MachineState` |
| `jobName(r: number): string` | a `JobReason` |
| `faultNames(mask: number): string` | the set `Fault` bits, joined with `\|`; `"NONE"` for 0 |
| `nackName(code: number): string` | a `NACK_*` code |
| `cfgNackName(code: number): string` | a `CFG_NACK_*` code |

# Waiting

The controller answers a command before its motion ends, so a script
that must wait for the machine polls its status. These ask for the status
themselves, on top of the link's poll.

## Functions

| Function | |
|---|---|
| [`settle()`](#settle) | poll until a condition holds |
| [`waitAtRest()`](#waitatrest) | poll until the machine is at rest |

### `settle()`

**Syntax**

```ts
settle(link: Link, until: (s: MachineStatus) => boolean, options?: SettleOptions): Promise<MachineStatus>

interface SettleOptions {
  pollMs?: number;
  timeoutMs?: number;
  fatal?: readonly MachineState[];
  onPoll?: (s: MachineStatus) => void;
}
```

**Parameters**

* `link`: the link to poll.
* `until`: the condition. Ready ones: `atRest`, `inState(s)` and
  `inAnyState(...s)`. `atRest` means the controller would take the next
  command: outside a job, `IDLE` with nothing queued; in a job, paused, or
  buffering with nothing queued.
* `options.pollMs`: the gap between polls. Default 150.
* `options.timeoutMs`: give up after this long; 0 waits forever. Default 0.
* `options.fatal`: states that end the wait with an error. Default `ESTOP`
  and `ALARM`; `[]` waits through them.
* `options.onPoll`: called with every sample, the last included.

**Return value**

The sample that met the condition. The first poll is at once.

**Exceptions**

* `SettleError` on a timeout or a fatal state. Its `reason` is `"timeout"`
  or `"fatal"`, and `status` the last sample.

**Example**

```ts
await link.command("goto w xy 100 50");
await settle(link, atRest, { timeoutMs: 30000 });
```

### `waitAtRest()`

Waits until the machine is at rest, without throwing.

**Syntax**

```ts
waitAtRest(link: Link, timeoutMs?: number, options?: SettleOptions): Promise<boolean>
```

**Parameters**

* `link`: the link to poll.
* `timeoutMs`: default 5000.
* `options`: as for `settle()`, without `timeoutMs` and `fatal`; `pollMs`
  defaults to 50.

**Return value**

`true` once [`atRest`](#settle), `false` on a timeout. `ALARM` and
`ESTOP` count as rest: the machine isn't moving.

# Text-command helpers

Typed wrappers over [`command()`](#linkcommand), one per command in
[commands.md](../commands.md). Each takes the link first and parses the
reply. How a refusal shows varies: some resolve to `false` or `null`,
some to the reply line, some throw. Their parameters and results are on
their types.

| Group | Helpers |
|---|---|
| reachability | `ping`, `pingNode`, `pingAll` |
| keys | `get`, `getKeys`, `getState`, `GET_MAX_KEYS`, `STATE_KEYS` |
| nodes | `nodePos`, `nodeStat`, `enable`, `disable` |
| machine | `setOrigin`, `axisMap`, `readAxisMap`, `unalarm`, `jog` |
| legs | `linLeg`, `rotLeg`, `parkLeg`, `legAbort` |
| probe | `probeMap`, `probeLeg`, `probeEnd`, `setProbe`, `unprobe` |
| peripherals | `vacServo`, `vacPump`, `vacSwitch`, `knifeOsc`, `knifeBlower` |
| job | `pause`, `cancel` |

The e-stop and the status are on `Link` itself: [`estop()`](#linkestop),
[`status`](#properties), [`freshStatus()`](#linkfreshstatus).

```ts
const faults = await unalarm(link);
```
