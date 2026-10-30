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
  seen from above (the right-hand rule about Z+). A heading of 0° is the
  edge facing X+, so a cut's heading is `atan2(dy, dx)` with no sign flip.
  Machine A 0 is where the holder releases the tool; a tool's `aOffsetDeg` is its edge's
  heading there (A, below).
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
* The current configs set `parkPos` on Z only (0 at the top). X and Y take
  the default (`home.cpp` `parkSteps`): seeking +, park is `maxTravel`, so X0
  is the left end, `maxTravel` from the right switch, not at it. `parkPos: 0`
  on X and Y would put the origin at the switches instead.
* Y in the configs seeks + toward a switch commented "at the front", which
  contradicts Y+ away from the operator: either the switch is at the back or
  Y is mirrored. Check by jogging Y+ (`invertDir` on Y is unconfirmed too).
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
  * Z: per head, the Z where that head's tip meets the bed (the underlay's
    top), whatever the material. With a bed zero, `WPos` Z with the tip on
    the switch is the switch's height, and on the material it is `t`.
* A `work` config block gives the default (absent: 0, so `WPos` equals
  `MPos`). A volatile runtime offset overrides it: set by "zero here" (the
  selected tip's position) or by coordinates, cleared back to the config
  value, lost on reboot. The Pico never writes the config; to keep an offset
  the host writes it there.
* `WPos` = selected tip − work offset. Signed.
* Zeroing Z, on the selected head's mounted tool:
  * `wzero z`: the tip is on the bed.
  * `wzero z_switch`: the tip is on the head's bed switch, touched off by
    hand or probed; work Z = here − `probeSwitch.z`. Refused when the head
    has no `probeSwitch.z`. Covers a bed the tip cannot reach beside the
    sheet.
  * No zero on the material: `t` never enters the zero.
* Z0 by probing builds on the probe's contact height (`pz`) later.
* `wclear` and reset leave every head's Z unset until a Z zero; config has no
  `work.z` (Branch Z1).
* A has no work offset: the heading is absolute, and a constant offset would
  cut sideways. Machine A 0 is machine calibration, `indexPos`
  in the homing config; the edge's offset from it is the tool's
  `aOffsetDeg`. A skewed sheet is a job rotation in XY
  (the G68 role), not an A offset.

### A

* **Machine A 0.** The tool holder's interlock releases the tool at one
  angle only; turned away from it, the holder holds the tool. Machine A 0 is
  where the holder releases the tool. A head's `unlockRange` (degrees, absent
  or 0: no interlock) is how far either side of 0 it still releases.
  * `mount <h> <tool>` parks the head's Z, then turns A to
    `unlockRange` + 1° so the holder holds the tool.
  * `unmount <h>` parks Z, then turns A to 0 so the tool can come out.
  * `get unlocked` names the heads whose holder releases now (at rest, A
    homed, |A| ≤ `unlockRange`).
