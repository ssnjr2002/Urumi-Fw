# Coordinate system

The machine's frames, axis directions, offsets and the work frame, owned by
the Pico. Supersedes §1, §4 and §6 of docs/coordinate_frames_and_limits.md,
which predates the Pico controller. The homing config that sets the machine
frame is in docs/plans/controller-homing.md.

## Decisions

### Directions

* **+ is physical and fixed.** Looking at the bed from above, from the
  operator's side: X+ is right, Y+ is up (away from the operator). Z+ is up,
  away from the work (ISO 841).
* Rotary A (a rotation about Z; ISO would name it C): + is counter-clockwise
  seen from above (the right-hand rule about Z+), 0° is the knife's edge
  facing X+. A is then the path heading, `atan2(dy, dx)`, with no offset or
  sign flip.
* **`invertDir`** per axis is the wiring fix that makes the motor obey it.
  Commissioning checks it by jogging each axis +.
* Only the + direction can mirror a job. Where 0 sits only shifts the
  numbers, so it is free to choose.

### Machine frame (`MPos`)

* The anchor's position: the laser tip if one is fitted, else the head the
  config places at zero offset.
* 0 per axis is placed by homing: `parkPos` (controller-homing.md), absent
  meaning the all-positive front-left default.
* Signed: any corner may be the origin. On the current machine (X switch
  right, Y switch front):

  | Origin | X range | Y range | X `parkPos` | Y `parkPos` |
  |---|---|---|---|---|
  | front-left | 0 … +max | 0 … +max | +max | 0 |
  | front-right | −max … 0 | 0 … +max | 0 | 0 |
  | back-left | 0 … +max | −max … 0 | +max | −max |
  | back-right | −max … 0 | −max … 0 | 0 | −max |

  Rule: `parkPos` is 0 when the switch is at the origin end, +max when the
  switch is at the + end and the origin at the − end, −max the other way.
* Coordinates are tip positions, measured by the machine: the unmeasured
  distance from the switch striker to the anchor cancels, because every bed
  point (work origin, probe station, offsets) is taught by jogging a tip to it.

### Controlled point

* The selected head and tool: the anchor plus the head offset (XY) plus the
  tool offset (XY). Offsets are measured tip to tip.
* No tool length offset: each head has its own Z and keeps its tool, and a
  blade change moves the tip by an unknown amount, so the tip height is
  measured per head (the work Z below), never stored.
* Selecting a head works like a G-code tool change: the offset changes, the
  gantry does not move, `WPos` jumps by the offset.
* Z and A are per head, with no shared-axis arithmetic.

### Work frame (`WPos`)

* One work offset to start (the G54 role), in machine coordinates, valid
  across re-homing (homing re-establishes the machine frame exactly):
  * XY: a point on the material, shared by every head.
  * Z: per head, the Z where that head's tip meets the material.
* A `work` config block gives the default (absent: 0, so `WPos` equals
  `MPos`). A volatile runtime offset overrides it: set by "zero here" (the
  selected tip's position) or by coordinates, cleared back to the config
  value, lost on reboot. The Pico never writes the config; to keep an offset
  the host writes it there.
* `WPos` = selected tip − work offset. Signed.
* Z0 by probing builds on the probe's contact height (`pz`) later; "zero
  here" with a tip touched off by hand works from the start.
* A has no work offset: the heading is absolute, and a constant offset would
  cut sideways. Its 0 (knife edge facing X+) is machine calibration,
  `indexPos` in the homing config. A skewed sheet is a job rotation in XY
  (the G68 role), not an A offset.

### A

* `MPos` A is the count in degrees, unbounded: it is never folded.
* `WPos` A is the heading: `MPos` A folded to (−180°, 180°], no offset. Motion
  and range checks use `MPos`; the fold loses the turn count.

### Stored positions

* A `positions` config block of anchor positions in machine coordinates, the
  same for every head (the G28 and G30 roles): `park` (absent: where homing
  parks), `load` optional.
* Each head's `probeSwitch`: a tip position in its head block. There is one
  switch at each end of X and a head reaches only its own. Since
  tip = anchor + head offset, the anchor goes to switch − head offset to put
  the tip on it. The decoder refuses a switch outside
  its head's reach.
* Going to one is `jogto park|load|probe` (motion-sessions.md): `probe` is the
  selected head's switch, since a head reaches and probes only its own. Every
  head's Z goes to its park position first, then XY; a numeric `jogto` moves
  XY only. Z's park is just below its switch (the pull-off from it; `parkPos`
  only numbers it), so it is the safe height while Z homes toward a switch at
  the top, which the homing cycle order already assumes. No safe-Z setting.

