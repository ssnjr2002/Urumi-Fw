# Homing

Commands, replies, status keys and config fields are written as the
[notation](../README.md#notation) says.

## 1. What homing is

Homing gives each axis a known position. Each linear axis runs to its limit
switch; each rotary axis finds the index magnet on its holder. The axis's node
drives the motor and watches the sensor; the controller runs the sequence and
records where every axis is; the host starts it and shows progress.

Until an axis is homed the controller will not drive it to a position (`goto`,
park, mount). Whether it can still be nudged with a relative `jog` is set by
`machine.jogUnhomed`.

After a tool swap nothing is homed again; only the tool's height is probed.

## 2. Homing cycles

Every axis has a cycle number, `machine.<axis>.homing.cycle`. Lower cycles home
first; axes with the same number home together. By default every Z is cycle 1
and everything else cycle 2, so a plain `home` looks like this:

1. **Every Z lifts.** An unhomed Z homes against its top switch. A Z that is
   already homed doesn't touch its switch: it rises straight to its park
   position, the safe height. All Zs move at once.
2. **Then X, Y and every A, together.** X and Y home against their switches;
   each A sweeps past its index.
3. **Done.** Every linear axis rests at its park position, each A just past its
   index (§4). Nothing else moves.

Before any axis of a cycle moves, every axis of every earlier cycle is made
clear, named or not: homed ones park, unhomed ones are homed. `home only` and
`home head` skip this.

If an earlier-cycle axis can't be made clear (its node is silent, excluded, or
its leg failed), nothing in a later cycle moves.

**Choosing what to home.** `home` on its own homes every axis that has a
`homing` block. The other forms home part of the machine:

| Form | Homes | Clears earlier cycles first |
| --- | --- | --- |
| `home` | every homeable axis | yes |
| `home <node> …` | the named axes, re-homed even if already homed | yes |
| `home only <node> …` | the named axes, in cycle order | **no**: you vouch the way is clear |
| `home unhomed` | every axis that isn't homed; with none, `→ ok` and nothing moves | yes |
| `home cycle <k>` | every axis in cycle *k* | yes |
| `home head <n>` | one head's Z and A | **no**, as `only` |
| `home xy` | the gantry's X and Y | yes, so the Zs lift first |

Nodes are named by their bus id.

**Starting a run.** `home` runs only from idle with no fault set, except an
axis standing on its switch; after a failed home, clear the fault with
`unalarm` first (§7). Everything is checked before anything moves, and
`home` replies `→ ok` once the run starts:

| Reply | Why |
| --- | --- |
| `→ err node <id> not_in_config` | no axis in the config uses that node |
| `→ err node <id> not_homeable` | its axis has no `homing` block |
| `→ err node <id> mute`, `→ err node <id> excluded` | the node is silent, or excluded from the bus |
| `→ err usage` | a node named twice, or a malformed form |

**Following a run.** `get homing homecycle homefail homenode`; each key reads
`-` when it doesn't apply:

| Key | Reads |
| --- | --- |
| `homing` | set while a home runs |
| `homecycle` | the cycle running now |
| `homefail` | after a failure: why (§6) |
| `homenode` | after a failure: which node |

## 3. Homing legs

A leg is one move of one axis, ended by its sensor or by its distance. A
linear axis homes in four legs, as grbl, Marlin and FluidNC do; a rotary axis
in two.

| Phase | Linear leg | Rotary leg |
| --- | --- | --- |
| 1 | **Seek:** fast toward the switch until it triggers | **Forward sweep:** turns + until it has passed the magnet three times |
| 2 | **Back off:** a fixed distance away | **Reverse sweep:** the same, turning − |
| 3 | **Seek slowly:** back in until it triggers again; this touch is the one that counts | — |
| 4 | **Pull off:** a fixed distance away, clear of the switch | — |

Every axis in a cycle runs its phase-1 leg at the same time, and the next phase
starts when all of them have finished. A rotary axis in the same cycle as a
linear one sweeps forward during the seek and back during the back-off, then
waits.

The fast legs (the seek, both sweeps) ramp up from a start speed; the slow
legs run at one speed throughout:

| Set by | Linear | Rotary |
| --- | --- | --- |
| start speed, ramp length | `machine.<linear>.homing.startFeed`, `machine.<linear>.homing.rampSteps` | `machine.<rotary>.homing.startFeed`, `machine.<rotary>.homing.rampSteps` |
| fast speed | `machine.<linear>.homing.seekFeed` (seek) | `machine.<rotary>.homing.sweepFeed` (both sweeps) |
| slow speed | `machine.<linear>.homing.latchFeed` (back-off, slow seek, pull-off) | — |
| distances | `machine.<linear>.homing.backoffDist`, `machine.<linear>.homing.pullOffDist` | — |
| how far before giving up | (`machine.<linear>.maxTravel` + `machine.<linear>.homing.pullOffDist`) × `machine.<linear>.homing.seekScaler` | `machine.<rotary>.homing.budgetRevs` turns |

The back-off and pull-off are fixed distances, not "until the switch
releases": the release point is the noisiest part of a switch's travel.

An axis that starts **on** its switch skips the seek, backs off, and carries
on from there.

**Park position.** Where a linear axis rests after the pull-off, and where a
homed axis returns when an earlier cycle is cleared.

* With `machine.<linear>.homing.parkPos` set, the axis reads that coordinate
  there.