* **`indexPos`** (the head's A homing config) is the machine A given to the
  hall index when homing finds it. Machine A 0 sits 46.7°
  counter-clockwise of the index on head 0, so `indexPos` is −46.7: the
  index reads −46.7. It is per head (holder and sensor), not per tool.
* **`aOffsetDeg`** (per tangential tool, absent = 0) is the heading the tool's
  edge points at when the holder is at 0, i.e. how the tool sits in the
  holder. Measure it on mount: with A at 0, see where the edge points.

  | Tool mounted with edge along | `aOffsetDeg` |
  |---|---|
  | +X | 0 |
  | +Y | 90 |
  | −X | 180 |
  | −Y | −90 (or 270) |

  The crease wheel is either 0 or 90, since its period is 180°.
* A heading h is cut at machine A = h − `aOffsetDeg` (then the nearest
  equivalent for the tool's period inside its cable range).
* `MPos` A is the count in degrees, unbounded: it is never folded.
* `WPos` A is the edge heading: `MPos` A plus the mounted tool's `aOffsetDeg`
  (as `WPos` XY is the tip), folded to (−180°, 180°]. With a tool mounted,
  machine 0 reads `aOffsetDeg` and `WPos` 0 (edge along X+) is at `MPos`
  −`aOffsetDeg`. With no tool it is `MPos` folded.
  Motion and range checks use `MPos`; the fold loses the turn count.

### Stored positions

* A `positions` config block of anchor positions in machine coordinates, the
  same for every head (the G28 and G30 roles): `park` (absent: where homing
  parks), `load` optional.
* Each head's `probeSwitch`: a tip position in its head block. There is one
  switch at each end of X and a head reaches only its own. Since
  tip = anchor + head offset, the anchor goes to switch − head offset to put
  the tip on it. The decoder refuses a switch outside
  its head's reach. `probeSwitch.z` is the switch's height above the bed,
  measured as `WPos` Z with a tip on it after a bed zero.
* Going to one is `jogto park|load|probe` (motion-sessions.md): `probe` is the
  selected head's switch, since a head reaches and probes only its own. Every
  head's Z goes to its park position first, then XY; a numeric `jogto` moves
  XY only. Z's park is just below its switch (the pull-off from it; `parkPos`
  only numbers it), so it is the safe height while Z homes toward a switch at
  the top, which the homing cycle order already assumes. No safe-Z setting.
* `probe_park <tool>` and a job's tool change also turn the head's A to
  machine 0 along the XY line, so a tool can be mounted
  or unmounted there; `a_not_homed` unless A is homed. A head without A
  parks XY only. `jogto` never moves A. The tool-change stop unmounts the
  head about to cut (motion-sessions.md, position-moves.md G5).

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
  A defined laser is (0, 0) by definition: its config is `laser: {node}`, the
  bus id of the node it is wired to, with no offsets. `select anchor` turns
  it on (when the anchor is the laser); a head switch, by `select` or by a
  raw `axes_map`, turns it off.
  The decoder refuses a config with no laser and no head at (0, 0).
  `get head` reports `anchor`.
* `select` drives `axes_map`, but a raw `axes_map` can bind another head's
  nodes, and the laser has no slot. On every committed map the selection is
  re-derived: kept if it still matches (a laser anchor always does); else
  the head whose Z and A the map binds; else the laser anchor if there is
  one; else none. With none, `get head` is `-` and XY moves in work
  coordinates are refused (`err no_head`): the XY offset is unknown.

### Limits and reach

* Soft limits per axis: the range from homing (`[park, park ± maxTravel]`).
  The strip between the switch and the park position is outside it.
* Checked on homed axes only. Absolute moves need homing (motion-sessions'
  motion gating); relative jogs may run unhomed, without limits, only with
  `jogUnhomed` (testing). As Klipper, LinuxCNC and grbl's homing lock.
* Each tip reaches the anchor's range shifted by its offsets. Checked at job
  preflight (each tool's bbox, shifted by the work offset) and on every jog.
  `select` never moves, so it cannot fail on reach.
* Z's lower limit: `WPos` −`maxOvercut` (machine `workZ − maxOvercut`, mesh
  applied), for every move while the head has a work Z. `maxOvercut` is a
  property of the bed, in config. Without a work Z, Z keeps only its axis
  range, so a tool can be jogged down to a touch-off; work-Z gotos and jobs
  already refuse it.
* Layer depth: the layer header carries `t` (material thickness) and `p`
  (plunge depth below the material top), replacing job-z.md's `k`. Plunge
  `workZ + t − p`, travel `workZ + t + c`. Refused: `p − t > maxOvercut`,
  and `p ≤ 0` on a plunging tool. The host derives `p` from the same `t`
  (`p = t + overcut` for a through cut), so an error in `t` moves only a
  partial cut's bottom (`p < t`). A tool with `plunge: false` stays at
  `workZ + t`.

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
* Work Z 0 on the material top: a through cut's bottom would then carry the
  error in `t`, and the lower limit would depend on a number the host sends.
* A stricter Z limit at `WPos` 0 for jogs and gotos: a zero set slightly high
  could not be jogged down to the real bed to re-zero, and the mat takes
  `maxOvercut` by definition.
* `wzero z_material`, and storing bed, switch and material heights (`wref`):
  reading `WPos` after a bed zero gives both heights.
* Overcut as a fraction of `t` (`k` above 1): the overcut is a distance, set
  by the material, blade and mat, not the sheet's thickness.

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

The bed zero (Work frame, Limits and reach) is branches Z2–Z4, a Single
Session in that order: Z2 and Z3 share the config files, and Z4 needs Z3's
`maxOvercut`. Z1, first left out, is reopened after Z3.

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
  `web/src/wire/`.
* Checks: `pio run -e pico`; `pio test -e native` where it builds (not on
  this Windows machine); `pnpm typecheck` and `pnpm test` in `web/`.
  Human: `get mpos wpos head` after `home`; `select 0|1` binding the head's Z and A;
  zero here with each head selected, `WPos` jumping by the head offset on a
  head change; a `line` in work coordinates landing at offset + target; a
  `line` past the soft range refused.
* Agreed in Read:
  * Signed `plannerSpm` (−spm on an `invertDir` axis), so planner mm are
    machine mm: the planner (`planner/queue.cpp:50`) and Core 1
    (`emit/follower.cpp:186`) apply no `invertDir` today, only homing does
    (`seq/home.cpp:80`). No change on the current machine (X and Y false).
  * Records translate at ingest in `plannerQueueRecord`: offsets added to
    `p[0..3]` before `checkBezier`; the analysed fields do not change under
    translation. `lib/planner` untouched.
  * The new config fields are required in the blob (the host's loader fills
    defaults), except `positions.park`, which only the Pico can default.
    `CFG_SCHEMA_VERSION` and `CONFIG_BLOB_VERSION` 2 → 3.
  * The laser is the anchor when defined; the selection re-derived on every
    committed map (Commands and frames, above).
  * Agreed in Write: `laser` becomes `{node}` (offsets dropped); `select
    anchor` switches it on, a head switch off (`CMD_LASER` to that node).
  * Soft range on homed axes only.
* Decision for Write: `select` while a slot is fenced or a node is mute:
  refuse, or bind what answers.

### Status

Done, merged 2026-10-07 (`3ce091f`..`ce63276`). Unblocks the
motion-sessions jog branches (`jogto` calls into `frames`).

### Outcome

* A head's `probeSwitch` is optional in the blob (a head may have none, and
  the host has no default for it), beside `positions.park` and `.load`.
