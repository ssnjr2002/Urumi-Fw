# Planner

**Checked against `dbc3118`.** To see what may have changed since:

```bash
git diff dbc3118 -- lib/planner/ src/rp2350/core0/planner/ src/rp2350/core1/emit/follower.cpp src/rp2350/core0/ops/mesh.h src/rp2350/core0/ops/mesh.cpp src/rp2350/core0/cmd/axis.cpp src/rp2350/core0/ops/cjog.cpp src/rp2350/ipc/shared_state.h src/rp2350/core1/core1.cpp platformio.ini
```

Commands, replies and code names are written as the
[notation](../README.md#notation) says.

The planner turns moves into steps. A move is a line or a cubic Bézier; the
planner limits its speed, joins it to its neighbours, gives it a speed profile
in time, and runs it as positions that become stream bytes on the bus. This
page is that pipeline on the controller, from a move coming in to a byte going
out.

```text
command or record
  → admission            Core 0: may this move run now?
  → block                Core 0: the move's shape and speed caps
  → ring                 Core 0 → Core 1: blocks, with a look-ahead plan
  → executor   (1 ms)    Core 1: time → position along the path
  → offsets              Core 1: blade lead, bed mesh, steps per unit
  → followers  (20 µs)   Core 1: position → one stream byte
  → bus
```

Two things act along the whole pipeline: the bed mesh, which bends Z under
every XY move (§13), and the tool's duty limits, which stop a cut before the
tool runs too long (§14).

## 1. Hardware

* **Two cores** (RP2350, Cortex-M33 at 150 MHz). Core 0 plans; Core 1 runs
  the plan and drives the bus ([cores](cores.md)).
* **A single-precision FPU.** The planner works in `float`. Doubles go through
  slow library calls, so they appear only where a value must stay exact over
  a long run: A's turns and Q32.32 step positions.
* **RAM-resident code.** Flash is slow and stalls while Core 0 writes it.
  Everything Core 1 calls every slot runs from RAM: the follower is placed in
  `.time_critical`, and the library's tick path in `.time_critical.planner`
  through `PLANNER_RAM` (`platformio.ini`).
* **A hardware spinlock** (`plannerLock`) guards the ring between the cores.
* **The bus** is the output: one stream byte per 20 µs slot
  ([bus](bus.md) §3).

## 2. Codebase

The planner is three layers of code.

| Layer | Where | Holds | Knows about the Pico |
| --- | --- | --- | --- |
| The library | `lib/planner/` | blocks, speed caps, junctions, look-ahead, the trapezoid, the executor, the mesh lookup | no |
| Core 0's side | `core0/planner/queue.*`, `core0/ops/mesh.*` | admission, limits from the config, the ring's reset and latches, the mesh walk, the commit retry; job records | yes |
| Core 1's side | `core1/emit/follower.cpp` | the slot loop, offsets, followers, holds, events, duty | yes |

* **The library** has no Arduino, no locks and no shared state. A caller that
  splits it across cores does the locking (§9). Two macros are its only
  concessions to a platform: `PLANNER_RAM` places the tick path, and
  `PLANNER_BENCH` adds counters for the bench branches.
* **Core 0's side** also frames job records into lifts, travels, plunges and
  swivels (`plannerQueueRecord()`). That is the jobs page's subject; this page
  names only what it uses from the planner.
* **The shared objects** (the ring `plannerRing`, the executor `plannerExec`,
  the lock and the latches) are declared in `ipc/shared_state.h` and defined
  in `main.cpp`.

### Tests

* **The library is tested on the host.** The `native` env runs
  `test/test_planner/` with doctest and the host compiler; Arduino and the
  Pico SDK are stubbed in `test/stubs/`.

  | Suite | Covers |
  | --- | --- |
  | `test_trapezoid` | profiles, triangles, closed-form position |
  | `test_line` | per-axis caps, junction deviation |
  | `test_bezier` | analysis, the t(s) fit, `checkBezier()` against real records from the web pipeline |
  | `test_axes` | axis sets, Z and A lines, A's heading and turns |
  | `test_planner` | look-ahead, replan and commit, staged pieces, restart |
  | `test_executor` | ticks, holds, resume, abort |
  | `test_event` | events in the ring |
  | `test_tangent` | A following a Bézier |
  | `test_blade` | caps at the knife's axis |
  | `test_mesh` | lookup, walks, caps |

* **Core 0's and Core 1's sides have no tests.** They are checked by building
  `pico` and on the machine: the cross-core timing, the follower and the bus
  output are bench work.
* **Bench branches** measure without merging: `bench/planner` (commit timing
  and staged pieces) and `bench/bezier` (analysis cost). Their numbers are in
  `plans/pico-planner.md`.

## 3. Inputs

Every move enters through one of four functions in `core0/planner/queue.h`.
Each push names the kind of motion it is (`RingOwner`); the ring runs one kind
at a time (§9).

| Source | Function | Kind |
| --- | --- | --- |
| `jog x\|y\|xy`, `line` | `plannerQueueLine()` | `RING_JOG_STEP` |
| `jog z\|a` | `plannerQueueAxis()` | `RING_JOG_STEP` |
| `bez` | `plannerQueueBezier()` | `RING_JOG_STEP` |
| a continuous jog's packets (`ops/cjog.cpp`) | `plannerQueueLine()`, `plannerQueueAxis()` | `RING_JOG_CONT` |
| `goto`'s moves (`controller/seq/`) | `plannerQueueLine()`, `plannerQueueAxis()` | `RING_SEQ` |
| a job's BEZIER records (`data_plane.cpp`) | `plannerQueueRecord()` | `RING_JOB` |

* **Units:** machine mm, A in degrees, feeds in units per second. The
  commands convert from work coordinates and check the soft range first
  ([positioning](../machine/positioning.md)).
* **`bez`** gives three control points; the start is where the queued moves
  end. The Pico analyses the curve itself (§5). A job's records arrive
  already analysed by the host.
* **Step jogs** keep at most four blocks queued (`JOG_MAX_QUEUED`); a fifth
  is `→ err busy`.
* **Core 0 also pushes on its own:** a lift when a job is paused or aborted
  (`plannerQueueLift()`) and a duty lift (§14).

## 4. Admission

Every push first passes `admit()`:

* **State:** the machine is IDLE, or already running motion this push may
  join. A jog joins JOGGING, or a job in tool change, tool wait or place; a
  record joins a job that is buffering, running or dwelling. A requested
  pause or abort refuses every push.
* **Config:** a valid machine config.
* **Limits:** X and Y must have a `maxFeed` and `maxAccel`; Z and A take
  theirs from the head bound to their slot, and a move on a slot no head
  binds is refused.
* **Owner:** a push whose kind is not the ring's owner is refused.

The result is a `PlannerQueueResult`, answered as:

| Result | Command reply | Record NACK |
| --- | --- | --- |
| `PQ_OK` | `→ ok <depth>` | none |
| `PQ_BAD_STATE` | `→ err bad_state` | |
| `PQ_NO_CONFIG` | `→ err unconfigured` | |
| `PQ_NO_LIMITS` | `→ err no_limits` | |
| `PQ_FULL` | `→ err full` | `MSEG_NACK_FULL` |
| `PQ_BAD_CURVE` | `→ err bad_curve` | `MSEG_NACK_BAD_CURVE` |
| `PQ_SOFT_LIMIT` | `→ err soft_limit z` | `MSEG_NACK_SOFT_LIMIT` |
| `PQ_NO_FEED`, `PQ_NO_JOB`, `PQ_NO_RESUME` | | jobs only |

`<depth>` is the number of blocks queued, the running one included. `goto`
reports its own refusals ([commands](../reference/commands.md)).

## 5. Blocks

The ring holds blocks (`Block`). A block is one move, its shape, its speed
caps and, once planned, its profile.

### Positions

* **A position** (`Pos`) is X, Y, Z in mm and A in degrees. It is not a
  vector: no maths runs across all four.
* **A is a heading and a turn count:** `a` in [0, 360) and an integer
  `turns`. A float A would lose a step after about a thousand turns; this
  keeps A exact however far it has turned. `addA()` turns either way and
  carries into `turns`.

### Axis sets

A block moves one set of axes (`Axes`):

| Set | Moves | Direction for junctions |
| --- | --- | --- |
| `AXES_XY` | X, Y, with A riding along on a line or following a Bézier | the XY tangent |
| `AXES_Z` | Z alone | `{±1, 0}` |
| `AXES_A` | A alone | `{±1, 0}` |

A move that changes Z with anything else is `AXES_MIXED` and refused: mm and
degrees share no length. A move that changes nothing queues nothing.

### Lines

`pushMove()` builds a line (`Line`, `makeLine()`) from where the ring ends to
the target. An XY line may carry an A change, which A makes linearly along it
(`aRides()`). A Z or A line is a path along that axis alone (`axisPath()`).

### Béziers

A Bézier (`Bezier`) is four control points and what the planner needs from
them:

* its arc length;
* `t(s) = c1·s + c2·s² + c3·s³`, the curve parameter at `s` mm, fitted so that
  `t(length) = 1`. A fitting error moves a point along the exact curve (speed
  ripple), never off it;
* its largest curvature `kappa_max`, the largest rate of change of curvature
  `dkappa_max`, and the signed curvature at each end.

There are two ways in:

* **`analyzeBezier()`** works it out on the Pico (128 samples, integrated by
  3-point Gauss–Legendre, about 0.6 ms). `bez` uses it, outside the lock.
* **`checkBezier()`** takes a record the host analysed, derives `c1` and the
  end curvatures, and checks the host's numbers against the control points
  cheaply, with slack for doubles rounded to floats.

Either refuses a curve as `BezierError`: a degenerate handle (no end tangent),
a cusp, a fit that runs backwards, or host numbers inconsistent with the
points. All are `PQ_BAD_CURVE`.

`pushBezierFrom()` can start part-way along a curve (`s0`), which is how a
paused job resumes mid-curve. A Bézier block whose A follows the heading is
`tangent`; §10 says how A then moves.

### Events and tags

* **An event** (`pushEvent()`) is a block with no motion that the executor
  stops at, at rest on both sides, until Core 1 has run it. Its code is
  opaque to the library; the follower runs tool events (§14).
* **Tags:** every block carries the caller's job record (`layer`, `record`),
  the distance to the next break (`break_dist`), where a pause resumes
  (`resume_s`) and why it lifts (`lift`). The library stores them and reads
  none of them.

## 6. Speed caps

Each block gets a top speed (`v_max_sqr`, squared) and an acceleration along
the path (`accel`). The path (`Path`) holding them is all that look-ahead
sees of a block. Every cap that applies is taken; the lowest wins.

| Cap | Applies to | Rule |
| --- | --- | --- |
| The feed | every block | `v ≤ feed` |
| The axes, along a line | XY lines | per axis i: `v ≤ maxFeed_i / \|u_i\|`, `a ≤ maxAccel_i / \|u_i\|`, `u` the unit direction |
| The axis | Z and A lines | that axis's `maxFeed` and `maxAccel` |
| A riding a line | XY lines with an A change | `v ≤ ω_max / r`, `a ≤ α_max / r`, `r` degrees of A per mm |
| The axes, on a curve | Béziers | `a = min(maxAccel_X, maxAccel_Y) / √2`, `v ≤ min(maxFeed_X, maxFeed_Y)` |
| Curvature | Béziers | `v² ≤ a / κ_max` |
| A's turn rate | tangent Béziers | `v ≤ ω_max / κ_max` |
| A's turn acceleration | tangent Béziers | `v² ≤ ½·α_max / dκ_max`, and `a ≤ ½·α_max / κ_max` |
| The blade offset | while a blade is latched | XY's limits at the knife's axis, not the tip (below) |
| The mesh | XY blocks with the mesh active | Z keeps up with the slope (§13) |

* **On a curve, √2:** the tangential and centripetal accelerations each get
  `a/√2`, so their sum stays inside `a` in any direction, braking included.
* **A's acceleration on a tangent curve** is `a_t·κ + v²·dκ/ds`; half of
  A's limit goes to each term (`kAAccelShare`).
* **A's limits** are in degrees and converted to radians against κ in 1/mm.
* **The blade offset** (`bladeCaps()`): the knife's axis runs `d` mm ahead of
  the tip along A's heading. On an A-only block it sweeps a circle of radius
  `d`, so XY's feed and acceleration cap A. On an XY block turning `rate`
  radians per mm it runs up to `1 + d·rate` faster than the tip, so the cap
  and acceleration are divided by that, and `v² ≤ a / (d·rate²)` holds the
  axis's centripetal acceleration.
* **Caller caps** (`PathCap`): a caller can add its own top speed and
  acceleration to a push. Core 0 adds the mesh's this way, so the library
  never sees the mesh.

## 7. Junctions

Where one block meets the next, the planner caps the speed through the join
(`junctionMaxSqr()`), the next block's highest entry (`max_entry_sqr`).

* **Junction deviation** (as GRBL): the speed at which a circle staying
  within `deviation` of the corner could round it at the lower of the two
  accelerations: `v² = a·δ·sin(θ/2) / (1 − sin(θ/2))`, with δ = 0.02 mm
  (`kDeviation`). Straight on, only the two blocks' own caps apply; a full
  reversal stops.
* **A change of axis set stops.** There is no angle between mm and degrees.
  A Z or A block's direction is `{±1, 0}`, so two Z blocks the same way join
  at full speed and a reversal stops, by the same formula.
* **A's speed may only jump so far.** Where A's rate per mm changes at a join
  (a curvature jump on a tangent curve, or a line's A change starting), A's
  speed jumps by `v·Δrate`. It is capped at `√(2·α_max·e_max)`, the jump that
  leaves the blade at most `e_max` behind (`aJump()`, `kAMisalignRad`, 2°).
* **An event, the first block, and a block after an event** enter from rest.

## 8. Look-ahead

Each time a block is pushed, the plan is worked out again (`replan()`), the
way GRBL does:

1. **Reverse,** from the newest block to the oldest not yet running: the
   newest ends at rest, and each entry is the most that can still stop in
   time: `v_entry² ≤ v_exit² + 2·a·L`, capped by the junction and the block's
   own cap.
2. **Forward,** from the running block's exit: each exit is what can be
   reached, `v_exit² ≤ v_entry² + 2·a·L`.
3. **Each block becomes a trapezoid** (`Trapezoid`, `makeTrapezoid()`):
   accelerate, cruise, decelerate, at the block's one acceleration. A block
   too short to reach its cruise speed is a triangle.

* **The newest block always ends at rest,** so a ring that drains stops on
  its own: the machine stutters rather than overruns.
* **Position is closed-form in time** (`Trapezoid::position()`), so running
  a profile never accumulates error.
* **Speeds are kept squared** throughout, as the passes produce them.

## 9. The ring between the cores

The ring (`Planner`, `plannerRing`) holds 64 blocks (`PLANNER_RING_SIZE`).
Core 0 pushes, plans and commits; Core 1 claims the oldest block, runs it and
releases it.

### Locking

The library is single-threaded; the caller locks. Core 0 holds `plannerLock`
around `pushMove()`, `pushBezierFrom()`, `pushEvent()` and `commit()`. Core 1
holds it only on a tick that claims, releases or resets (§12).

### Replan and commit

Look-ahead is two calls, so the lock is held only briefly:

* **`replan()`** runs without the lock. It reads only the blocks' paths,
  which Core 1 never writes, and writes its speeds into scratch.
* **`commit()`** runs under the lock and copies the speeds into the ring. Every
  push, claim and release bumps an epoch; a commit whose plan began before the
  epoch changed is refused.
* **Core 0 retries** (`replanAndCommit()`): replan, commit, and again with
  Core 1's clock read afresh until a commit is taken.

### The running block

A claimed block's geometry never changes. Its exit speed may still rise, as
GRBL's planner allows, when later pushes show it need not stop:

* **The plan for the running block** starts from a horizon a little ahead of
  Core 1's clock (`ahead_`, 5 ms). `replan()` builds the rest of the block
  from there as a piece (`Piece`): its start time, its start along the block,
  and a new profile with the higher exit.
* **The piece is staged** in one slot that Core 1 takes without the lock
  (`takeStaged()`). `commit()` refuses unless Core 1 is still more than a
  guard (`guard_`, 1.2 ms, the longest tick) short of the horizon, and the
  last piece has been taken.
* **The block after the running one** enters at the running block's
  committed exit (`pinned_entry_sqr_`), so the plan stays continuous.
* **A piece Core 1 finds too late** (its clock already past the start) is
  refused and counted (`lateAdoptions()`), and the executor holds (§15).

### Reset and the latches

The ring starts again only while it is empty and Core 1 is not running it
(`resetIfIdle()`). The first push from idle resets it, and with it Core 0
latches what the whole ring runs with:

* **the owner** (`ringOwner`): the kind of motion pushing; any other kind is
  refused until the ring next drains;
* **steps per unit** for X, Y, Z, A (`plannerSpm`), signed so an inverted axis
  still plans in machine units; 0 on a slot no head binds;
* **one turn of A in steps** (`plannerTurnQ32`), Q32.32;
* **the mesh** (`plannerMesh`, §13) and **the blade** (`plannerBlade`): the
  mounted tool's trail `d` and its edge's heading at A = 0;
* **the start position** (`posFromSteps()`): `machinePos` in planner units,
  the XY of the blade tip (less the blade lead), Z flat (less the mesh), A
  split into heading and turns in double.

### Front pushes and restarts

* **Front pushes** (`beginFront()`, `endFront()`): blocks pushed between
  the two go before the oldest block, from rest. Only with no block claimed;
  the duty lift uses it (§14).
* **A restart** (`restartFrom()`): the machine stopped part-way into a
  block. The block is trimmed to start there and the plan enters from rest.

## 10. The executor

The executor (`Executor`, `plannerExec`) runs on Core 1. Each tick
(`tick()`) moves its clock forward by the tick's time and returns the
position along the path.

* **Claiming:** with no block running it claims the oldest, and runs its
  profile from the start. Time left over when a block ends carries into the
  next.
* **Adopting:** before each tick, `adopt()` takes a staged piece. It takes
  over at its start time; the block then ends where the piece does.
* **Position** (`pointAt()`): on a line, along its direction, A riding along
  linearly; on a Bézier, `B(t(s))`; on a Z or A block, along that axis.
* **A on a tangent Bézier** is tracked tick to tick (`follow()`): the
  heading's wrapped change since the last tick is added to A. A curve may
  turn past 180°, so A is unwrapped this way, not computed at each point.
  Where the tangent nearly vanishes A holds. At the block's end A snaps to
  the planned target; a miss of more than 0.5° (`kAMissDeg`) is counted
  (`aMisses()`).
* **A block's end** snaps the position to the block's target, so rounding
  never builds up from block to block.

### Holds

* **`hold()`** brakes along the path at each block's own acceleration. The
  committed plan is at or above this braking curve everywhere and ends at
  rest, so a hold stops no later than the plan would.
* **Held:** stopped. Three ways out:
  * `resume()`: replan from rest where it stopped and run on;
  * `release()`: restart the ring where it stopped without a plan; Core 0
    edits the ring, replans and commits before the next tick;
  * `abort()`: hold, then empty the ring and reset once stopped.
* **At an event** the executor waits, at rest, until Core 1 calls
  `eventDone()`. A hold reaching an unrun event is complete there.
* **`needsRing()`** says whether the next tick may claim, release or reset,
  so the caller knows to take the lock. Every other tick touches only the
  executor and the claimed block.

## 11. From position to step targets

Each tick, Core 1 turns the executor's position into a step target per slot:

| Slot | Target, in steps |
| --- | --- |
| X | `(x + lead_x) · plannerSpm[0]` |
| Y | `(y + lead_y) · plannerSpm[1]` |
| Z | `(z + meshAt(x + tip_x, y + tip_y) − ref) · plannerSpm[2]` |
| A | `a · plannerSpm[3] + turns · plannerTurnQ32` |

* **The blade lead** (`bladeLead()`): the planner's XY is the blade tip; the
  motors carry the knife's axis `d` mm ahead along the edge's heading at
  this tick's A.
* **The mesh:** the planner's Z is flat; the mesh offset at the selected tip
  is added here (§13).
* **A** is a heading times steps per degree, plus whole turns in steps
  computed once in Q32.32, so its target does not lose resolution as turns
  add up.
* **Steps per unit are signed,** so an inverted axis needs nothing more.

## 12. Followers and the stream byte

The follower loop (`processPlanner()`) runs a 20 µs slot, 50 slots to a 1 ms
tick.

* **One follower per slot:** a Q32.32 step position (`accum`) and an
  increment per slot (`inc`). Each slot the increment is added; where the
  integer part changes, that slot steps.
* **One stream byte per slot,** steps or not: bit 2n steps slot n, bit 2n+1
  is its direction, set while the increment is positive
  ([bus](bus.md) §3). Slots are X, Y, Z and A, Z and A the selected head's.
* **Each tick sets the increments** (`incrementFor()`) to close the gap to
  the step target over the next 50 slots. Division runs in 32 bits at 2⁻²⁵ of
  a step; what it misses is gap again on the next tick, so error never
  accumulates. The increment stays under one step per slot, the most a byte
  can carry.
* **The followers start** at `machinePos`, plus half a step, so a step falls
  where the target crosses a half and position rounds to the nearest step.
* **`machinePos`** is written as each byte goes out, for each slot that
  stepped.
* **A tick that needs the ring** (`needsRing()`) only tries the lock. If Core 0
  holds it, the tick waits for the next slot, its time carried over, while
  the followers keep their increments.
* **At rest** only once every increment is 0 and the executor's speed is 0:
  the followers have caught up.

## 13. The mesh

The bed mesh is a grid of heights the Z follows under every XY move.

### The file

`/mesh.bin` on LittleFS, apart from the config, read into RAM at boot
(`meshInit()`). Little-endian:

```text
u32 magic 'MESH', u16 version 1, u16 nx, u16 ny, u16 pad
f32 x0, y0, dx, dy          mm, tip machine coordinates
i16 z[nx·ny]                µm, + up, x fastest
u32 crc32                   of everything before it
```

Up to 16384 points (`MESH_MAX_POINTS`), at least 2 × 2. Missing is absent;
a bad size, magic, version or CRC is bad; both run flat. `uploadfs` flashes a
bench mesh from `data/mesh.bin`.

### The lookup

`meshAt()` is bilinear between points, the edge value outside the grid. It is
RAM code with no library calls, so Core 1 runs it every tick.

### The rule

* **Active** when the file is good, the mesh is on, and X, Y and Z are homed.
  `mesh on|off` is volatile; at boot it follows `machine.meshOn`.
* **The offset** is the height at the selected tip less the height at the
  work origin: `mesh(tip) − mesh(work origin)`. One mesh serves both heads.
* **The planner's Z is flat.** Actual Z = flat Z + offset at the tip. Core 1
  adds the offset each tick (§11); Core 0 subtracts it when the ring starts
  from the motors (§9). The first tick then targets where Z already is, so Z
  never jumps.
* **Latched per ring** (`meshLatch()`, `plannerMesh`): the grid, the tip's
  offset from the anchor, and the reference height. A change at rest (`mesh
  on|off`, homing, `select`, a new work origin) moves nothing; the next ring
  takes it.
* **Work Z is flat too:** `wzero z` stores actual Z less the offset, and
  `WPos` Z subtracts it, so 0 is on the material everywhere.

### Along a block

As Core 0 pushes an XY block it walks the offset along it at half the grid
spacing (`meshWalkLine()`, `meshWalkBezier()`), under the lock:

* **The range:** flat Z plus the offset's range must stay inside Z's soft
  range, else `PQ_SOFT_LIMIT`. A continuous jog is clipped instead, at the
  last point that fits (`meshClip()`), to 0.01 mm.
* **The caps** (`meshCap()`): `v ≤ Z maxFeed / slope`; half of Z's
  acceleration to the tangential share (`a ≤ ½·Z maxAccel / slope`), half to
  slope changes, where crossing a grid line is taken as a junction:
  `v² ≤ ½·Z maxAccel · step / Δslope`.

### Status

* `get mesh` → `<nx>x<ny>`, `off`, `flat` or `bad`.
* `get meshslope` → the steepest slope between neighbouring points: Z's
  `maxFeed` over it is the slowest the mesh can make XY.

## 14. Duty limits

A powered tool may have duty limits: it may run at most `maxOnS` at a time,
and after a long run (`minOnS` or more) must rest `dwellS` before it starts
again. The planner meets them in two places.

### Tool events

The tool is switched by events in the ring, run by Core 1 at rest
(`runEvent()`):

* **`TOOL_EV_ON`:** if the tool ran `minOnS` or more and has been off less
  than `dwellS`, wait out the dwell (the job shows DWELL); switch on, up to
  three tries; wait `settleS`. A tool that will not switch on raises
  `FACT_TOOL_FAILED` and waits there for the abort the fault brings.
* **`TOOL_EV_OFF`:** switch off; a tool that will not is made safe
  (`CMD_MAKE_SAFE`) and raises `FACT_TOOL_FAILED`.
* **The counters** (`dutyOnMs`, `toolOnAtMs`) are kept across rings, so a
  pause counts as time off. `get onms` reads them.

### Planned stops, on Core 0

As records are queued, Core 0 estimates the tool's on-time where the queue
ends (`estOnS`), from rest to rest at the cut feed. At a break past `minOnS`,
if the next stretch would certainly run past `maxOnS`, the record is queued
with a duty lift before it: lift, tool off, tool on (which dwells), plunge.
A tool with duty limits keeps four ring slots free for the deadline below.

### The deadline, on Core 1

Curvature slows a cut more than the estimate counts, so Core 1 checks each
tick of a cut (`dutyDue()`): on-time so far, plus the time to brake, plus
the lift, plus 50 ms of margin (`kDutyMarginS`), against `maxOnS`. When it is
due:

1. The executor holds.
2. At rest, Core 1 records where it stopped (`dutyStop`), releases the
   executor there, sets `dutyWait`, and raises `FACT_DUTY_HELD`. The
   followers idle while `dutyWait` is set.
3. Core 0 (`plannerDutyLift()`) pushes lift, tool off, tool on and plunge in
   front of the ring, replans, commits, and clears `dutyWait`. If they do
   not fit Z's soft range or the ring, it raises `F_TOOL`, which aborts the
   job.

## 15. Running and stopping

`processPlanner()` is Core 1's planner mode ([cores](cores.md) §4). It
starts motion, runs it, and hands the machine back.

* **Entering** (`enter()`, under the lock): a jog's or a sequence's ring
  runs at once. A job's runs once it has buffered: `machine.buffer.queueTime`
  seconds of motion queued, the ring full, or no new record for
  `machine.buffer.idleTime`. Core 0's lifts in a held or aborted job run at
  once.
* **Holds,** in order of precedence:

  | Hold | Started by | When stopped |
  | --- | --- | --- |
  | Abort | `abortRequested` | the ring emptied; leave drained |
  | Pause | `pauseRequested`, jobs only | keep where the job stopped (`jobResume`), drop the ring, raise `FACT_HELD` |
  | Late piece | a staged piece found too late | a jog resumes; a job goes back to buffering |
  | Duty | `dutyDue()` | the duty lift (§14) |

* **Leaving:**
  * a jog, a sequence, or a held or aborted job's lift drains: clear the
    pause and abort requests and raise `FACT_RING_DRAINED`;
  * a running job's ring runs dry: wait in BUFFERING, or TOOL_CHANGE if a
    layer asked for one.
* **What it publishes:** `machinePos` as it steps; for a job, the record the
  running block belongs to (`jobExec`) and the lifts it has begun (`jobLift`).
* **E-stop:** the slot loop returns at once on `estopRequested`. Core 1's
  sweep then drops the ring (`dropPlanner()`).

The job states named here are the jobs page's.

## 16. I/O

### Host to Core 0

Commands (`jog`, `line`, `bez`, `goto`, `mesh`), continuous jog packets, and
job records. Replies as in §4.

### Core 0 to Core 1

| What | Channel |
| --- | --- |
| Blocks and their plan | the ring, under `plannerLock` |
| A raised profile for the running block | the staged piece, without the lock |
| What the ring runs with | the latches, set at reset: `ringOwner`, `plannerSpm`, `plannerTurnQ32`, `plannerMesh`, `plannerBlade` |
| Stop requests | `pauseRequested`, `abortRequested`, `estopRequested` |
| The tool | `jobPower`: node, command, settle and duty limits |
| A duty lift is in | `dutyWait` cleared |

### Core 1 to Core 0

| What | Channel |
| --- | --- |
| Position | `machinePos` |
| Its clock in the running block | `plannerExec.clock()`, read without the lock |
| Where a job is | `jobExec`, `jobLift`, `jobResume`, `dutyStop` |
| What finished | facts: `FACT_RING_DRAINED`, `FACT_HELD`, `FACT_DUTY_HELD`, `FACT_TOOL_FAILED` |

### Core 1 to the bus

One stream byte per 20 µs slot; command frames for tool events, which stretch
a slot, after which the slot clock restarts.

## 17. Lifecycle

* **Boot:** Core 0 claims the lock, resets the ring at the origin, and reads
  the mesh after the config.
* **The first move from idle** resets the ring and latches (§9).
* **Running:** Core 1 is in `processPlanner()` while it owns motion
  (`plannerActive`).
* **Draining:** the newest block ends at rest, so a ring that empties stops
  smoothly.
* **E-stop:** the loop returns mid-slot; Core 1 drops the ring and the
  executor at the position reached. Steps already sent stand.
* **Failures:** a refused push answers its caller and leaves the ring as it
  was. A late piece holds. A tool that will not switch raises
  `FACT_TOOL_FAILED`; a duty lift that cannot fit raises `F_TOOL`.

## 18. Who writes what

| Value | Code name | Writer | Readers |
| --- | --- | --- | --- |
| The ring | `plannerRing` | Core 0 pushes and commits; Core 1 claims and releases | both |
| The executor | `plannerExec` | Core 1; Core 0 resets it at an idle reset | both |
| The ring's lock | `plannerLock` | both, briefly | both |
| Core 1 owns motion | `plannerActive` | Core 1, under the lock | both |
| The ring's kind | `ringOwner` | Core 0, at reset | both |
| Steps per unit | `plannerSpm` | Core 0, at reset | Core 1 |
| One turn of A, in steps | `plannerTurnQ32` | Core 0, at reset | Core 1 |
| The mesh for the ring | `plannerMesh` | Core 0, at reset | both |
| The blade for the ring | `plannerBlade` | Core 0, at reset | both |
| The staged piece | `staged_` | Core 0, at commit | Core 1 |
| Machine position | `machinePos` | Core 1 as it steps; Core 0 with the ring idle | both |
| Where a job runs | `jobExec`, `jobLift` | Core 1 | Core 0 |
| Where a pause stopped | `jobResume` | Core 1 | Core 0 |
| Where a duty hold stopped | `dutyStop` | Core 1 | Core 0 |
| A duty lift pending | `dutyWait` | Core 1 sets, Core 0 clears | both |
| On-time | `dutyOnMs`, `toolOnAtMs` | Core 1 | Core 0 (`get onms`) |
| Estimated on-time | `estOnS` | Core 0 | Core 0 |
| The mesh grid | `heights` | Core 0, at boot | both, through `plannerMesh` |
| The mesh switch | `meshEnabled()` | Core 0 (`mesh on\|off`) | Core 0 |

## 19. Decisions

### The library

* **Platform-free and single-threaded; the caller locks.** Rejected: locks
  inside the library, which would tie it to the Pico and keep it off the
  host's tests.
* **Replan without the lock, commit with it, refused if stale.** Rejected:
  planning under the lock, which would stall Core 1's tick for a whole
  replan.
* **A staged piece raises the running block's exit, taken without the lock.**
  Rejected: never touching a claimed block, which caps every block's exit at
  what was known when it was claimed and slows long runs of short blocks.
* **Squared speeds, and position closed-form in time.** Rejected:
  integrating speed each tick, which accumulates error.
* **`Path` is all look-ahead sees, and callers add caps through `PathCap`.**
  Rejected: the library knowing the mesh or the tools.
* **A fixed ring, no allocator; tags and events opaque.** Rejected: job
  types inside the library.
* **`Pos` is not a vector; A is a heading and whole turns.** Rejected: a
  float A, which loses a step after about a thousand turns.
* **`PLANNER_RAM` is an opt-in macro.** Rejected: Pico section attributes in
  the library.

### The cores

* **Planning on Core 0, the tick on Core 1.** Core 0 has time and no deadline;
  Core 1 has a deadline every 20 µs and nothing else to do.
* **A fixed 50 kHz stream**, one byte per slot, steps or not. Rejected:
  per-step scheduling, which needs timers per axis.
* **Position followers.** Each tick sets where each axis should be, and the
  follower closes the gap. Rejected: sending velocities, where error
  accumulates.
* **Interpolation on the controller.** Rejected: interpolating on the nodes,
  which needs a shared time base across nodes that the half-duplex bus never
  delivered.

### Motion

* **Cubic Béziers on the wire, analysed on the host and checked on the
  Pico.** Rejected: lines only (tessellation), biarcs, clothoids.
* **Junction deviation at joins.** Rejected: the curvature-jump rule alone,
  which gives nothing for corners between lines.
* **A change of axis set stops.** Rejected: an angle between mm and degrees.
* **A flat planner Z, the mesh a rule.** The mesh is added on the way out and
  subtracted on the way in, so a change at rest moves nothing. Rejected:
  storing the mesh in the planner's Z, which a mesh change at rest would make
  stale.
* **`a/√2` on curves.** Rejected: separate tangential and centripetal
  limits, which together could exceed an axis's limit.

## 20. Code

| File | Holds | Section |
| --- | --- | --- |
| `lib/planner/planner/path.h` | `Vec2`, `Pos`, `Axes`, `AxisLimits`, `Path`, `PathCap` | 5, 6 |
| `lib/planner/line.cpp`, `planner/line.h` | lines, axis paths, junctions, A riding, blade caps | 5, 6, 7 |
| `lib/planner/bezier.cpp`, `planner/bezier.h` | analysis, the check, headings, turns | 5, 6 |
| `lib/planner/trapezoid.cpp`, `planner/trapezoid.h` | profiles | 8 |
| `lib/planner/planner.cpp`, `planner/planner.h` | the ring, look-ahead, commit, staging, front pushes | 8, 9 |
| `lib/planner/executor.cpp`, `planner/executor.h` | ticks, holds, events, A following | 10 |
| `lib/planner/mesh.cpp`, `planner/mesh.h` | lookup, walks, caps | 13 |
| `lib/planner/planner/ram.h` | `PLANNER_RAM` | 1 |
| `src/rp2350/core0/planner/queue.cpp`, `queue.h` | admission, resets and latches, the mesh walk, job records, duty lifts | 3, 4, 9, 13, 14 |
| `src/rp2350/core0/ops/mesh.cpp`, `mesh.h` | `/mesh.bin`, the rule, the latch | 13 |
| `src/rp2350/core1/emit/follower.cpp` | the slot loop, offsets, followers, holds, events, duty | 11, 12, 14, 15 |
| `src/rp2350/ipc/shared_state.h` | the shared ring, executor, lock, latches | 9, 16 |
| `src/rp2350/core0/cmd/axis.cpp` | `jog`, `line`, `bez` | 3 |
| `src/rp2350/core0/ops/cjog.cpp` | continuous jogs | 3 |
| `test/test_planner/` | the library's tests | 2 |
