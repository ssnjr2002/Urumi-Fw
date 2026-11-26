# Commands

Every text command the controller answers: every row of its command tables
(`kCommands` and `kControllerCommands` in
`src/rp2350/core0/control_plane.cpp`), and every key `get` reads.

**Checked against `d12bc73`.** To see what may have drifted since:

```bash
git diff d12bc73 -- src/rp2350/core0/control_plane.cpp src/rp2350/core0/cmd src/rp2350/core0/controller src/rp2350/core0/ops src/rp2350/core0/config/runtime_edit.cpp
```

Strings are written as the [notation](../README.md#notation) says. Binary
records, including `STATUS_RSP`, are in [packets.md](packets.md).

## Synopsis notation

| Written | Means |
| --- | --- |
| `home` | typed as shown |
| `<id>` | a value you fill in; `<id>` is a bus id, 1–8 |
| `[sps]` | optional |
| `on\|off` | one of these |
| `<id> …` | one or more |

Each form of a command gets its own synopsis line.

## Index

| Group | Commands |
| --- | --- |
| [Reads](#reads) | `ping`, `?`, `get`, `cfg`, `pingnode`, `busstat`, `nodestat`, `nodepos`, `vac_switch` |
| [Lifecycle](#lifecycle) | `stop`, `unstop`, `unalarm`, `reset`, `uncfg`, `seqreset` |
| [Bus and nodes](#bus-and-nodes) | `bus_exclude`, `bus_enable`, `axes_enable`, `enable`, `disable`, `makesafe`, `slot_map`, `axes_map` |
| [Peripherals](#peripherals) | `vac_servo`, `vac_pump`, `knife_osc`, `knife_blower`, `laser` |
| [Motion primitives](#motion-primitives) | `step`, `line`, `bez`, `hallscan` |
| [Homing](#homing) | `home`, `leg`, `leg_abort`, `dummy_leg`, `setorigin`, `home_end` |
| [Probe](#probe) | `probe_map`, `probe_end`, `setprobe`, `unprobe` |
| [Positioning](#positioning) | `select`, `wzero`, `wset`, `wclear`, `jog`, `goto`, `mesh` |
| [Tools](#tools) | `mount`, `unmount` |
| [Jobs](#jobs) | `pause`, `cancel`, `feed`, `place` |
| [Config](#config) | `set`, `save` |

## Lines and replies

One command per line, ended by `\n` or `\r`; an empty line is ignored. A line
is at most 255 characters: bytes past that are dropped, and the line runs as
cut. The first word picks the row; the rest are its arguments, separated by
spaces. Every command answers exactly one line:

| Reply | Means |
| --- | --- |
| `ok`, or `ok <detail>` | done, or started for a command that takes time |
| `err <reason>` | refused; nothing changed unless the entry says so |
| `node <id> <result>` | a command relayed to one node, with the node's answer |
| anything else | the command's own read, as its entry says |

A line whose first word is in neither table answers `err unknown`.

### Node results

`<result>` in a `node <id> <result>` reply, and in `err node <id> <result>`:

| Result | Means |
| --- | --- |
| `ok` | the node acknowledged |
| `timeout` | no answer in time |
| `bad_reply` | an answer that doesn't parse |
| `pending` | the exchange hasn't finished |
| `excluded` | the node is excluded (`bus_exclude`); nothing was sent |
| `nak <reason>` | the node refused: `unsupported`, `bad_token`, `bad_arg`, `intent_mismatch`, `busy`, `no_datum` |

### Refusals

What each `<reason>` in `err <reason>` means wherever it appears. An entry
explains a reason again only where it means something narrower there.

| Reason | Means |
| --- | --- |
| `usage` | the arguments don't match any form |
| `busy` | something running is in the way, or a fixed table is full (queued jogs, legs) |
| `bad_state` | the machine's state doesn't admit it now |
| `unconfigured` | no valid config |
| `estop` | the e-stop is latched |
| `degraded` | the BUS fault: a node is mute; `bus_exclude` it |
| `unmapped` | the slot map is unmet |
| `bad_node` | not a bus id |
| `dup` | a node named twice |
| `unbound` | no axis node bound where one is needed (the axis map, or its Z) |
| `not_engaged` | the node holds no stream slot |
| `not_enabled` | the node is not energised |
| `not_homed` | an axis or node it needs has no origin; `not_homed <axis>` names it |
| `a_not_homed` | a head's A has no origin |
| `no_head` | no head is selected, or the selection has no tip |
| `no_tool` | no such tool in the config, or none mounted |
| `no_feed` | the feed works out to 0: the axis's `jogFeed` is unset |
| `no_limits` | an axis the move needs has `maxFeed` or `maxAccel` 0 |
| `full` | the planner's ring is full; retry as it drains |
| `bad_curve` | a degenerate handle, a cusp, or a curve the planner can't fit |
| `soft_limit <axis>` | the move leaves the axis's soft range; a jog or `goto` adds `<mm>`, how far it could still go |
| `too_far` | further than an unhomed axis may move (its `maxTravel`, one turn on a rotary axis), or no heading within `aMaxDeg` |
| `range` | a number out of range, or a zero interval or budget |
| `node <id> <result>` | a node the command needed refused or didn't answer; `<result>` as above |
| `node <id> mute`, `node <id> excluded` | a node the command needed is mute or excluded |
| `node <id> not_in_config` | the node is not an axis node the config marks present |

## When a command runs

Each command has a kind. The kind is checked before the handler runs; a refused
command answers `err busy` when something running is the reason, else
`err bad_state`. Entries don't repeat these two. Contexts, sequences and steps
are described in [system/controller.md](../system/controller.md#admission).

| Kind | Admitted when |
| --- | --- |
| `any` | always |
| `read` | always |
| `write` | nothing running, any e-stop swept, and no context open or a job paused |
| `step` | nothing running, no e-stop latched, no job |
| `sequence` | nothing running, no e-stop latched, no context open, and no fault but a held limit latch |
| `context` | no e-stop latched |
| `clear` | nothing running and no context open |

"Running" is a leg, a jog or an unpaused job's motion, or a sequence; `busy`
(`get`, `STATUS_RSP`) says whether something is. The
contexts are a job, a probe session and a homing session; inside a probe or
homing session only `any`, `read` and the commands flagged for it are admitted.

| Flag | Adds |
| --- | --- |
| `homing` | also inside a homing session |
| `probe` | also inside a probe session |
| `tool wait` | also while a job waits for a tool, with nothing running and no e-stop latched |
| `place` | also while a job is being placed, as for `tool wait` |
| `bare read` | the form with no arguments is a read, admitted always |
| `needs config` | `err unconfigured` without a valid config, before the kind |

Each entry's **Runs** line names the kind and flags, then any narrower check
the handler makes after them.

---

## Reads

### ping

```
ping
```

Is the controller answering.

**Replies:** `pong`
**Runs:** `read`

### ?

```
?
```

Shorthand for `get status`.

**Replies:** as `get status`
**Runs:** `read`

### get

```
get
get <key> …
```

Reads keys from one snapshot, in the order asked. Bare lists the keys this
build knows. A group name stands for its keys, expanded in place; group and key
names share one namespace. A key asked for twice is printed once, where first
asked, so `get status homed` prints `homed` once. At most 32 keys after
expansion.

`config.<path>` reads one editable field of the active config, as `set` names
it (exact case): `x,y` for a point, `-` for an absent optional field, `!` for
no such field.

In place of a value: `?` the read failed, `-` the key doesn't apply now, `!`
the key is not recognised (older firmware).

**Replies:**

- `<key>=<value> …`
- `keys <key> …`: bare
- `err too_many_keys`: more than 32 after expansion

**Runs:** `read`

The one group:

```
status = state ctx homed enabled head tools unlocked mpos wpos buf busy mute excluded touched mesh
```

The keys, in the order bare `get` lists them. A mask is hex, bit n = bus node
n (or head n, for `unlocked`), whether mapped or not.

| Key | Value | `-` when |
| --- | --- | --- |
| `state` | 0 IDLE, 2 ESTOP, 3 ALARM, 5 HOMING, 6 PROBING, 7 JOGGING, 8 TRAVERSE, 9 JOB | |
| `ctx` | 0 IDLE, 1 JOB, 2 PROBE, 3 HOMING | |
| `enabled` | mask: nodes energised | |
| `homed` | mask: nodes holding an origin | |
| `faults` | the fault mask, as `STATUS_RSP` (e.g. `0x0004`) | |
| `job` | 0 BUFFERING, 1 RUNNING, 2 HOLDING, 3 PAUSED, 4 ABORTING, 5 TOOL_CHANGE, 6 TOOL_WAIT, 7 DWELL, 8 PREFLIGHT, 9 PLACE | not in a job |
| `resume_layer` | the layer a paused job resumes at | no resume point |
| `resume_record` | the record within it | no resume point |
| `liftreason` | the latest lift's reasons, comma-separated: `end`, `corner`, `duty`, `deadline`, `unwind`, `pause`, `abort` (`dry` reserved) | not JOB, or before the first lift |
| `liftcount` | lifts begun since the job opened | not JOB |
| `toolon` | 0 or 1: the powered tool's switch | not JOB, or no powered tool |
| `onms` | its duty on-time since the duty's last reset, the open span included, ms | as `toolon` |
| `latched` | mask: nodes held by a latched limit | |
| `homefail` | the failed leg's cause (`LEGFAIL_*`, `ops/homing.h`) | no HOMING fault |
| `homenode` | that leg's node; 0 for a dummy leg | as `homefail` |
| `seq` | the running sequence, else the one a SEQUENCE, HOMING or PROBE fault ended: 1 LEG, 2 TRAVERSE, 3 HOME, 4 RECOVERY, 5 GOTO, 6 TOOL_CHANGE, 7 PLACE | none |
| `seqfail` | what refused a sequence after its `ok`, one token (e.g. `soft_limit`) | no SEQUENCE fault |
| `cfgerr` | why the boot config was refused: `absent`, `fs`, `file`, or a decode error | no CONFIG fault |
| `host` | 1 heard, 0 lost: nothing from the host for 3 s, until it is heard again; or DTR fell (the port closed), until the job pauses or ends, even if a host reopens the port ([jobs](../system/jobs.md) §6) | not in a job |
| `pausereason` | why the job last paused: 0 the operator, 1 a lost host | |
| `jobid` | the open job's id from its `JOB_HDR`, 8 hex digits | not in a job |
| `dtrdrops` | DTR falls since power-on: the host closed the port or the cable was pulled | |
| `cfgdirty` | 1 while the active config holds edits `save` hasn't stored | |
| `homing` | 0 LEG (a leg runs), 1 WAIT (none) | not HOMING |
| `jogging` | 1 STEP (`jog`, `line`, `bez`), 2 CONT (a continuous jog), 3 GOTO | not JOGGING |
| `probing` | 0 LEG, 1 CLEAR (switch closed), 2 CONTACT (switch open) | not PROBING and no PROBE fault |
| `probe` | the last leg's cause (`PROBE_*`) | as `probing` |
| `retries` | the last leg's retries | as `probing` |
| `psteps` | the last leg's steps | as `probing` |
| `probed` | 1 while a probe height is held for the bound Z | |
| `pz` | that height, machine steps | `probed=0` |
| `homecycle` | the cycle a `home` run is in | no run |
| `mute` | mask: nodes the config names, or touched, that didn't confirm make-safe to the boot sweep | |
| `excluded` | mask: mute nodes `bus_exclude` named | |
| `touched` | mask: nodes energised or holding a slot, bound or fenced | |
| `buf` | planner blocks queued | |
| `busy` | 1 while a step or sequence runs: a command that needs nothing running answers `err busy`; 0 in a paused job. As `STATUS_RSP` | |
| `queued` | motion time of the queued blocks not yet started, µs | |
| `expectedseq` | the next wire seq the data plane executes; not flow control | |
| `pos` | `<id>:<steps>,…` per config axis node, id order: the machine position the controller believes; `<id>:-` unknown. `nodepos` reads the node's own counter | no config axis node |
| `mpos` | `x,y,z,a`: machine position, mm and degrees; `-` per axis unbound or unhomed | |
| `wpos` | `x,y,z,a`: work position of the selected tip (the blade tip with an `offsetMm`) less the work offset; A the edge heading (`mpos` A + the tool's `aOffsetDeg`) folded to (−180, 180]. `-` per axis unbound or unhomed; X, Y with no head; Z until the head's Z is zeroed. `STATUS_RSP` carries NaN for each `-` | |
| `head` | the controlled point: a head index, or `anchor` (the laser) | none selected |
| `laser` | `on` or `off`, as its node last acked `CMD_LASER`; `?` before the first command or after a failed one | no laser |
| `tools` | the tool id each head has mounted, head order, `-` for none | no config |
| `unlocked` | mask by head: holders that can release now (IDLE or JOB.TOOL_WAIT, nothing running, its A homed and inside `heads[].a.unlockRange`) | no head has an `unlockRange` |
| `late` | planner blocks Core 1 found late, since boot: a job lands PAUSED, a jog holds and runs on | |
| `amiss` | tangential curves whose tracked A ended over 0.5° from the planned A, since boot; A snapped to it | |
| `mesh` | the bed mesh: `<nx>x<ny>` followed, `off` (loaded, not followed), `flat` (no `/mesh.bin`), `bad` (unreadable) | |
| `meshslope` | its steepest slope between neighbouring points, mm/mm | no mesh loaded |

A lift is Z rising out of a cut or a plunge for one of `liftreason`'s reasons,
with the A, tool events or XY it carries and the plunge back; one lift carries
every reason due at its spot (a BREAK's duty stop and corner swivel are one
lift, `duty,corner`). A START's travel is not a lift. `liftreason` stays on
the latest lift until the next one; `liftreason`, `liftcount`, `toolon` and
`onms` reset as a job opens, not as it resumes.

### cfg

```
cfg
```

The stored config file, whether it decoded, and the last rejection. Answers
with or without a config.

**Replies:**

- `cfg fs=unmounted`
- `cfg none`
- `cfg seq=<n> len=<n> crc=<hex>`, then `schema=<n>` or `decoded=0`

Any of these is followed by `rejected=<error>`, `ignored=1` (after `uncfg`) or
`dirty=1` where they apply.

**Runs:** `read`

### pingnode

```
pingnode
pingnode all
pingnode <id>
```

Pings one node, or every bus address in one line.

**Replies:**

- `nodes <id>=<result> …`: every address
- `node <id> <result>`
- `err bad_node`
- `err bad_state`: the bus isn't free

**Runs:** `read`; the bus free: at rest, or a homing session between legs

### busstat

```
busstat <id>
```

A node's receive-error counters: framing errors, overflows, CRC failures.
Wrapping 16-bit counts since the node powered on.

**Replies:**

- `node <id> ferr <n> ovf <n> crc <n>`
- `node <id> <result>`
- `err usage`
- `err bad_state`: the bus isn't free

**Runs:** `read`; the bus free, as `pingnode`

### nodestat

```
nodestat <id>
```

A node's state: `type`, `en` (energised), `datum`, `limit` where the node has
a switch, `homing` (a leg runs), then its type's tail. A stepper's is `pos`,
`slot` (`none` or the slot), `span` after a span leg, the hall index fields on
a rotary node (`idxcause`, `index`, `hall`, `base`, `cross`, `steprev`). A
vacuum node's is `servos <hex> ssr <n>`, a knife node's `osc <n> blower <n>`;
an unknown type's is `tail` and its raw bytes.

**Replies:**

- `node <id> type <t> en <0|1> datum <0|1> …`
- `node <id> <result>`
- `err usage`
- `err bad_state`: the bus isn't free

**Runs:** `read`; the bus free, as `pingnode`

### nodepos

```
nodepos <id>
```

A node's own step counter, to check against the controller's `get pos`.

**Replies:**

- `node <id> pos <steps>`
- `node <id> bad_reply`: not a stepper
- `node <id> <result>`
- `err usage`
- `err bad_state`: not at rest

**Runs:** `read`; at rest

### vac_switch

```
vac_switch <id>
```

Reads a vacuum node's switch.

**Replies:**

- `node <id> switch open|closed (level=<0|1>)`
- `node <id> <result>`
- `err usage`
- `err bad_state`: not at rest

**Runs:** `read`; at rest

---

## Lifecycle

### stop

```
stop
```

E-stop: latches, voids every origin, fences every bound slot. The controller
flushes motion and makes the bus safe; ALARM shows once it has.

**Replies:** `ok`
**Runs:** `any`

### unstop

```
unstop
```

Makes safe every touched node that isn't excluded, refused until all confirm.
Then forgets the maps and clears the latch: IDLE (or ALARM on a held fault),
unmapped, de-energised, nothing homed.

**Replies:**

- `ok`
- `err unconfirmed <id> …`: these nodes didn't confirm; nothing cleared
- `err bad_state`: no e-stop latched, or Core 1's sweep not done

**Runs:** `clear`; an e-stop latched and swept

### unalarm

```
unalarm
```

Clears the event faults, then fixes what it can, in order: a mute node, an
unmet map (retried once), a held limit latch (starts the recovery sequence).

**Replies:**

- `ok faults=<hex>`: the faults left
- `err bad_state`: no fault set
- `err estop`: use `unstop`
- `err degraded`: a node is still mute
- `err unmapped`: the retry left the map unmet
- `err node <id> not_homeable|mute|excluded|<result>`: the recovery
  sequence's refusal

**Runs:** `clear`, `needs config`; a fault set

### reset, rst

```
reset
rst
```

Soft reset of the controller.

**Replies:** `ok`, `err bad_state` (a context open)
**Runs:** `write`; no context open

### uncfg

```
uncfg
```

Built only with `PICO_ALLOW_UNCONFIGURED`. Clears CONFIG and runs
unconfigured; the stored file is ignored until power-off.

**Replies:** `ok`, `err bad_state` (no CONFIG fault)
**Runs:** `write`; the CONFIG fault set

### seqreset

```
seqreset
```

Zeroes the data plane's duplicate guard and forgets an open curve contour. The
host sends it before each stream.

**Replies:** `seq reset`
**Runs:** `any`

---

## Bus and nodes

### bus_exclude

```
bus_exclude <id> …
```

Runs without the named mute nodes. Commands to them then answer `excluded`
until the next reset. With no mute node left unexcluded, the BUS fault clears.

**Replies:**

- `ok`
- `err not_mute`: a named node isn't mute
- `err bad_node`
- `err usage`

**Runs:** `any`

### bus_enable

```
bus_enable on|off
```

Broadcasts enable or disable to every node, unacknowledged. `off` voids every
origin.

**Replies:** `ok`, `err usage`
**Runs:** `write`

### axes_enable

```
axes_enable on|off
```

Enables or disables the bound axes' nodes. `off` voids their origins.

**Replies:** `ok`, `err unbound`, `err usage`
**Runs:** `write`

### enable

```
enable <id>
```

Relays enable to one node. `ok` whatever the node answers; `get enabled` shows
the result.

**Replies:** `ok`, `err bad_node`
**Runs:** `write`

### disable

```
disable <id>
```

Relays disable to one node and voids its origin.

**Replies:** `ok`, `err bad_node`
**Runs:** `write`

### makesafe

```
makesafe <id>
```

The node de-energises, clears its datum and drops its slot. Confirmed, the
controller drops it from the slot and axes maps; unconfirmed, its slot stays
fenced.

**Replies:**

- `node <id> en <0|1> datum <0|1> slot -`: confirmed
- `node <id> <result>`: unconfirmed
- `err usage`

**Runs:** `write`

### slot_map

```
slot_map
slot_map <n0> <n1> <n2> <n3>
```

Binds any nodes to stream slots 0–3, no config needed; `-` or `0` for none. A
node that doesn't engage leaves the map unmet (UNMAPPED). Bare reads the
binding.

**Replies:**

- `slot_map <t> <t> <t> <t>`: bare; `-` empty, `!n` fenced on node n
- `ok`
- `err fenced <t> <t> <t> <t>`: a slot is fenced after an e-stop; `unstop`
  first
- `err node <id> <result>`: a node didn't engage
- `err degraded`, `err dup`, `err bad_node`, `err usage`

**Runs:** `write`, `bare read`

### axes_map

```
axes_map
axes_map <x> <y> <z> <a>
```

The axis map: each id must be a stepper axis node the config marks present,
then applied as a `slot_map`. Inside a probe session it ends the session. Bare
reads it.

**Replies:**

- `axes_map <t> <t> <t> <t>`: bare; `-` none, `?n` pending
- `ok`
- `ok switch=<0|1|?>`: ending a probe session, as `probe_end`
- `err node <id> not_stepper`: the node answers as another type
- `err node <id> not_in_config`, `err unconfigured`
- as `slot_map`

**Runs:** `write`, `bare read`, `probe`; a config

---

## Peripherals

A peripheral's reply is the node's: `node <id> <result>`. Switching one on is
refused (`err bad_state`) while any fault is set; switching off always runs.
`on|off` also takes `1` and `0`; anything else is off.

### vac_servo

```
vac_servo <id> <0-6> on|off
```

A vacuum node's servo: 0 all, 1–6 one.

**Replies:** `node <id> <result>`, `err usage`, `err bad_state`
**Runs:** `write`; on refused under a fault

### vac_pump

```
vac_pump <id> on|off
```

A vacuum node's pump, soft-started.

**Replies:** as `vac_servo`
**Runs:** `write`; as `vac_servo`

### knife_osc

```
knife_osc <id> on|off
```

A knife node's oscillator.

**Replies:** as `vac_servo`
**Runs:** `write`; as `vac_servo`

### knife_blower

```
knife_blower <id> <0-100>
```

A knife node's blower duty, percent. A missing duty is 0, off.

**Replies:** as `vac_servo`
**Runs:** `write`; above 0 refused under a fault

### laser

```
laser <id> on|off
```

A stepper node's laser; a node built without one answers `nak unsupported`.

**Replies:** as `vac_servo`
**Runs:** `write`; as `vac_servo`

---

## Motion primitives

For bring-up: none checks homing.

### step

```
step <id> <count> [sps]
```

Streams `count` steps (sign is direction) to a node holding a slot, at `sps`
(default 1000, capped at 60000). The origin survives.

**Replies:**

- `ok <count> steps <sps> sps`
- `err too_many`: `count` beyond the debug limit
- `err not_engaged`, `err not_enabled`, `err usage`

**Runs:** `write`

### line

```
line <x> <y> <feed>
```

Queues a planner line to work mm (x, y) at `feed` mm/s, run as a jog. Lines
sent back to back join at their junction speed.

**Replies:**

- `ok <depth>`: the blocks queued
- `err soft_limit x|y`: the end point leaves the soft range
- `err soft_limit z`: the bed mesh would take Z out of its range
- `err no_head`, `err unconfigured`, `err no_limits`, `err full`,
  `err bad_state`, `err usage`

**Runs:** `step`

### bez

```
bez <p1x> <p1y> <p2x> <p2y> <p3x> <p3y> <feed>
```

Queues a cubic Bézier from where the last move ends, through handles p1 and p2
to p3, work mm. Each point is checked against the soft range.

**Replies:** as `line`, and `err bad_curve`
**Runs:** `step`

### hallscan

```
hallscan <id> <stepPer> <samples> [sps]
```

Steps a rotary node `stepPer` at a time and reads its hall sensor after each,
`samples` times (at most 300). One line.

**Replies:**

- `hallscan node <id> step <n> n <n> from <pos> vals <v> … end <pos>`; the
  line ends ` err bus` if the node stops answering, ` err stalled` if its
  counter stops short
- `err no_index`: not a rotary node, or not answering
- `err too_many`: more than 300 samples
- `err not_engaged`, `err not_enabled`, `err usage`

**Runs:** `write`

---

## Homing

The homing activity is described in [machine/homing.md](../machine/homing.md).

### home

```
home
home <id> …
home only <id> …
home unhomed
home cycle <k>
home head <n>
home xy
```

Homes from the config's homing cycles. `ok` once it starts, or at once with
nothing to home; progress in `homing=` and `homecycle=`, failure as the HOMING
fault.

| Form | Homes |
| --- | --- |
| `home` | every homeable node |
| `home <id> …` | the named nodes, after clearing the cycles before theirs |
| `home only <id> …` | the named nodes alone |
| `home unhomed` | every homeable node with no origin |
| `home cycle <k>` | cycle k's nodes; `err usage` for an empty cycle |
| `home head <n>` | `home only` of head n's Z and A; `err usage` if neither is homeable |
| `home xy` | X and Y, after clearing the cycles before theirs; `err usage` if neither is homeable |

**Replies:**

- `ok`
- `err node <id> not_homeable`: the node's axis has no homing
- `err node <id> not_in_config|mute|excluded|<result>`
- `err busy`: more nodes than legs can run at once
- `err usage`

**Runs:** `sequence`, `needs config`

### leg

```
leg seek    <id> <dir> <start_us> <floor_us> <ramp> <max>
leg retract <id> <dir> <start_us> <floor_us> <ramp> <max>
leg sweep   <id> <dir> <start_us> <floor_us> <ramp> <max>
leg park    <id> <target> <start_us> <floor_us> <ramp>
leg span    <id> <dir> <start_us> <floor_us> <ramp> <max>
leg unlatch <id> <dir> <start_us> <floor_us> <ramp> <max>
leg probe   <id> <dir> <start_us> <ceil_us> <ramp> <poll_div> <max> <deadline_us> <intent>
```

One raw leg on one node, its numbers in the node's own steps and
microseconds.

| Form | Runs in | Does |
| --- | --- | --- |
| `seek` | IDLE, HOMING | Runs toward the node's switch. The node checks that it starts off the switch (`nak intent_mismatch`). |
| `retract` | IDLE, HOMING, ALARM with only a limit latch | Runs off the node's switch. The node checks that it starts on it. |
| `sweep` | IDLE, HOMING | The rotary leg: turns until the node finds its index. |
| `park` | IDLE, HOMING | Runs to `target` on the node's own counter; needs the node's origin. |
| `span` | IDLE | Seeks a switch within the datum; the travel is read from `nodestat`'s `span`. The node must be a bound axis. |
| `unlatch` | ALARM with only a limit latch | Retracts a latched node within its datum. |
| `probe` | PROBING | One probe leg on the session's Z. `<intent>` 1: the host expects the switch open (the tool on the surface), 0 closed. |

**Replies:**

- `ok`
- `err bad_state`: the form's state above isn't met
- `err node <id> busy`: a leg already runs on that node
- `err busy`: every leg slot in use
- `err kind_mismatch node <id> is <k> want <k>`: the node's homing kind
  (0 none, 1 limit, 2 index) doesn't suit the form
- `err node <id> no_start limit <0|1>`: the node accepted but isn't pulsing
- `err node <id> <result>`, `err bad_reply`: the node's status or arm
  failed
- `err not_enabled`, `err range`, `err usage`
- `park`: `err not_homed`, `err soft_limit` (the target leaves the soft
  range)
- `span`: `err unbound`
- `unlatch`: `err not_latched`
- `probe`:
  - `err not_probing`: no probe session
  - `err node <id> not_probe_z`: not the session's Z
  - `err pos_mismatch`: Z's counter moved between legs; the session ends
    with the PROBE fault
  - `err already_open`: `<intent>` 0 but the switch is open; the session
    ends with the PROBE fault
  - `err intent switch=<0|1>`: `<intent>` 1 but the switch is closed
  - `err node no_ack`, `err node no_switch`: Z's status or the switch read
    failed

**Runs:** `step`, `homing`, `probe`, then the form's state above

### leg_abort

```
leg_abort <id>
```

Stops a node's leg. An idle node just acknowledges.

**Replies:** `ok`, `err node <id> <result>`, `err bad_state`, `err usage`
**Runs:** `any`; not in a job or while a jog runs

### dummy_leg

```
dummy_leg 0|1 [ms]
```

A test leg with no node and no motion: after `ms` (default 1000, at most
600000) `1` succeeds, `0` fails as the HOMING fault with `homefail=7`.

**Replies:** `ok`, `err bad_state`, `err busy`, `err range`, `err usage`
**Runs:** `step`, `homing`; IDLE, HOMING, or ALARM with only a limit latch

### setorigin

```
setorigin <id>:<steps> …
```

Sets each node's datum: it stands at machine position `<steps>`. Every pair is
tried; the error names the first that failed, the others keeping their datum.
In a homing session, `ok` also ends the session.

**Replies:**

- `ok`
- `err estop`: an e-stop landed while it ran
- `err node <id> not_stepper`: the node has no step counter
- `err node <id> no_datum`: the node holds no datum
- `err node <id> not_in_config|<result>`
- `err usage`

**Runs:** `write`, `homing`

### home_end

```
home_end
```

Ends a homing session without a datum: IDLE, or ALARM with LATCHED if a switch
is held.

**Replies:** `ok`, `err busy`, `err bad_state` (no session between legs)
**Runs:** `context`, `homing`; a homing session between legs

---

## Probe

### probe_map

```
probe_map <id>
```

Opens a probe session: the bound Z with the switch on vacuum node `<id>`. Legs
are then `leg probe`.

**Replies:**

- `ok probing z=<id> vac=<id> switch=<0|1>`
- `err busy`: a session is open
- `err bad_state`: not IDLE
- `err no_z`: no Z bound
- `err vac_mapped`: the vacuum node is in the axis map
- `err not_enabled`: Z isn't energised
- `err node <id> no_ack`: Z or the vacuum node didn't answer
- `err node <id> not_stepper`, `err node <id> not_vacuum`: the wrong node
  type
- `err node <id> engage`: a node didn't engage; the map is put back
- `err node <id> no_switch`: the switch read failed; the map is put back
- `err degraded`, `err bad_node`

**Runs:** `context`; IDLE, no session open

### probe_end

```
probe_end
```

Ends the session and puts the map back.

**Replies:**

- `ok switch=<0|1|?>`: `1` means the tool may be resting on the switch; `?`
  the read failed
- `err not_probing`
- `err busy`: a leg runs

**Runs:** `context`, `probe`

### setprobe

```
setprobe <z_steps>
```

Records the contact height, machine steps, for the bound Z's node.

**Replies:**

- `ok probe node=<id> z=<steps>`
- `err bad_state`: a fault set
- `err unbound`, `err not_homed`, `err usage`

**Runs:** `write`; no fault set

### unprobe

```
unprobe
unprobe <id>
```

Forgets a probe height: the bound Z's, or the named node's.

**Replies:** `ok unprobe node=<id>`, `err unbound`, `err bad_node`
**Runs:** `any`

---

## Positioning

The positioning activity is described in
[machine/positioning.md](../machine/positioning.md).

### select

```
select <head>
select anchor
```

Picks the controlled point. A head maps its Z and A and turns the laser off;
`anchor` is the laser (maps X and Y if needed, releases Z and A; in a job it
keeps them), or without a laser the head at (0, 0). Never moves.

**Replies:**

- `ok`
- `err usage`: no such head
- `err no_head`: `anchor` without a laser and no head at (0, 0)
- `err unmapped`: the laser couldn't be selected
- `err degraded`
- as `axes_map`

**Runs:** `write`, `tool wait`, `needs config`

### wzero

```
wzero [x] [y] [z|z_switch]
```

The work offset becomes the selected tip's position on the named axes; bare is
X, Y, and Z when a head's Z is bound. `z_switch` zeroes with the tip on the
head's switch.

**Replies:**

- `ok`
- `err bad_state`: X or Y in a job
- `err no_switch_height`: `z_switch` on a head with no switch height
- `err not_homed x|y|z`
- `err no_head`, `err usage`

**Runs:** `write`, `tool wait`, `needs config`; X and Y not in a job

### wset

```
wset <axis> <v> …
```

Sets the work offset in machine units, per axis (x, y, z).

**Replies:**

- `ok`
- `err bad_state`: X or Y in a job
- `err no_head`: Z with no head's Z bound
- `err usage`

**Runs:** `write`, `needs config`; X and Y not in a job

### wclear

```
wclear
```

The work offset's XY back to the work area's min corner (no work area: the
soft ranges' low ends); every head's work Z unset.

**Replies:** `ok`, `err bad_state` (in a job), `err usage`
**Runs:** `write`, `needs config`; not in a job

### jog

```
jog x|y|z|a <dist> [scale]
jog xy <dx> <dy> [scale]
```

Moves one axis by `dist` mm (A: degrees), or X and Y together, from where the
queued jogs end. Speed is the axis's `jogFeed` × `scale` (`jogFeedUnhomed`
before homing, with `jogUnhomed`), capped at `maxFeed`; `xy` takes the slower
of the two. At most 4 queued.

**Replies:**

- `ok <depth>`
- `err busy`: 4 already queued
- `err bad_state`: while placing, anything but X or Y, or after `place set`
- `err not_homed`: unhomed, and the config doesn't allow unhomed jogs
- `err soft_limit <axis> <mm>`
- `err too_far`, `err no_feed`, `err no_head`, `err unconfigured`,
  `err usage`
- `line`'s queue refusals

**Runs:** `step`, `tool wait`, `place`; while placing, only X and Y and only
before `place set`

### goto

```
goto m xy <x> <y> [scale]
goto w xy <x> <y> [scale]
goto m z|a <pos> [scale]
goto w z <pos> [scale]
goto w a <deg> [scale]
goto park [scale]
goto load [scale]
goto mount [scale]
goto probe [scale]
goto up
goto unlock
```

Moves to a position, as a sequence. Speed is `jogFeed` × `scale` (default 1),
capped at `maxFeed`. `ok` once it starts, or at once with nothing to move. A
failed park is the HOMING fault; a line refused after the park is the SEQUENCE
fault.

| Form | Does |
| --- | --- |
| `m xy` | Parks every head's Z, then a straight line to the anchor's machine mm. X, Y and every Z must be homed. `<x>` or `<y>` may be `min` or `max`: that end of the soft range, the same point in `m` and `w`. |
| `w xy` | As `m xy`, to work mm for the selected tip. |
| `m z\|a` | The selected head's Z or A to machine mm or degrees, no park. |
| `w z` | Z to work mm, with the bed mesh at the tip; needs `wzero z`. |
| `w a` | A to an edge heading, on the turn nearest where it is, within the mounted tool's `aMaxDeg` of machine 0. |
| `park`, `load` | As `m xy`, to the stored position; `park` falls back to where homing parks X and Y. |
| `mount` | As `park`, to the stored mount position, turning the selected head's A to unlock along the line. |
| `probe` | Puts the selected head's tool on that head's probe switch, turning A to unlock along the line. |
| `up` | Parks every head's Z. |
| `unlock` | Turns every head's A to machine 0. |

**Replies:**

- `ok`
- every form: `err node <id> excluded|mute|<result>`, `err usage`
- all but `unlock`: `err not_homed`
- `m a`, `w a`, `unlock`, and `mount` and `probe` turning A:
  `err a_not_homed`
- all but `up` and `unlock`: `err no_feed`, `err soft_limit <axis> <mm>`,
  `err bad_state`
- the XY forms (`m xy`, `w xy`, `park`, `load`, `mount`, `probe`):
  `line`'s queue refusals
- `m xy`, `w xy` with `min` or `max`: `err no_range` (no soft range to take
  it from)
- `w xy`, `m z|a`, `w z`, `w a`, `probe`: `err no_head`
- `w z`: `err no_work` (Z not zeroed)
- `w a`: `err too_far`
- `load`, `mount`: `err no_position` (none stored); `probe`:
  `err no_position` (the head has no switch), `err no_tool`

**Runs:** `sequence`, `needs config`; `mount` and `probe` also `tool wait`

### mesh

```
mesh on|off
```

Follows the bed mesh or not, from the next move started from rest, until
reboot or a config edit.

**Replies:** `ok`, `err usage`
**Runs:** `write`, `needs config`

---

## Tools

### mount

```
mount <head> <tool id>
```

The head takes the tool. On a head whose A has an `unlockRange`
(`heads[].a.unlockRange`, ignored while that A node is not `present`), its Z
parks and its A turns past the range, locking the holder. Clears the head's
work Z.

**Replies:**

- `ok`
- `err mounted`: the head has a tool; `unmount` first
- `err no_head`: no such head
- `err no_tool`: no such tool
- `err not_homed`, `err a_not_homed`, `err node <id> excluded|mute|<result>`:
  the park or turn couldn't start
- `err usage`

**Runs:** `sequence`, `tool wait`, `needs config`

### unmount

```
unmount <head>
```

The head has no tool; its Z parks and, with `unlockRange`, its A turns to
machine 0, unlocking the holder.

**Replies:** as `mount`, without `mounted` and `no_tool`
**Runs:** `sequence`, `tool wait`, `needs config`

---

## Jobs

### pause

```
pause
```

The job brakes to a hold and lands paused, dropping its queue. A layer header
resumes it.

**Replies:** `ok`, `err bad_state`
**Runs:** `context`; a job buffering, running or dwelling, with a layer opened

### cancel

```
cancel
```

Ends the job: its queue dropped, a powered tool switched off, IDLE.

**Replies:** `ok`, `err bad_state`
**Runs:** `context`; a job at rest: paused, buffering, in preflight, being
placed and not moving, or waiting for a tool with nothing queued

### feed

```
feed <cut> <travel>
```

Overrides the config's cut and travel feeds for streamed curves, mm/s, each at
least 1, until reboot. A layer's or tool's feed overrides the cut.

**Replies:** `ok`, `err usage`
**Runs:** `any`

### place

```
place set
place reset
place go xmin|xmax ymin|ymax
place frame
place ok
```

Places a job on the bed with the laser dot, before it buffers.

| Form | Runs | Does |
| --- | --- | --- |
| `set` | before `place set` | The dot becomes the job's (xmin, ymin); the work zero follows, and the dot stops jogging. |
| `reset` | after `place set` | Clears it: the dot back to the first corner, jogging again. |
| `go` | after `place set` | The dot to that corner of the job. |
| `frame` | after `place set` | Traces the job's box once, back to (xmin, ymin). |
| `ok` | after `place set` | Signs off: laser off, the job buffers. |

**Replies:**

- `ok`
- `err bad_state`: no job being placed, or the wrong side of `place set`
- `err busy`: the placement hasn't started, or the dot is moving
- `set`: `err no_head`
- `reset`, `go`, `frame`: `err no_feed`
- `err usage`

**Runs:** `context`, `needs config`; a job being placed with nothing moving,
then the form's row above

---

## Config

### set

```
set config.<path> <value>
```

Edits one field of the active config in RAM, at once. `<path>` is the
config's own, exact case; `<value>` a number, `true`/`false`, or `<x> <y>`.

**Replies:**

- `ok`
- `err bad_state`: a context open, or an e-stop latched
- `err no_field`: no such field on this machine
- `err bad_value`: not a value of the field's type
- `err range`: outside the field's limits
- `err precision`: more digits than a float32 keeps
- `err absent`: a probe switch height on a head with no probe switch
- `err rule <error>`: a config rule refused it
- `err usage`

**Runs:** `write`, `needs config`; no context open, no e-stop latched

### save

```
save
```

Stores the active config's editable fields in `/config.bin`.

**Replies:**

- `ok`
- `err flash`: the store couldn't be read or written
- `err decode`: the stored file doesn't decode
- `err too_big`: the encoded config doesn't fit
- `err verify`: the encoded config doesn't read back the same
- `err bad_state`: as `set`, or the store refused the commit
- `err usage`

**Runs:** `write`, `needs config`; as `set`