### Commands and frames

* Every move command takes work coordinates: `line`, `bez`, job records,
  `jogto`. The Pico adds the work offset and the controlled point's offset.
  Relative jogs are frame-free.
* Stored positions are machine coordinates, resolved by the Pico; nothing on
  the wire is in machine coordinates.
* `select <head> [tool]`: binds the head's Z and A (`axes_map`) and makes it
  the controlled point. It never moves.
* `select anchor`: offset (0, 0). With a laser, the laser tip; Z and A stay
  bound as they were. Without one, the head at (0, 0), same as selecting it.
  The decoder refuses a config with no laser and no head at (0, 0).
  `get head` reports `anchor`.

### Limits and reach

* Soft limits per axis: the range from homing (`[park, park ± maxTravel]`).
  The strip between the switch and the park position is outside it.
* Each tip reaches the anchor's range shifted by its offsets. Checked at job
  preflight (each tool's bbox, shifted by the work offset) and on every jog.
  `select` never moves, so it cannot fail on reach.

### Jobs from SVG

* One y reflection (SVG's y points down), then translations only: placement
  and the work offset. A sign change anywhere after the reflection mirrors
  the job.

### Ownership

* The Pico owns the frames. `status` reports `MPos` and `WPos`, each labelled;
  the host displays them. The web's `frames.ts` stays until its job and
  homing paths are removed; nothing new is built on it.

### Dropped

* Positive-only coordinates with + pointing away from the origin
  (`originDir`, `atOrigin`): an origin choice could mirror the frame.
* A reference frame between machine and work: the work offset (default 0)
  does its job.
* A fixed front-left origin: `parkPos` chooses it per axis.
* A left-most-anchor rule to avoid negative tip coordinates: signs are
  harmless.
* Positive-down Z and a host-side Z sign flip: ISO Z, since ranges are signed.

## Wire changes

* `STATUS_RSP` (0xA7) unchanged: steps, sampled together with state. A host
  that wants a live `WPos` reads the offsets once and applies them to it.
* `get` gains keys `mpos=x,y,z,a`, `wpos=x,y,z,a` (units) and `head=<n>|anchor`
  (`tool` once the Pico decodes tools). `pos` stays steps. `status` shows
  them for people.
* Commands: `select <head>`; set the work offset (zero here, by coordinates,
  clear). Going to a stored position is `jogto <name>` (motion-sessions.md).
* `line`, `bez` and job records are read in work coordinates (identical to
  machine coordinates while the offset is 0).

## Branches

Proposed; each gets a full Plan section when its turn comes.

1. `feature/pico-frames`: `select`, the controlled point, the work offset,
   `MPos`/`WPos` as `get` keys, stored positions, soft ranges. Lands before the
   motion-sessions jog branches, which use its frames, positions and range
   check.

## Branch 1: `feature/pico-frames`

### Plan

* Type: feature.
* Purpose: the Pico owns the frames: it selects a head, holds the work offset,
  reads move targets in work coordinates, reports `MPos`/`WPos`, resolves
  stored positions, and refuses targets outside the soft range.
* Depends on: controller-homing (done, 25ece01): `parkPos`, park legs.
* What the code has today:
  * `CfgHead` (`core0/config/config_decode.h:65`) holds only `z` and `a`; the
    decoder (`config_decode.cpp:200`) reads no `xOffset`/`yOffset` and no
    `tools`, though `config/controller.jsonc:81,136,204` has them.
  * No head-select command: the head is whichever head's Z/A nodes the slot
    map binds (`configSlotMap`, `config_decode.cpp:243`, committed for
    `defaultHead` at `controller/seq/controller.cpp:17`; changed by
    `slotmap`/`axesmap`).
  * The planner moves XY only (`plannerQueueLine(x, y, feed)`,
    `core0/planner/queue.h:24`). Jog on the Pico is pico-planner branch 3,
    deferred.
  * `softLimits` is decoded (`config_decode.h:59`) and checked nowhere; Core
    1's ramp check is a stub (`core1/emit/microsegment.cpp:53`).
  * `get` (`core0/cmd/get.cpp`, key table at `:114`) and `status` (`query.cpp:45`) print
    steps.
* Scope:
  1. Config, Pico and web schema/loader: head `xOffset`, `yOffset`,
     `probeSwitch {x, y}`; a `work {x, y, z[heads]}` block; a `positions`
     block (`park`, `load`, each `{x, y}`). The decoder refuses a probe switch
     outside its head's reach. Fixtures and `controller.msgpack` regenerated.
  2. `core0/ops/frames.{h,cpp}` (new): the selected head, the controlled
     point, the volatile work offset over the config default, `MPos`/`WPos`
     in units, work → machine for targets, stored positions resolved to
     machine coordinates (`probe` through the selected head), the soft range
     per axis (`[park, park ± maxTravel]`, side from the homing direction)
     and its check, for the jog branches to call.
  3. `select <head>|anchor` (a controller command): binds the head's Z and A
     through `axes_map`, sets the selected head. `defaultHead` at boot goes
     through it. The decoder check for an anchor (laser, or a head at
     (0, 0)).
  4. `get` keys `mpos`, `wpos`, `head` (`cmd/get.cpp`); `status` shows them.
  5. Work offset commands (`core0/cmd/table.h`): `wzero` (zero here, named
     axes or all), `wset <axis> <v> …` (by coordinates), `wclear` (back to
     config). Names settled in Write.
  6. `line`, `bez` and `plannerQueueRecord` targets read through work →
     machine, and refused outside the soft range on axes with `softLimits`;
     the bench `leg <node> park <count>` target converted through the origin
     and refused likewise (deferred from controller-homing 1b).
  7. Web: `commands.ts` reads the new `get` keys; the sim answers them
     and `select`. The job path sends work coordinates (unchanged while the
     offset is 0). `frames.ts` untouched.
  8. Docs: `docs/wire_protocol.md` (`get` keys, `select`, the work offset
     commands, work coordinates for moves), the config doc for the new
     blocks; motion-sessions.md's frames section points here.
* Out of scope: `jogto` and its Z lift (motion-sessions jog branches), tool
  offsets and `select`'s `[tool]` (the Pico does not decode `tools` yet), Z0
  by probing, anything A beyond `MPos`, the Core 1 ramp check, merging the
  status replies.
* Overlap: `src/rp2350/core0/cmd/table.h`, `web/src/machine/schema.ts`,
  `web/src/wire/`, `lib/planner/` if the record path needs the transform
  there.
* Checks: `pio run -e pico`; `pio test -e native` where it builds (not on
  this Windows machine); `pnpm typecheck` and `pnpm test` in `web/`.
  Human: `get mpos wpos head` after `home`; `select 0|1` binding the head's Z and A;
  zero here with each head selected, `WPos` jumping by the head offset on a
  head change; a `line` in work coordinates landing at offset + target; a
  `line` past the soft range refused.
* Decisions for Read:
  * `select` while a slot is fenced or a node is mute: refuse, or bind what
    answers.
  * Where records get the transform: at ingest in `plannerQueueRecord`, or
    in `lib/planner`.

### Status

Planned.

### Outcome

## Open questions

* Rotary A, for an A-axis pass:
  * The knife has a cable out of it, so A needs a soft range (min/max degrees
    from the homed 0).
  * Each tool has a symmetry order n (period 360°/n): knife 1, crease wheel
    2, revolver pen 7 (to confirm). One mechanism picks, for a target angle,
    the nearest equivalent mod 360°/n inside the range (unwinding first if
    none is reachable). Headings call it every segment; the revolver pen
    calls it once per pen change; A jogs use it. Order lives in the tool's
    config, absent meaning 1.
  * A in the planner: the Pico takes headings from the path tangent. At lift
    points (contour start, corners past `cornerAngleDeg`) any equivalent mod
    360°/n may be chosen; mid-cut A follows the path. Per contour, its heading
    sweep (known from the contour framing) picks the start equivalent that
    keeps the whole sweep inside the range, so unwinding happens only at
    lifts; a sweep wider than the range splits the contour at a lift. Replaces
    the host's `unwind` flag (`choreograph.ts:378`).
  * A limits XY speed on curves: about A's `maxFeed` (deg/s) over the
    curvature (deg/mm), beside the centripetal limit.
  * Tool tip offset: in the knife's frame, rotating with A; found by a test
    cut (opposite-direction slits coincide when it is right).
  * Headings made continuous in `flatten.ts` (each one the previous plus the
    shortest difference). Whether `web/src/toolpath/flatten.ts:70` takes
    headings before or after the y reflection (before means its angles are
    clockwise-positive today).
* Z: the work Z0 by probing; the probe config (`switchXMm/YMm`, `tripMm`)
  restated in these frames, its switch position replaced by the head's
  `probeSwitch`.
* More work offsets (G55–G59), and which stored positions besides park.
* Soft limits for tips beyond the anchor's range (heads that reach past the
  bed edge).