* Without it, the axis reads `machine.<linear>.maxTravel` there when the switch
  is at the + end, or `machine.<linear>.homing.pullOffDist` when it is at the −
  end. Either way the usable range runs from 0 to `machine.<linear>.maxTravel`.
* Z homes up with `machine.heads[].z.homing.parkPos` at 0: the top is 0,
  everything below it negative, and the park position is the safe height.

A **park leg** moves a homed axis to its park position without touching the
switch. Meeting the switch on the way, or stopping anywhere but the target,
fails it.

## 4. Types

| | Linear (X, Y, Z) | Rotary (A) |
| --- | --- | --- |
| Sensor | a limit switch at one end; `machine.<linear>.homing.seekPositive` says which | a magnet on the holder passing a Hall sensor once per turn |
| Legs | four (§3) | two sweeps (§3), each between two and three turns depending on where it started |
| Position comes from | the slow seek's touch | the two sweeps together: the magnet's centre, and the length of a turn |
| Checked | the switch triggers within the budget | the sweeps agree on the magnet within `machine.<rotary>.homing.toleranceDeg`, and on a turn's length within 2% |
| Rests at | its park position | just past the magnet on the − side, short of it by the magnet's width plus a short tail |
| Reads there | `machine.<linear>.homing.parkPos`, or as §3 says without it | a little below `machine.<rotary>.homing.indexPos`, the angle at the magnet itself |
| Park position | yes | none: a homed A in an earlier cycle is left where it is |

## 5. Commands

| Command | Does | Section |
| --- | --- | --- |
| `home` | homes every homeable axis | §2 |
| `home <node> …` | homes the named axes, clearing earlier cycles first | §2 |
| `home only <node> …` | homes the named axes, without clearing earlier cycles | §2 |
| `home unhomed` | homes every axis that isn't homed | §2 |
| `home cycle <k>` | homes every axis in cycle *k* | §2 |
| `home head <n>` | homes one head's Z and A | §2 |
| `home xy` | homes the gantry's X and Y, after the Zs lift | §2 |
| `get homing homecycle homefail homenode` | a run's progress, and why it failed | §2 |
| `unalarm` | clears a homing fault and backs an axis off its switch | §7 |
| `leg_abort <node>` | stops one axis's leg | §6 |

## 6. Leg failures

A failed leg stops its axis and fails its whole cycle. The other axes in that
cycle are told to stop at once and stay powered, holding position. The machine
raises a homing fault; `get homefail homenode` says why and on which node.

Nothing from the failed cycle counts as homed, not even an axis that finished.
Earlier cycles keep their positions, so the Zs stay homed. No later cycle runs.

| `homefail` | Axis | What you see | Cause |
| --- | --- | --- | --- |
| 1 | linear | The axis runs its full budget and stops, no switch | Switch never triggered: dead switch, broken wire, wrong direction, travel set too short |
| 8 | linear | An axis parking stops short | Met its switch on the way, or didn't reach the target |
| 4 | rotary | A holder sweeps but no index is found | Magnet missing, or the sensor not seeing it |
| 5 | rotary | A holder sweeps, the signal looks wrong | Something passed the sensor that doesn't look like the magnet |
| 6 | rotary | A holder sweeps, index positions disagree | The index repeated at inconsistent spacing: belt slip, loose magnet |
| 12 | rotary | Both sweeps ran but disagree | Beyond `machine.<rotary>.homing.toleranceDeg`, or turn lengths more than 2% apart |
| 2 | both | The axis stops; its node stopped answering | The node went silent mid-leg (power, cable, reset) |
| 3 | both | The axis keeps going past the time the controller allowed | The node is still stepping when it should have finished |
| 9 | both | The axis stops when you abort it | You ran `leg_abort <node>` on it |
| 10 | both | The axis didn't start a leg | The node refused a leg mid-run |
| 11 | both | Motion finished but the axis isn't homed | The controller couldn't record the position on that node |

## 7. Failure recovery

1. **Fix the cause**, if it's physical: switch, wire, magnet, node power.
2. **`unalarm`** clears the homing fault, then fixes what it can, in order, and
   stops at the first thing it can't:

   | Left over | Reply, or what it does |
   | --- | --- |
   | a node silent and not excluded | `→ err degraded`. Fix the node, or `bus_exclude <node>`. |
   | an incomplete axis map | retries once, else `→ err unmapped` |
   | an axis standing on its switch | backs it off by `machine.<linear>.homing.backoffDist`, slowly, then parks it if it's still homed |
   | nothing | `→ ok faults=<mask>` |

3. **Home again.** `home unhomed` homes only what the failure left unhomed.

An emergency stop is not cleared by `unalarm` (`→ err estop`); use `unstop`.

**Where this doesn't help:**

* **The cause is still there.** A dead switch, an unseen magnet or a silent
  node fails the same way again.
* **A switch stuck closed.** The axis reads as already on its switch, backs
  off, touches again at once, and homes in the wrong place. Nothing checks for
  a minimum travel.
* **An axis that didn't hear the stop.** When a cycle fails, an axis whose node
  doesn't acknowledge the stop may keep moving under the fault. Nothing stops
  it.
* **A node that resets after homing.** The controller still believes its axis
  is homed, so nothing asks you to home again.
* **No usable config.** `unalarm` can't fix that; load a new config.

## Underneath

* The homing session, cycles and the `home` command on the controller:
  [plans/controller-homing.md](../plans/controller-homing.md).
* Legs on the node, `setorigin`, the supervisor and the rotary index:
  [homing.md](../homing.md).
* The homing config fields: `web/src/machine/schema.ts`.