* Reach, the anchor and `work.z` per head are the decoder's `frames`
  rejection; a laser node outside the bus is `node_id`. The host checks the
  anchor and that the laser's node is configured.
* Selection on a map commit: a map that binds a different head than the last
  one is a head switch (that head, laser off); one that leaves the head as
  it was keeps a laser selection. `select <head>` selects the head even when
  its map was already bound.
* The reported selection is derived from the axes request, so a path that
  unmaps (`unstop`, `bus_exclude`) can't leave a stale head: a head needs X,
  Y and its Z/A mapped, else the anchor (laser, X and Y mapped), else none.
  `select n` writes `x y zn an`; `select anchor` maps X and Y if missing and
  leaves Z and A.
* Out of scope: the laser's on/off state can't be read on the wire
  (`nodestat` doesn't report it); a `laser` get key would.
  `bus_exclude` settling `ALARM_BUS_DEGRADED` stays unmapped (decided).
* `select` with a fenced slot or a mute node does what `axes_map` does (the
  map commits, a pending axis holds `ALARM_NODE_FAULT`) and replies with
  its refusal.
* New: `ops/frames.{h,cpp}`, `controller/cmd/frames.cpp`
  (`select`, `wzero`, `wset`, `wclear`), `MSEG_NACK_SOFT_LIMIT` 0x09 for
  records. `wzero` needs the axes homed. Homing's park position comes from
  `configParkPos`. The web host has `getFrames`, `select`, `wzero`, `wset`,
  `wclear` in `wire/link/commands.ts`; the Sim does not model frames (it
  answers `!` for the new keys).

## Branch Z1: `fix/wclear-zeroed`

### Considered

* Found: `framesClearWork` (`ops/frames.cpp:23`), run by `wclear` and reset,
  restores config's `work.z` and sets `zSet` on every head. A `mount` then
  `wclear` (or a reset with the tool kept) passes the tool-change and
  layer-header checks with a zero made for no tool; the Z3 limit, measured
  from that zero, does not catch it either.
* Weighed: unset Z on every head in `framesClearWork` (one line); also
  remove `work.z` from config (a config change for tidiness only) and show
  `WPos` Z as `-` while unzeroed (display only).

Not implemented then: the operator checks the zero by going to it before a
job, and `wclear` and a reset are deliberate.

### Plan

Reopened after Z3: the false "zeroed" now turns on the Z floor, so at boot
and after `wclear` Z is held at config `work.z` (0, the top here) until a
`mount` or `wset z` (demo-console.md D2 Outcome), against wire_protocol.md's
Z floor ("Without a work Z ... jogged down to touch off").

* Type: fix.
* Purpose: no head has a work Z until a Z zero.
  * `framesClearWork` (`ops/frames.cpp:23`) restores XY from config and
    leaves every head's Z unset (0, `zSet` false), at boot and on `wclear`.
    The floor is off, `goto w z` is `no_work`, a layer header stops for a
    tool change, until `wzero z` / `wset z`.
  * Config `work.z` goes: firmware decoder (`config_decode.*`,
    `CFG_SCHEMA_VERSION`), web `schema.ts`, `load.ts` (`buildWork`, the old
    key rejected by name), `validate.ts`, `blob.ts` (`CONFIG_BLOB_VERSION`
    14), fixtures; `config/controller.jsonc`.
  * `WPos` Z while unset stays machine Z less 0.
* Test: no native test reaches `frames.cpp` (`test_cjog` stubs it); benched
  instead: after boot and after `wclear`, `jog z -1` from the top is ok.
* Files: the above; `test/test_config`; `docs/wire_protocol.md` (`wclear`,
  `wzero`, the Z floor); this doc's Work frame bullet.
* Depends on: Z3.

### Status

Planned.

## Branch Z2: `feature/z-switch-zero`

### Plan

* Type: feature.
* Purpose: zero a head's Z on its bed switch, touched off by hand.
  * Config: a head's `probeSwitch` gains `z`, the switch's height above the
    bed (mm). Optional; the decoder refuses a non-finite value.
  * `wzero z_switch`: the selected head's work Z = its tip's machine Z − the
    mesh − `probeSwitch.z`. `err no_switch_height` without one; the rest as
    `wzero z`. Combines with `x`/`y` like `z` does.
  * `wset z` stays.
* Files: `config/config_decode.{h,cpp}` (`CfgHead`, `:235`, `:384`),
  `controller/cmd/frames.cpp` (`cmdWzero`), `test/test_config`; web
  `machine/schema.ts`, `json/load.ts`, `json/validate.ts`,
  `wire/link/commands.ts` (`wzero`), `wire/link/backends/sim.ts`, fixtures;
  `config/controller.jsonc` (head 0's value measured on the bench);
  `docs/wire_protocol.md`.
* Bench: bed zero, read `WPos` Z on the switch, set `probeSwitch.z`, upload;
  remount, `wzero z_switch`, touch the bed: `WPos` Z ≈ 0.
* Not here: `tripMm` folded into `probeSwitch.z` (with the working probe).
* Depends on: nothing.

### Status

Done (merged at `78ccd49`). Unblocks Z3.

### Outcome

* `probeSwitch.z` must also be ≥ 0, not only finite: a switch below the
  bed is refused (`frames` on the Pico, `switchHeightValid` on the host).
  The firmware holds it as `CfgHead.hasSwitchZ` / `switchZ`.
* `sim.ts` unchanged: its `wzero` already treats any word with a `z` as Z.
* `controller.jsonc` has no `probeSwitch` yet: head 0's switch XY and height
  are still to be measured.
* Benched without a tool: refusals, and `wzero z_switch` at Z park reads
  `WPos` Z = the switch height (mesh off). The touch-off check (switch, then
  bed ≈ 0) waits for the measured switch.

## Branch Z3: `feature/z-overcut-limit`

### Plan

* Type: feature.
* Purpose: no move takes a zeroed head's tip deeper than `maxOvercut` below
  the bed.
  * Config: `machine.maxOvercut` (mm, ≥ 0), a property of the bed. Required
    or defaulted: read phase decides with the user (a blob version bump if
    required).
  * Limit: while the bound head has a work Z, Z's lower bound is machine
    `workZ − maxOvercut + mesh at the tip`, in `framesCheckMove`
    (`ops/frames.cpp:315`) and `framesTipRange`; it reaches jogs
    (`cmd/axis.cpp:693`, `:706`), gotos (`seq/goto.cpp:73`, `:98`) and job Z
    moves (`planner/queue.cpp:124`, `:463`). The node-side check
    (`framesCheckNode`, `cmd/axis.cpp:530`) and continuous jog: read phase.
  * Without a work Z: the axis range only, so a tool can be jogged to a
    touch-off.
* Files: the above; `config_decode.*`, `test/test_config`; web config files,
  fixtures; `config/controller.jsonc`; `docs/wire_protocol.md` (the refusal).
* Depends on: Z2 (shares the config files; not its behaviour).

### Status

Done (merged at `5272a56`). Unblocks Z4.

### Outcome

* `maxOvercut` is required in the blob, defaulted to 0 by the host (no tip
  below the bed until set); blob v13, decode error `overcut`.
* The floor takes the mesh offset as an argument: `framesTipRange(k, lo, hi,
  mesh)` and `framesCheckMove(..., off)`, defaulting to 0. Each Z caller passes
  the offset it already uses (latched at the jog's XY, the walk's lo/hi in
  `zInRange`), so the bound is on flat Z: `workZ − maxOvercut`.
* The refusal is the existing `soft_limit z <left>` / `NACK_SOFT_LIMIT`.
* Unchanged: `framesCheckNode` (park legs only go up) and raw `leg` moves,
  which have no soft check.
* Until Z4, a layer plunging `k·t` deeper than `maxOvercut` below the work Z
  is refused at the plunge.
* Benched without a tool: jog and `goto w z` refused at the floor with the
  right room left. Not benched: continuous jog, a job, the mesh, and a head
  without a work Z.

## Branch Z4: `feature/layer-plunge-depth`

### Plan

* Type: feature, wire protocol.
* Purpose: work Z 0 is the bed in the job's heights.
  * `LAYER_HDR` `[15..18]` `k` (f32) becomes `p` (plunge depth below the
    material top, mm, f32), same size; the header's version changes.
  * Heights (`planner/queue.cpp:456-457`): travel `workZ + t + zClearance`,
    cut `workZ + t − p`; a tool with `plunge: false` cuts at `workZ + t`.
  * Refused at the header (`ops/layer.cpp:8`): `p − t > maxOvercut`, `p ≤ 0`
    on a plunging tool; `depth` (`:21`) becomes `p`.
  * Host: `wire/format/layer.ts` sends `p`; the toolpath side derives
    `p = t + overcut` for a through cut; `sim.ts`.
* Files: `data_plane.cpp:229`, `ops/layer.{h,cpp}`, `planner/queue.{h,cpp}`,
  web `wire/format/layer.ts` and its callers, `sim.ts`, tests;
  `docs/wire_protocol.md`; a pointer in job-z.md's work Z bullet.
* Depends on: Z3 (`maxOvercut`).

### Status

Done (merged at `ac3d804`). Completes the bed zero (Z2–Z4); unblocks nothing
further in this plan.

### Outcome

* No version byte in `LAYER_HDR`: the new header is magic `0xA1` (`0xA3` is a
  retired status frame), and `0xA2` is retired and dropped as an unknown byte,
  so an old host's `k` is never read as `p`. Host and firmware flash together.
* `LayerMotion` gains `top` (the thickness); `clearZ`/`cutZ` sit over
  `workZ + top`.
* `p` must be finite for every tool, otherwise it is unused on a tool with
  `plunge: false`; no upper cap besides `p − t ≤ maxOvercut`.
* Host: `packLayerHdr` defaults `p` to the thickness (a cut to the bed). No
  overcut setting exists on the host yet; a caller passes `p` for one.
  `sim.ts` never read the field and is unchanged.
* Benched without a tool: overcut past `maxOvercut`, `p` 0, −1 and NaN refused
  as `NACK_JOB`, an unknown tool as `NACK_TOOL_UNKNOWN`, `0xA2` dropped. Not
  benched: an accepted header and a cut at `workZ + t − p`.

## Open questions

* Rotary A: the cable range (`aMaxDeg`, from machine 0), the tool's symmetry
  period, headings from the path tangent and A capping XY speed are settled
  in docs/plans/job-a.md. Still open:
  * Tool tip offset: in the knife's frame, rotating with A; found by a test
    cut (opposite-direction slits coincide when it is right).
  * Homing A to end at machine 0 rather than wherever the index search
    stops.
  * Headings made continuous in `flatten.ts` (each one the previous plus the
    shortest difference). Whether `web/src/toolpath/flatten.ts:70` takes
    headings before or after the y reflection (before means its angles are
    clockwise-positive today).
* Z: whether a successful probe sets the work Z (`probeZ` feeds nothing
  today); the probe config (`switchXMm/YMm`, `tripMm`) restated in these
  frames, its switch position replaced by the head's `probeSwitch` and
  `tripMm` by `probeSwitch.z`.
* More work offsets (G55–G59), and which stored positions besides park.
* Soft limits for tips beyond the anchor's range (heads that reach past the
  bed edge).
