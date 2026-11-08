# Positioning

Commands, replies, status keys and config fields are written as the
[notation](../README.md#notation) says.

## 1. What positioning is

Once the machine is [homed](homing.md), a number means a place on the bed.
Positioning is how you read where the tool is, say where 0 is, and move the
tool somewhere: by a distance (a jog) or to a place (a `goto`). The controller
checks every move against the machine's limits before it starts, and refuses
one that would leave them.

The numbers describe one point, the **controlled point**: the tip of the
selected head's tool, or the laser dot (§3).

## 2. Coordinates

**Directions** are fixed and physical. Looking down at the bed from the
operator's side:

| Axis | + is |
| --- | --- |
| X | right |
| Y | away from you |
| Z | up, away from the work |
| A | counter-clockwise seen from above |

**Machine coordinates** (`mpos`) are where homing puts 0. Each linear axis's 0
comes from its park position ([homing](homing.md) §4), so the numbers may be
negative; that is harmless. Machine coordinates don't change unless the homing
config does.

**Work coordinates** (`wpos`) are measured from the work zero you set (§4):

* X and Y: a point on the material, shared by every head.
* Z: per head, the height where that head's tip meets the bed. It is the bed,
  not the material's top, whatever the material.
* A: no work zero. Work A is the heading the blade's edge points at: 0 is along
  X+, read from −180 to 180.

With no work zero set, work X and Y equal machine X and Y, and work Z reads `-`.

Read the position with `get mpos wpos`:

| Key | Reads |
| --- | --- |
| `mpos` | X, Y, Z and A in machine coordinates; `-` for an axis with no position |
| `wpos` | the same in work coordinates; `-` for an axis with no work zero |

**Named positions** are machine coordinates for the gantry, the same for every
head:

| Name | Where | Unset |
| --- | --- | --- |
| `park` | `machine.positions.park` | where homing parks X and Y |
| `load` | `machine.positions.load` | `→ err no_position` |
| `mount` | `machine.positions.mount`, where a job stops for a tool change | `→ err no_position` |
| `probe` | the selected head's bed switch, `machine.heads[].probeSwitch`, under its mounted tool's tip | `→ err no_position`; `→ err no_tool` with no tool mounted |

## 3. Selecting a head

`select <head>` makes that head the one you position. It never moves anything;
it changes what the numbers mean and which axes the commands drive:

| | Follows the selection |
| --- | --- |
| X and Y in `wpos`, `goto w xy`, `wzero x y` | the selected head's tip |
| `jog z`, `jog a`, `goto z`, `goto a` | the selected head's Z and A |
| `wzero z`, `wset z`, work Z in `wpos` | the selected head's work Z |
| `goto probe` | the selected head's bed switch |

**The controlled point** is the gantry's anchor plus the selected head's
offset (`machine.heads[].xOffset`, `machine.heads[].yOffset`) plus its mounted
tool's offset. Offsets are measured tip to tip. Selecting another head makes
`wpos` jump by the difference; the gantry stays where it is.

**The laser.** `select anchor` makes the laser dot the controlled point when
the machine has a laser, and turns the laser on; no Z or A is driven while it
is selected. Selecting a head turns the laser off. Without a laser, `anchor`
selects the head with no offset.

`get head` reads the selection: a head's number, `anchor`, or `-` for none.
With none, nothing that needs a tip moves: `→ err no_head`.

`select` runs at rest, and while a job waits for a tool change. It is refused
while a node is silent (`→ err degraded`).

## 4. Zeroing

Zeroing sets the work zero at the selected tip. It is held until it is
cleared, or until the controller restarts; to keep one, put it in the config's
`machine.work` block.

**X and Y:** jog the tip, or the laser dot, onto the point that should be 0,
then `wzero x y`.

**Z**, for the selected head's mounted tool, either way:

| Touch | Then | Z 0 is |
| --- | --- | --- |
| jog the tip down onto the bed, beside the material | `wzero z` | the tip's height here |
| jog the tip down onto the head's bed switch | `wzero z_switch` | the tip's height here, less the switch's height, `machine.heads[].probeSwitch.z` |

The switch is for a bed the tip can't reach beside the material.
`goto probe` brings the tip over it.

* A bare `wzero` zeroes X, Y and, with a head selected, its Z.
* Each axis it names must be homed: `→ err not_homed <axis>`, and nothing is
  set.
* Mounting or unmounting a tool unsets that head's Z, since the new tip sits
  at a different height. Zero it again before a work-Z `goto` or a job.
* `wset` sets the work zero by number instead, in machine coordinates:
  `wset x 120 y 40`.
* `wclear` sets X and Y back to `machine.work`, or 0, and unsets every head's
  Z.
* All three run at rest; `wzero` also while a job waits for a tool change.
  While a job runs, its X and Y zero is held: `→ err bad_state`.

## 5. Moves

| Move | Moves | Z lifts first | Needs homing |
| --- | --- | --- | --- |
| `jog`, a held jog button | the named axis, or X and Y together | no | no, with `machine.jogUnhomed` (§6) |
| `goto xy`, `goto park\|load\|mount\|probe` | X and Y | yes, every Z, to its park position | X, Y and every Z |
| `goto z`, `goto a` | the selected head's Z or A | no | that axis |
| `goto up` | every Z | — | every Z |
| `goto unlock` | every A | no | every A |

A `goto` takes `m` for machine coordinates or `w` for work coordinates (§2). A
jog has no frame: its distance is the same in both. Holding a jog button in the
host moves the same way as `jog`, for as long as it is held.

An absolute XY move is a travel move, so it always lifts the Zs first. A jog is
a nudge near the work, so it never does.

A jog runs when nothing else is moving and no job is running, and also while a
job waits for a tool change or a placement. A `goto` runs from idle, with no
fault except an axis standing on its switch; `goto mount` and `goto probe` also
while a job waits for a tool change.

Jogs queue: up to four can wait behind the one moving (`→ ok <depth>`); a fifth
is `→ err busy`. A `goto` doesn't queue; it replies `→ ok` once it starts.

**Speed.** A jog or `goto` runs at `machine.<axis>.jogFeed` × `scale` (1 if
omitted), never
above `machine.<axis>.maxFeed`. An XY move runs at the slower of X's and Y's
speeds. An unhomed axis jogs at `machine.<axis>.jogFeedUnhomed` instead.

The Z lift in a `goto` runs at the Z's homing seek speed,
`machine.heads[].z.homing.seekFeed`, ramping from its start speed. An A
turning to unlock runs at its `jogFeed`.

**A turns the long way when it has to.** Machine A counts whole turns, so
`goto m a <pos>` and `goto unlock` unwind the way A was turned. A `goto w a`
picks the turn of the heading that stays within the mounted tool's
`tools.<tool>.aMaxDeg` of machine 0 (its cable), not the nearest one.

## 6. Limits

| | Linear (X, Y, Z) | Rotary (A) |
| --- | --- | --- |
| Range | from its park position, `machine.<linear>.maxTravel` toward the far end | none: A turns freely |
| Checked when | `machine.<linear>.softLimits` is on and the axis is homed | a `goto w a`, against `tools.<tool>.aMaxDeg` |
| X and Y also shrink by | the mounted blade's offset, so the blade's axis stays in range | — |
| Z also stops at | `machine.maxOvercut` below the head's work Z, once it has one | — |
| A move outside it | `→ err soft_limit <axis> <left>`: how far it could go | `→ err too_far` |

The strip between a switch and the park position is outside the range.

An axis already outside its range (a blade mounted near the edge) may stay
where it is or move back in; it can't move further out.

Turning A is refused while the tip is outside X and Y's range, since A swings
the blade's axis about the tip.

**Unhomed axes.** Without `machine.jogUnhomed`, an unhomed axis doesn't move:
`→ err not_homed`. With it, an unhomed axis jogs at its unhomed speed, with no
range check, at most `machine.<linear>.maxTravel`, or one turn for A, per jog
(`→ err too_far`). This is for setting up a machine; there is nothing to stop an
unhomed jog running into the end of an axis. A `goto` always needs homing.

## 7. Commands

| Command | Does | Section |
| --- | --- | --- |
| `select <head>`, `select anchor` | makes that head's tip, or the laser dot, the controlled point; never moves | §3 |
| `wzero [x] [y] [z\|z_switch]` | sets the work zero where the tip is now | §4 |
| `wset <x\|y\|z> <v> …` | sets the work zero by number, in machine coordinates | §4 |
| `wclear` | puts the work zero back to the config's | §4 |
| `jog <x\|y\|z\|a> <d> [scale]` | moves one axis by `<d>` | §5 |
| `jog xy <dx> <dy> [scale]` | moves X and Y together, in one straight line | §5 |
| `goto <m\|w> xy <x> <y> [scale]` | every Z lifts, then the tip goes to (x, y) | §5 |
| `goto <m\|w> z <pos> [scale]`, `goto <m\|w> a <pos> [scale]` | the selected head's Z or A goes to `<pos>` | §5 |
| `goto park\|load\|mount [scale]` | every Z lifts, then the gantry goes to a named position | §5 |
| `goto probe [scale]` | every Z lifts, then the tip goes over the selected head's bed switch | §5 |
| `goto up` | every Z lifts to its park position | §5 |
| `goto unlock` | every A turns to machine 0, where its holder lets go of the tool | §5 |
| `get mpos wpos` | the position, in machine and work coordinates | §2 |
| `get head` | the selection | §3 |

## 8. Refusals and stops

A refused move doesn't start. Everything is checked first:

| Reply | Command | Cause |
| --- | --- | --- |
| `→ err not_homed`, `→ err a_not_homed` | jog, goto | the axis isn't homed (§6) |
| `→ err soft_limit <axis> <left>` | jog, goto | the move would leave the range; `<left>` is how far it could go |
| `→ err too_far` | jog, goto | an unhomed jog longer than the axis, or a work-A heading the tool's cable can't reach |
| `→ err no_head` | jog, goto, select, wzero, wset | no head is selected, so Z, A or the tip is unknown |
| `→ err no_work` | goto | a work-Z target on a head with no work Z |
| `→ err no_position`, `→ err no_tool` | goto | the named position isn't set, or `probe` with no tool mounted |
| `→ err no_switch_height` | wzero z_switch | the head has no `machine.heads[].probeSwitch.z` |
| `→ err busy` | jog | four jogs already queued |
| `→ err bad_state` | jog, goto, wzero, wset, wclear | the machine is busy, or a job holds X and Y |
| `→ err usage` | all | a malformed command |

**Stopping a move.** The host's stop brakes a jog, or a `goto`'s XY, Z or A
move, to a halt; the machine stays homed and the work zero stays set.
Releasing a held jog button stops that jog the same way.

An emergency stop (`stop`) stops everything at once. After `unstop`, nothing is
homed: home again before positioning.

## Underneath

* Contexts, sequences, faults and when a command runs:
  [system/controller.md](../system/controller.md).
* Frames, the controlled point, the work zero, limits and named positions:
  [plans/coordinate-system.md](../plans/coordinate-system.md).
* Jog and `goto` forms, what each does, and their speeds:
  [plans/position-moves.md](../plans/position-moves.md).
* The config fields: `web/src/machine/schema.ts`.
