# Commands

Every text command the controller answers: every row of its command tables
(`kCommands` and `kControllerCommands` in
`src/rp2350/core0/control_plane.cpp`). A command whose second word picks a
form has a section of its own after its group's table.

**Checked against `ecd2b3c`.** To see what may have drifted since:

```bash
git diff ecd2b3c -- src/rp2350/core0/control_plane.cpp src/rp2350/core0/cmd src/rp2350/core0/controller/cmd
```

Strings are written as the [notation](../README.md#notation) says. The status
keys `get` reads are listed in [wire_protocol.md](../wire_protocol.md#get-keys).

## Lines and replies

One command per line, `\n`-terminated. The first word picks the row; the rest
are its arguments, separated by spaces. Every command answers exactly one line:

| Reply | Means |
| --- | --- |
| `ok`, or `ok <detail>` | done, or started for a command that takes time |
| `err <reason>` | refused; nothing changed unless the row says so |
| `node <id> <result>` | a command relayed to one node, with the node's answer |
| anything else | the command's own read, as its row says |

A line whose first word is in neither table answers `err unknown`.

### Node results

`<result>` in a `node <id> <result>` reply:

| Result | Means |
| --- | --- |
| `ok` | the node acknowledged |
| `timeout` | no answer in time |
| `bad_reply` | an answer that doesn't parse |
| `pending` | the exchange hasn't finished |
| `excluded` | the node is excluded (`bus_exclude`); nothing was sent |
| `nak <reason>` | the node refused: `unsupported`, `bad_token`, `bad_arg`, `intent_mismatch`, `busy`, `no_datum` |

## When a command runs

Each row has a kind. The kind is checked before the handler runs; a refused
command answers `err busy` when something running is the reason, else
`err bad_state`. Contexts, sequences and steps are described in
[system/controller.md](../system/controller.md#admission).

| Kind | Admitted when |
| --- | --- |
| `any` | always |
| `read` | always |
| `write` | nothing running, any e-stop swept, and no context open or a job paused |
| `step` | nothing running, no e-stop latched, no job |
| `sequence` | nothing running, no e-stop latched, no context open, and no fault but a held limit latch |
| `context` | no e-stop latched |
| `clear` | nothing running and no context open |

"Running" is a leg, a jog or an unpaused job's motion, or a sequence. The
contexts are a job, a probe session and a homing session; inside a probe or
homing session only `any`, `read` and the rows flagged for it are admitted.

| Flag | Adds |
| --- | --- |
| `homing` | also inside a homing session |
| `probe` | also inside a probe session |
| `tool wait` | also while a job waits for a tool, with nothing running and no e-stop latched |
| `place` | also while a job is being placed, as for `tool wait` |
| `bare read` | the form with no arguments is a read, admitted always |
| `needs config` | `err unconfigured` without a valid config, before the kind |

The Runs column names the kind and flags, then any narrower check the handler
makes after them.

## Reads

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `ping` | `ping` | `pong` | `read` | Is the controller answering. |
| `?` | `?` | as `get status` | `read` | Shorthand for `get status`. |
| `get` | `get`, `get <key> …` | `<key>=<value> …`, `keys <key> …`, `err too_many_keys` | `read` | Reads keys from one snapshot, in the order asked; a group name stands for its keys. Bare lists the keys this build knows. At most 32 keys after expansion. |
| `cfg` | `cfg` | `cfg fs=unmounted`, `cfg none`, `cfg seq=<n> len=<n> crc=<hex> schema=<n>` or `decoded=0`, then any of `rejected=<error>`, `ignored=1`, `dirty=1` | `read` | The stored config file, whether it decoded, and the last rejection. Answers with or without a config. |
| `pingnode` | `pingnode`, `pingnode all`, `pingnode <id>` | `nodes <id>=<result> …`, `node <id> <result>`, `err bad_node` | `read`; the bus free: at rest, or a homing session between legs | Pings one node, or every bus address in one line. |
| `busstat` | `busstat <id>` | `node <id> ferr <n> ovf <n> crc <n>`, `node <id> <result>`, `err usage` | `read`; the bus free, as `pingnode` | A node's receive-error counters: framing errors, overflows, CRC failures. Wrapping 16-bit counts since the node powered on. |
| `nodestat` | `nodestat <id>` | `node <id> type <t> en <0\|1> datum <0\|1> …`, `node <id> <result>`, `err usage` | `read`; the bus free, as `pingnode` | A node's state, with its type's tail: a stepper's `pos`, `slot`, `span`, index and hall fields; a vacuum node's `servos` and `ssr`; a knife node's `osc` and `blower`; `limit` only where the node has a switch. |
| `nodepos` | `nodepos <id>` | `node <id> pos <steps>`, `node <id> bad_reply` (not a stepper), `node <id> <result>`, `err usage` | `read`; at rest | A node's own step counter, to check against the controller's `get pos`. |
| `vac_switch` | `vac_switch <id>` | `node <id> switch open\|closed (level=<0\|1>)`, `node <id> <result>`, `err usage` | `read`; at rest | Reads a vacuum node's switch. |

## Lifecycle

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `stop` | `stop` | `ok` | `any` | E-stop: latches, voids every origin, fences every bound slot. The controller flushes motion and makes the bus safe; ALARM shows once it has. |
| `unstop` | `unstop` | `ok`, `err unconfirmed <id> …` | `clear`; an e-stop latched and swept | Makes safe every touched node that isn't excluded, refused until all confirm. Then forgets the maps and clears the latch: IDLE (or ALARM on a held fault), unmapped, de-energised, nothing homed. |
| `unalarm` | `unalarm` | `ok faults=<hex>`, `err estop`, `err degraded`, `err unmapped`, a recovery refusal | `clear`, `needs config`; a fault set | Clears the event faults, then fixes what it can in order: a mute node (`err degraded`: needs `bus_exclude`), an unmet map (retried once), a held limit latch (starts the recovery sequence). `ok` carries the faults left. With the e-stop latched: `err estop`, use `unstop`. |
| `reset`, `rst` | `reset`, `rst` | `ok` | `write`; no context open | Soft reset of the controller. |
| `uncfg` | `uncfg` | `ok` | `write`; the CONFIG fault set | Built only with `PICO_ALLOW_UNCONFIGURED`. Clears CONFIG and runs unconfigured; the stored file is ignored until power-off. |
| `seqreset` | `seqreset` | `seq reset` | `any` | Zeroes the data plane's duplicate guard and forgets an open curve contour. The host sends it before each stream. |

## Bus and nodes

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `bus_exclude` | `bus_exclude <id> …` | `ok`, `err not_mute`, `err bad_node`, `err usage` | `any` | Runs without the named mute nodes. Commands to them then answer `excluded` until the next reset. With no mute node left unexcluded, the BUS fault clears. |
| `bus_enable` | `bus_enable on\|off` | `ok`, `err usage` | `write` | Broadcasts enable or disable to every node, unacknowledged. `off` voids every origin. |
| `axes_enable` | `axes_enable on\|off` | `ok`, `err unbound`, `err usage` | `write` | Enables or disables the bound axes' nodes. `off` voids their origins. |
| `enable` | `enable <id>` | `ok`, `err bad_node` | `write` | Relays enable to one node. `ok` whatever the node answers; `get enabled` shows the result. |
| `disable` | `disable <id>` | `ok`, `err bad_node` | `write` | Relays disable to one node and voids its origin. |
| `makesafe` | `makesafe <id>` | `node <id> en <0\|1> datum <0\|1> slot -`, `node <id> <result>`, `err usage` | `write` | The node de-energises, clears its datum and drops its slot. Confirmed, the controller drops it from the slot and axes maps; unconfirmed, its slot stays fenced. |
| `slot_map` | `slot_map`, `slot_map <n0> <n1> <n2> <n3>` | `slot_map <t> <t> <t> <t>`, `ok`, `err degraded`, `err dup`, `err bad_node`, `err usage`, `err node <id> <result>`, `err fenced <t> <t> <t> <t>` | `write`, `bare read`; not with the BUS fault | Binds any nodes to stream slots 0–3, no config needed; `-` or `0` for none. Bare reads the binding: `-` empty, `!n` fenced on node n. A node that doesn't engage leaves the map unmet (UNMAPPED). |
| `axes_map` | `axes_map`, `axes_map <x> <y> <z> <a>` | `axes_map <t> <t> <t> <t>`, `ok`, `ok switch=<0\|1\|?>` (ending a probe), `err unconfigured`, `err node <id> not_in_config`, and as `slot_map` | `write`, `bare read`, `probe`; not with the BUS fault; a config | The axis map: each id must be a stepper axis node the config marks present, then applied as a `slot_map`. Bare reads it: `-` none, `?n` pending. Inside a probe session it ends the session. |

## Peripherals

A peripheral's reply is the node's: `node <id> <result>`. Switching one on is
refused (`err bad_state`) while any fault is set; switching off always runs.

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `vac_servo` | `vac_servo <id> <0-6> on\|off` | `node <id> <result>`, `err usage`, `err bad_state` | `write`; on refused under a fault | A vacuum node's servo: 0 all, 1–6 one. |
| `vac_pump` | `vac_pump <id> on\|off` | as `vac_servo` | `write`; as `vac_servo` | A vacuum node's pump, soft-started. |
| `knife_osc` | `knife_osc <id> on\|off` | as `vac_servo` | `write`; as `vac_servo` | A knife node's oscillator. |
| `knife_blower` | `knife_blower <id> <0-100>` | as `vac_servo` | `write`; above 0 refused under a fault | A knife node's blower duty, percent. A missing duty is 0, off. |
| `laser` | `laser <id> on\|off` | as `vac_servo` | `write`; as `vac_servo` | A stepper node's laser; a node built without one answers `nak unsupported`. |

`on|off` also takes `1` and `0`; anything else is off.

## Motion primitives

For bring-up: none checks homing.

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `step` | `step <id> <count> [sps]` | `ok <count> steps <sps> sps`, `err not_engaged`, `err not_enabled`, `err too_many`, `err usage` | `write` | Streams `count` steps (sign is direction) to a node holding a slot, at `sps` (default 1000, at most 60000). The origin survives. |
| `line` | `line <x> <y> <feed>` | `ok <depth>`, `err usage`, `err unconfigured`, `err no_head`, `err no_limits`, `err full`, `err soft_limit x\|y\|z`, `err bad_state` | `step` | Queues a planner line to work mm (x, y) at `feed` mm/s, run as a jog. `<depth>` is the blocks queued. |
| `bez` | `bez <p1x> <p1y> <p2x> <p2y> <p3x> <p3y> <feed>` | as `line`, and `err bad_curve` | `step` | Queues a cubic Bézier from where the last move ends, through handles p1 and p2 to p3, work mm. |
| `hallscan` | `hallscan <id> <stepPer> <samples> [sps]` | `hallscan node <id> step <n> n <n> from <pos> vals <v> … end <pos>`, `err not_engaged`, `err not_enabled`, `err no_index`, `err too_many`, `err usage`, the line ending ` err bus` or ` err stalled` | `write` | Steps a rotary node `stepPer` at a time and reads its hall sensor after each, `samples` times (at most 300). One line. |

## Homing

The homing activity is described in [machine/homing.md](../machine/homing.md).

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `leg_abort` | `leg_abort <id>` | `ok`, `err usage`, `err <why>` | `any`; not in a job or while a jog runs | Stops a node's leg. An idle node just acknowledges. |
| `dummy_leg` | `dummy_leg 0\|1 [ms]` | `ok`, `err usage`, `err range`, `err busy` | `step`, `homing`; IDLE, HOMING, or ALARM with only a limit latch | A test leg with no node and no motion: after `ms` (default 1000, at most 600000) `1` succeeds, `0` fails as the HOMING fault with `homefail=7`. |
| `setorigin` | `setorigin <id>:<steps> …` | `ok`, `err usage`, `err estop`, `err busy`, `err node <id> not_in_config\|not_stepper\|no_datum\|<result>` | `write`, `homing` | Sets each node's datum: it stands at machine position `<steps>`. Every pair is tried; the error names the first that failed. `err estop` if an e-stop lands while it runs. Between legs, `ok` also ends the homing session. |
| `home_end` | `home_end` | `ok`, `err busy`, `err bad_state` | `context`, `homing`; a homing session between legs | Ends a homing session without a datum: IDLE, or ALARM with LATCHED if a switch is held. |

### `home`

Homes from the config's homing cycles. `ok` once it starts; progress in
`homing=` and `homecycle=`, failure as the HOMING fault.

Every form: Runs `sequence`, `needs config`. Replies `ok`, `err usage`,
`err node <id> not_in_config|not_homeable|mute|excluded|<result>`, or a leg's
refusal.

| Form | Does |
| --- | --- |
| `home` | Every homeable node. |
| `home <id> …` | The named nodes, after clearing the cycles before theirs. |
| `home only <id> …` | The named nodes alone. |
| `home unhomed` | Every homeable node with no origin; `ok` with no motion if there is none. |
| `home cycle <k>` | Cycle k's nodes; `err usage` for an empty cycle. |
| `home head <n>` | `home only` of head n's Z and A; `err usage` if neither is homeable. |
| `home xy` | X and Y, after clearing the cycles before theirs; `err usage` if neither is homeable. |

### `leg`

One raw leg on one node, its numbers in the node's own steps and
microseconds.

Every form: Runs `step`, `homing`, `probe`, then the intent's state below.
Replies `ok`, `err usage`, `err range` (a number out of range, or a zero
interval or budget), `err busy`, or as its row.

| Form | Also replies | Runs in | Does |
| --- | --- | --- | --- |
| `leg seek <id> <dir> <start_us> <floor_us> <ramp> <max>` | `err not_enabled` | IDLE, HOMING | Runs toward the node's switch. The node checks that it starts off the switch (`nak intent_mismatch`). |
| `leg retract <id> <dir> <start_us> <floor_us> <ramp> <max>` | `err not_enabled` | IDLE, HOMING, ALARM with only a limit latch | Runs off the node's switch. The node checks that it starts on it. |
| `leg sweep <id> <dir> <start_us> <floor_us> <ramp> <max>` | `err not_enabled` | IDLE, HOMING | The rotary leg: turns until the node finds its index. |
| `leg park <id> <target> <start_us> <floor_us> <ramp>` | `err not_enabled`, `err not_homed`, a soft-limit refusal | IDLE, HOMING | Runs to `target` on the node's own counter; needs the node's origin. |
| `leg span <id> <dir> <start_us> <floor_us> <ramp> <max>` | `err not_enabled`, `err unbound` | IDLE | Seeks a switch within the datum; the travel is read from `nodestat`'s `span`. The node must be a bound axis. |
| `leg unlatch <id> <dir> <start_us> <floor_us> <ramp> <max>` | `err not_enabled`, `err not_latched` | ALARM with only a limit latch | Retracts a latched node within its datum. |
| `leg probe <id> <dir> <start_us> <ceil_us> <ramp> <poll_div> <max> <deadline_us> <intent>` | `err not_probing`, `err node <id> not_probe_z`, `err not_enabled`, `err pos_mismatch` | PROBING | One probe leg on the session's Z. |

## Probe

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `probe_map` | `probe_map <id>` | `ok probing z=<id> vac=<id> switch=<0\|1>`, `err busy`, `err degraded`, `err bad_state`, `err bad_node`, `err no_z`, `err vac_mapped`, `err not_enabled`, a bind refusal | `context`; IDLE, no session open, not with the BUS fault | Opens a probe session: the bound Z with the switch on vacuum node `<id>`. Legs are then `leg probe`. |
| `probe_end` | `probe_end` | `ok switch=<0\|1\|?>`, `err not_probing`, `err busy` | `context`, `probe` | Ends the session and puts the map back. `switch=1` means the tool may be resting on the switch. |
| `setprobe` | `setprobe <z_steps>` | `ok probe node=<id> z=<steps>`, `err usage`, `err unbound`, `err not_homed`, `err bad_state` | `write`; no fault set | Records the contact height, machine steps, for the bound Z's node. |
| `unprobe` | `unprobe`, `unprobe <id>` | `ok unprobe node=<id>`, `err unbound`, `err bad_node` | `any` | Forgets a probe height: the bound Z's, or the named node's. |

## Positioning

The positioning activity is described in
[machine/positioning.md](../machine/positioning.md).

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `select` | `select <head>`, `select anchor` | `ok`, `err usage`, `err degraded`, `err no_head`, `err unmapped`, and as `axes_map` | `write`, `tool wait`, `needs config`; not with the BUS fault | Picks the controlled point. A head maps its Z and A and turns the laser off; `anchor` is the laser (maps X and Y if needed, releases Z and A), or without a laser the head at (0, 0). Never moves. |
| `wzero` | `wzero [x] [y] [z\|z_switch]` | `ok`, `err usage`, `err bad_state`, `err no_head`, `err no_switch_height`, `err not_homed <axis>` | `write`, `tool wait`, `needs config`; X and Y not in a job | The work offset becomes the selected tip's position on the named axes; bare is X, Y, and Z when a head's Z is bound. `z_switch` zeroes with the tip on the head's switch. |
| `wset` | `wset <axis> <v> …` | `ok`, `err usage`, `err bad_state`, `err no_head` | `write`, `needs config`; X and Y not in a job | Sets the work offset in machine units, per axis (x, y, z). |
| `wclear` | `wclear` | `ok`, `err usage`, `err bad_state` | `write`, `needs config`; not in a job | The work offset's XY back to the config's; every head's work Z unset. |
| `jog` | `jog x\|y\|z\|a <dist> [scale]`, `jog xy <dx> <dy> [scale]` | `ok <depth>`, `err usage`, `err unconfigured`, `err no_head`, `err not_homed`, `err too_far`, `err no_feed`, `err busy`, `err bad_state`, `err soft_limit <axis> <mm>`, and `line`'s queue refusals | `step`, `tool wait`, `place`; while placing, only X and Y and only before `place set` | Moves one axis by `dist` mm (A: degrees), or X and Y together, from where the queued jogs end. Speed is the axis's `jogFeed` × `scale`, capped at `maxFeed`. At most 4 queued (`err busy`). |
| `mesh` | `mesh on\|off` | `ok`, `err usage` | `write`, `needs config` | Follows the bed mesh or not, from the next move started from rest, until reboot or a config edit. |

### `goto`

Moves to a position, as a sequence. Speed is `jogFeed` × `scale` (default 1),
capped at `maxFeed`. `ok` once it starts, or at once with nothing to move. A
failed park is the HOMING fault; a line refused after the park is the SEQUENCE
fault.

Every form: Runs `sequence`, `needs config`, and `tool wait` where its row
says. Replies `ok`, `err usage`, `err busy`, `err bad_state`,
`err node <id> excluded|mute|<result>`, or as its row.

| Form | Also replies | Runs | Does |
| --- | --- | --- | --- |
| `goto m xy <x> <y> [scale]` | `err not_homed`, `err no_feed`, `err soft_limit <axis> <mm>`, `line`'s queue refusals | | Parks every head's Z, then a straight line to the anchor's machine mm. X, Y and every Z must be homed. |
| `goto w xy <x> <y> [scale]` | as `m xy`, and `err no_head` | | As `m xy`, to work mm for the selected tip. |
| `goto m z\|a <pos> [scale]` | `err no_head`, `err not_homed`, `err no_feed`, `err soft_limit <axis> <mm>` | | The selected head's Z or A to machine mm or degrees, no park. |
| `goto w z <pos> [scale]` | as `m z`, and `err no_work` | | Z to work mm, with the bed mesh at the tip; needs `wzero z`. |
| `goto w a <deg> [scale]` | as `m a`, and `err too_far` | | A to an edge heading, on the turn nearest where it is, within the mounted tool's `aMaxDeg` of machine 0. |
| `goto park [scale]`, `goto load [scale]` | as `m xy`, and `err no_position` | | As `m xy`, to the stored position; `park` falls back to where homing parks X and Y. |
| `goto mount [scale]` | as `park`, and `err a_not_homed` | `tool wait` | As `park`, to the stored mount position, turning the selected head's A to unlock along the line. |
| `goto probe [scale]` | as `mount`, and `err no_head`, `err no_tool` | `tool wait` | Puts the selected head's tool on that head's probe switch, turning A to unlock along the line. |
| `goto up` | `err not_homed` | | Parks every head's Z. |
| `goto unlock` | `err not_homed` | | Turns every head's A to machine 0. |

## Tools

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `mount` | `mount <head> <tool id>` | `ok`, `err usage`, `err no_head`, `err no_tool`, `err mounted`, `err not_homed`, `err a_not_homed`, `err busy`, `err bad_state`, `err node <id> excluded\|mute\|<result>` | `sequence`, `tool wait`, `needs config` | The head takes the tool. On a head with `unlockRange`, its Z parks and its A turns past the range, locking the holder. Clears the head's work Z. |
| `unmount` | `unmount <head>` | as `mount`, without `no_tool` and `mounted` | `sequence`, `tool wait`, `needs config` | The head has no tool; its Z parks and, with `unlockRange`, its A turns to machine 0, unlocking the holder. |

## Jobs

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `pause` | `pause` | `ok`, `err bad_state` | `context`; a job buffering, running or dwelling, with a layer opened | The job brakes to a hold and lands paused, dropping its queue. A layer header resumes it. |
| `cancel` | `cancel` | `ok`, `err bad_state` | `context`; a job at rest: paused, buffering, in preflight, being placed and not moving, or waiting for a tool with nothing queued | Ends the job: its queue dropped, a powered tool switched off, IDLE. |
| `feed` | `feed <cut> <travel>` | `ok`, `err usage` | `any` | Overrides the config's cut and travel feeds for streamed curves, mm/s, each at least 1, until reboot. A layer's or tool's feed overrides the cut. |

### `place`

Places a job on the bed with the laser dot, before it buffers.

Every form: Runs `context`, `needs config`, and a job being placed with
nothing moving. Replies `ok`, `err usage`, `err bad_state`, `err busy`, or as
its row.

| Form | Also replies | Runs | Does |
| --- | --- | --- | --- |
| `place set` | `err no_head` | before `place set` | The dot becomes the job's (xmin, ymin); the work zero follows, and the dot stops jogging. |
| `place reset` | `err no_feed`, a move's refusal | after `place set` | Clears it: the dot back to the first corner, jogging again. |
| `place go xmin\|xmax ymin\|ymax` | as `reset` | after `place set` | The dot to that corner of the job. |
| `place frame` | as `reset` | after `place set` | Traces the job's box once, back to (xmin, ymin). |
| `place ok` | | after `place set` | Signs off: laser off, the job buffers. |

## Config

| Command | Forms | Replies | Runs | Does |
| --- | --- | --- | --- | --- |
| `set` | `set config.<path> <value>` | `ok`, `err usage`, `err bad_state`, `err no_field`, `err bad_value`, `err range`, `err precision`, `err absent`, `err rule <error>` | `write`, `needs config`; no context open, no e-stop latched | Edits one field of the active config in RAM, at once. `<path>` is the config's own, exact case; `<value>` a number, `true`/`false`, or `<x> <y>`. |
| `save` | `save` | `ok`, `err usage`, `err bad_state`, `err flash`, `err decode`, `err too_big`, `err verify` | `write`, `needs config`; as `set` | Stores the active config's editable fields in `/config.bin`. |
