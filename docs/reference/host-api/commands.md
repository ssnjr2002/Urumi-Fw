# Commands

The text commands of [commands.md](../commands.md) as methods on one
object, from [`link.commands()`](link.md#linkcommands). Each method sends
its command, reads the one reply line, and either resolves with what the
reply says or throws a [`CommandError`](#commanderror).

```ts
import { Link, CommandError } from "urumi-host/link";

const cmd = link.commands();
await cmd.home({ form: "xy" });
```

**Checked against `e8de999`.** To see what may have changed since:

```bash
git diff e8de999 -- web/src/link/
```

| Name | Kind |
|---|---|
| [`Commands`](#commands-1) | class |
| [`CommandError`](#commanderror) | error |

## What a reply tells

The reply only says whether the controller accepted the command. It
doesn't say when the command is done. `ok` from `home`, `goto`, `jog`,
`mount` or `leg` means the motion has started, and it keeps going after
the reply. The controller never reports that a command has finished.

So the host can know two things:

* from the reply: whether the command was accepted (`ok …`) or refused
  (`err <reason>`);
* from the status's [`busy`](packets.md#machinestatus): whether something
  is moving right now. While it is, most commands are refused with
  `err busy`.

`busy` false doesn't mean the next command will be accepted. A machine in
ALARM isn't busy, but it still refuses `home`. What each command needs is
in [commands.md](../commands.md#when-a-command-runs).

To send a command as soon as the machine stops moving, use
[`cmd.onReady`](#commandsonready). To do something when a move ends,
watch the status with [`onStatus()`](link.md#linkonstatus).

# `Commands`

The link's text commands. One per link, made by the link:
[`link.commands()`](link.md#linkcommands) returns the same object every
time. No public constructor.

Commands go out one at a time, in call order, each waiting for its reply
(default 1000 ms). The e-stop and the abort never wait behind them: they
are [`link.estop()`](link.md#linkestop) and
[`link.abort()`](link.md#linkabort).

## Properties

| Property | Type | |
|---|---|---|
| [`onReady`](#commandsonready) | `ReadyCommands` | the same methods, each waiting until the machine isn't busy before it sends |

## Methods

Every method but [`send()`](#commandssend) resolves when the reply is
`ok` (with the value the reply carries, where there is one) and throws a
[`CommandError`](#commanderror) on `err`, a node failure, a reply it
can't parse, or no reply in time.

| Group | Methods |
|---|---|
| any line | [`send()`](#commandssend) |
| reads | [`ping()`](#commandsping), [`pingNode()`](#commandspingnode), [`pingAll()`](#commandspingall), [`get()`](#commandsget), [`keys()`](#commandskeys), [`nodeStat()`](#commandsnodestat), [`nodePos()`](#commandsnodepos), [`vacSwitch()`](#commandsvacswitch) |
| lifecycle | [`unalarm()`](#commandsunalarm), [`reset()`](#commandsreset) |
| bus and nodes | [`enable()`](#commandsenable), [`disable()`](#commandsdisable), [`axesEnable()`](#commandsaxesenable), [`axesMap()`](#commandsaxesmap), [`slotMap()`](#commandsslotmap) |
| peripherals | [`vacServo()`](#commandsvacservo), [`vacPump()`](#commandsvacpump), [`knifeOsc()`](#commandsknifeosc), [`knifeBlower()`](#commandsknifeblower) |
| homing | [`home()`](#commandshome), [`leg()`](#commandsleg), [`legAbort()`](#commandslegabort), [`setOrigin()`](#commandssetorigin) |
| probe | [`probeMap()`](#commandsprobemap), [`probeEnd()`](#commandsprobeend), [`setProbe()`](#commandssetprobe), [`unprobe()`](#commandsunprobe) |
| positioning | [`select()`](#commandsselect), [`wzero()`](#commandswzero), [`wset()`](#commandswset), [`wclear()`](#commandswclear), [`jog()`](#commandsjog), [`goto()`](#commandsgoto) |
| tools | [`mount()`](#commandsmount), [`unmount()`](#commandsunmount) |
| jobs | [`pause()`](#commandspause), [`cancel()`](#commandscancel), [`place()`](#commandsplace) |

The other commands in [commands.md](../commands.md) (`unstop`,
`bus_exclude`, `laser`, `set`, `save`, the motion primitives, …) go
through `send()`.

### `Commands.send()`

Sends any line and resolves to its reply, unparsed. For a console, or a
command with no method.

**Syntax**

```ts
send(line: string, timeoutMs?: number): Promise<string>
```

**Parameters**

* `line`: a command, without the newline.
* `timeoutMs`: how long to wait for the reply. Default 1000.

**Return value**

The reply line, trimmed; `""` on a timeout. `stop` goes out as
[`link.estop()`](link.md#linkestop) and resolves to `""` at once. `reset`
resolves to its `ok`, as [`reset()`](#commandsreset).

**Example**

```ts
const reply = await cmd.send(line);
log(reply || "(no reply)");
```

### `Commands.onReady`

The same methods as on `Commands`, but each waits until the machine isn't
busy before sending its command. If the machine is busy, the method waits
until the status shows `busy` false, then sends. It sends only once: if
the command is still refused, it throws, and doesn't wait and try again.

The reads aren't on it: they're always taken, busy or not.

```ts
await cmd.onReady.goto({ name: "park" });
```

Waiting only gets past `busy`. The command can still be refused for
another reason (`err bad_state` in ALARM, say), and then it throws just as
it would on `Commands`. It watches the status the link already polls; it
doesn't send status requests of its own.

It waits as long as it takes; there's no timeout.

**Exceptions**

* [`CommandError`](#commanderror) with `reason` `"estop"` if the e-stop
  is latched, either when called or while waiting. Waiting can't clear an
  e-stop, so it gives up instead of waiting forever.
* `Error` if the link closes while it waits.

### `Commands.ping()`

Is the controller answering.

**Syntax**

```ts
ping(): Promise<void>
```

**Return value**

Resolves on `pong`.

### `Commands.pingNode()`

Pings one bus node.

**Syntax**

```ts
pingNode(id: number): Promise<void>
```

**Return value**

Resolves on `node <id> ok`. Any other node result throws, with `reason`
the result (`timeout`, `excluded`, …).

### `Commands.pingAll()`

Pings every bus address in one command.

**Syntax**

```ts
pingAll(): Promise<Map<number, string>>
```

**Return value**

Each address and its node result: `"ok"`, `"timeout"`, `"excluded"`, …
([node results](../commands.md#node-results)). An address that doesn't
answer isn't a failure here.

### `Commands.get()`

Reads keys from one snapshot.

**Syntax**

```ts
get(keys: readonly string[]): Promise<Map<string, string>>
```

**Parameters**

* `keys`: key or group names ([`get`](../commands.md#get)), 1 to 32
  after the controller expands groups.

**Return value**

Each key and its raw value, in the order asked. A value can be a marker:
`?` the read failed, `-` the key doesn't apply now, `!` this firmware
doesn't know the key.

**Exceptions**

* `RangeError` for no keys or more than 32.
* [`CommandError`](#commanderror) as for any method.

**Example**

```ts
const r = await cmd.get(["homefail", "homenode"]);
show(`leg failed on node ${r.get("homenode")}: ${r.get("homefail")}`);
```

### `Commands.keys()`

The keys this firmware knows (bare `get`).

**Syntax**

```ts
keys(): Promise<string[]>
```

### `Commands.nodeStat()`

One node's state (`nodestat`).

**Syntax**

```ts
nodeStat(id: number): Promise<NodeStat>

interface NodeStat {
  readonly id: number;
  readonly type: number;
  readonly enabled: boolean;
  readonly datum: boolean;
  readonly limit: boolean | undefined;
  readonly homing: boolean;
  readonly pos: number | undefined;
  readonly span: number | undefined;
  readonly index: number | undefined;
  readonly indexCause: string | undefined;
  readonly crossings: number | undefined;
  readonly stepsPerRev: number | undefined;
}
```

**Return value**

The node's state. A field the node didn't send is `undefined`, never a
default: `limit` on a node with no switch, `pos` on a node that isn't a
stepper, `span` before a span leg, the index fields on a linear node.
`index` is present only when `indexCause` is `"ok"`.

### `Commands.nodePos()`

A node's own step counter, to check against the controller's position.

**Syntax**

```ts
nodePos(id: number): Promise<number>
```

### `Commands.vacSwitch()`

A vacuum node's switch.

**Syntax**

```ts
vacSwitch(id: number): Promise<boolean>
```

**Return value**

`true` open (actuated), `false` closed (at rest).

### `Commands.unalarm()`

Clears the event faults and fixes what it can
([`unalarm`](../commands.md#unalarm)).

**Syntax**

```ts
unalarm(): Promise<number>
```

**Return value**

The fault mask left; 0 means IDLE.

### `Commands.reset()`

Soft-resets the controller.

**Syntax**

```ts
reset(): Promise<void>
```

**Return value**

Resolves once the controller has restarted: after its `ok`, and the
start-up lines that follow it, through `ready`. They aren't counted as
desyncs.

### `Commands.enable()`

Energises one node.

**Syntax**

```ts
enable(id: number): Promise<void>
```

**Return value**

Resolves on `ok`, whatever the node answered; the status's
`nodeEnabled` shows whether it took.

### `Commands.disable()`

De-energises one node; its origin is lost.

**Syntax**

```ts
disable(id: number): Promise<void>
```

### `Commands.axesEnable()`

Energises or de-energises the nodes in the axis map. `false` loses their
origins.

**Syntax**

```ts
axesEnable(on: boolean): Promise<void>
```

### `Commands.axesMap()`

Reads or sets the axis map: which config axis node drives X, Y, Z and A.

**Syntax**

```ts
axesMap(): Promise<AxisNodes>
axesMap(map: AxisNodes): Promise<void>

type AxisNodes = readonly [x: number | null, y: number | null, z: number | null, a: number | null];
```

**Parameters**

* `map`: a bus id per axis, `null` for none. Each id must be a stepper
  the config marks present.

**Return value**

With no `map`, the current map. A node the controller is still
connecting to its axis shows as already mapped.

Inside a probe session, setting the map ends the session.

### `Commands.slotMap()`

Reads or sets the four stream slots' nodes directly, with no config.
For bring-up; [`axesMap()`](#commandsaxesmap) is the checked form.

**Syntax**

```ts
slotMap(): Promise<AxisNodes>
slotMap(map: AxisNodes): Promise<void>
```

**Exceptions**

* [`CommandError`](#commanderror) with `reason` `"fenced"` if a slot is
  fenced after an e-stop: `unstop` first.

### `Commands.vacServo()`

A vacuum node's servo.

**Syntax**

```ts
vacServo(id: number, servo: number, on: boolean): Promise<void>
```

**Parameters**

* `servo`: 1 to 6, or 0 for all.

Switching on is refused while any fault is set; switching off always
runs. The same holds for every peripheral.

### `Commands.vacPump()`

A vacuum node's pump, soft-started.

**Syntax**

```ts
vacPump(id: number, on: boolean): Promise<void>
```

### `Commands.knifeOsc()`

A knife node's oscillator.

**Syntax**

```ts
knifeOsc(id: number, on: boolean): Promise<void>
```

### `Commands.knifeBlower()`

A knife node's blower.

**Syntax**

```ts
knifeBlower(id: number, duty: number): Promise<void>
```

**Parameters**

* `duty`: percent, 0 to 100; 0 is off.

**Exceptions**

* `RangeError` if `duty` isn't 0 to 100.

### `Commands.home()`

Homes from the config's homing cycles ([`home`](../commands.md#home)).
Resolves once homing starts; the status shows it run (`HOMING`) and end
(`IDLE`, or `ALARM` with the HOMING fault).

**Syntax**

```ts
home(target?: HomeTarget): Promise<void>

type HomeTarget =
  | { nodes: readonly number[]; only?: boolean }
  | { cycle: number }
  | { head: number }
  | { form: "unhomed" | "xy" };
```

**Parameters**

* none: every homeable node.
* `nodes`: these nodes, after clearing the cycles before theirs; with
  `only`, these alone.
* `cycle`: that cycle's nodes.
* `head`: that head's Z and A, alone.
* `form`: `"unhomed"` every homeable node with no origin; `"xy"` X and Y.

**Example**

```ts
await cmd.home({ form: "xy" });
const off = link.onStatus((s) => {
  if (s.state === MachineState.HOMING) return;
  off();
  show(s.state === MachineState.IDLE ? "homed" : faultNames(s.faults));
});
```

### `Commands.leg()`

Runs one raw leg on one node, in the node's own steps and microseconds
([`leg`](../commands.md#leg)). Resolves once the leg is armed; the
status shows it run.

**Syntax**

```ts
leg(leg: Leg): Promise<void>

type Leg =
  | { form: "seek" | "retract" | "sweep" | "span" | "unlatch"; id: number; dir: 0 | 1; ramp: Ramp; maxSteps: number }
  | { form: "park"; id: number; target: number; ramp: Ramp }
  | { form: "probe"; id: number; dir: 0 | 1; ramp: Ramp; pollDiv: number; maxSteps: number; deadlineUs: number; expectOpen: boolean };

interface Ramp {
  readonly startUs: number;   // the step interval it starts at
  readonly endUs: number;     // the interval it ramps to: floor, or a probe's ceiling
  readonly steps: number;     // steps the ramp takes
}
```

**Parameters**

* `form`: what the leg does; see the `leg` table in
  [commands.md](../commands.md#leg). `seek` must start off the switch and
  `retract` on it; the node checks and refuses the wrong one.
* `id`: the node's bus id. A leg names a node, not an axis, so it works
  before any axis map is set.
* `dir`: 0 or 1, in the node's own direction, not + or − on the axis.
* `maxSteps`: for a seek, the most it may travel before giving up; for a
  retract, exactly how far it travels; for a sweep, a safety limit.
* `target`: where a park ends, on the node's own step counter. The node
  must have an origin.
* `pollDiv`: a probe leg checks the switch every this many steps.
* `deadlineUs`: a probe leg gives up after this long.
* `expectOpen`: whether the host expects the switch to be open (the tool
  touching) when the probe leg starts.

### `Commands.legAbort()`

Stops a node's leg; the leg fails as aborted. An idle node just
acknowledges.

**Syntax**

```ts
legAbort(id: number): Promise<void>
```

### `Commands.setOrigin()`

Sets each node's datum: it stands at machine position `steps`. In a
homing session it also ends the session.

**Syntax**

```ts
setOrigin(pairs: readonly { id: number; steps: number }[]): Promise<void>
```

**Exceptions**

* [`CommandError`](#commanderror) naming the first node that failed; the
  others kept their datum.

### `Commands.probeMap()`

Opens a probe session: the bound Z, with the switch on vacuum node `vac`.
Its legs are then [`leg()`](#commandsleg) with `form: "probe"`.

**Syntax**

```ts
probeMap(vac: number): Promise<{ z: number; switchOpen: boolean }>
```

**Return value**

The session's Z node, and the switch as the session opened.

### `Commands.probeEnd()`

Ends the probe session and puts the map back.

**Syntax**

```ts
probeEnd(): Promise<boolean | null>
```

**Return value**

The switch: `true` open (the tool may rest on it), `false` closed, `null`
the read failed.

### `Commands.setProbe()`

Records the bound Z's contact height, machine steps.

**Syntax**

```ts
setProbe(zSteps: number): Promise<void>
```

### `Commands.unprobe()`

Forgets a probe height: the bound Z's, or node `id`'s.

**Syntax**

```ts
unprobe(id?: number): Promise<void>
```

### `Commands.select()`

Picks the controlled point: a head, or the laser (`"anchor"`). Never
moves.

**Syntax**

```ts
select(target: number | "anchor"): Promise<void>
```

### `Commands.wzero()`

Sets the work zero to where the selected tip is now, on these axes.

**Syntax**

```ts
wzero(axes?: readonly ("x" | "y" | "z" | "zSwitch")[]): Promise<void>
```

**Parameters**

* `axes`: none means X, Y, and Z when a head's Z is bound. `"zSwitch"`
  zeroes Z with the tip on the head's switch.

### `Commands.wset()`

Sets the work offset in machine mm, per axis.

**Syntax**

```ts
wset(offset: { x?: number; y?: number; z?: number }): Promise<void>
```

**Exceptions**

* `RangeError` for no axis.

### `Commands.wclear()`

Puts the work zero's X and Y back at the work area's min corner, and
forgets every head's work Z.

**Syntax**

```ts
wclear(): Promise<void>
```

### `Commands.jog()`

Moves by a distance: one axis, or X and Y together. Jogs queue up, so
each starts from where the previous one ends. At most 4 can be queued.

**Syntax**

```ts
jog(by: JogDistance, options?: { scale?: number }): Promise<number>

type JogDistance =
  | { x?: number; y?: number }
  | { z: number }
  | { a: number };
```

**Parameters**

* `by`: mm (A: degrees), signed. One axis set per jog, as for
  [`cjog()`](link.md#linkcjog).
* `options.scale`: a multiple of the axis's jog feed. Default 1.

**Return value**

The jogs queued, this one included.

**Exceptions**

* `RangeError` if `by` moves more than one axis set, or none.
* [`CommandError`](#commanderror) with `reason` `"busy"` when 4 are
  already queued.

### `Commands.goto()`

Moves to a position ([`goto`](../commands.md#goto)). Resolves once the
move starts, not when it arrives.

**Syntax**

```ts
goto(target: GotoTarget): Promise<void>

type GotoTarget =
  | { frame: "m" | "w"; x: number | "min" | "max"; y: number | "min" | "max"; scale?: number }
  | { frame: "m" | "w"; z: number; scale?: number }
  | { frame: "m" | "w"; a: number; scale?: number }
  | { name: "park" | "load" | "mount" | "probe"; scale?: number }
  | { name: "up" | "unlock" };
```

**Parameters**

* `frame`: `"m"` machine (X and Y the anchor's), `"w"` work, for the
  selected tip.
* `x`, `y`: mm, or `"min"` / `"max"`, that end of the soft range. XY
  moves park every Z first.
* `z`, `a`: mm, degrees.
* `name`: a stored position (`park`, `load`, `mount`), the head's probe
  switch, every Z parked (`up`), or every A to 0 (`unlock`).
* `scale`: a multiple of the jog feed. Default 1.

**Example**

```ts
await cmd.onReady.goto({ frame: "w", x: "min", y: "min" });
```

### `Commands.mount()`

Tells the controller head `head` now holds tool `tool`. On a head with an
unlock range, its Z parks and its A turns to lock the holder; the method
resolves once that move starts.

**Syntax**

```ts
mount(head: number, tool: number): Promise<void>
```

**Exceptions**

* [`CommandError`](#commanderror) with `reason` `"mounted"` if the head
  has a tool: `unmount()` first.

### `Commands.unmount()`

Tells the controller head `head` holds no tool. Its Z parks and, on a
head with an unlock range, its A turns to unlock the holder.

**Syntax**

```ts
unmount(head: number): Promise<void>
```

### `Commands.pause()`

Pauses the job: it brakes to a hold and lands paused. A job session
pauses through its own [`pause()`](session.md#sessionpause).

**Syntax**

```ts
pause(): Promise<void>
```

### `Commands.cancel()`

Ends a job at rest. A job session ends through its own
[`close()`](session.md#sessionclose).

**Syntax**

```ts
cancel(): Promise<void>
```

### `Commands.place()`

Places a job on the bed with the laser dot ([`place`](../commands.md#place)),
while the job is in PLACE. Whether `place set` stands is the status's
[`placed`](packets.md#machinestatus).

**Syntax**

```ts
place(form: "set" | "reset" | "frame" | "ok"): Promise<void>
place(form: "go", corner: { x: "min" | "max"; y: "min" | "max" }): Promise<void>
```

**Parameters**

* `"set"`: the dot becomes the job's (xmin, ymin).
* `"reset"`: clears it; the dot jogs again.
* `"go"`: the dot to that corner of the job.
* `"frame"`: traces the job's box once.
* `"ok"`: signs off; the job buffers.

**Example**

```ts
try {
  await cmd.place("set");
} catch (e) {
  if (!(e instanceof CommandError)) throw e;
  hint(`place set: ${e.reason}`);
}
```

# `CommandError`

Thrown by every [`Commands`](#commands-1) method but `send()`.

| Property | Type | |
|---|---|---|
| `command` | `string` | the line sent |
| `reason` | `string` | why, one token: the word after `err` (`"busy"`, `"not_homed"`, …), a node's result (`"timeout"`, `"excluded"`), `"timeout"` for no reply, `"bad_reply"` for one that doesn't parse, or `"estop"` from `onReady` |
| `reply` | `string` | the reply line in full; `""` for no reply |

`reason` is the token to branch on; `reply` keeps the rest
(`err soft_limit x 12.5`, `err node 4 nak intent_mismatch`) for a log.

**Example**

```ts
try {
  await cmd.goto({ name: "park" });
} catch (e) {
  if (!(e instanceof CommandError)) throw e;
  if (e.reason === "busy") show("still moving");
  else show(e.reply);
}
```
