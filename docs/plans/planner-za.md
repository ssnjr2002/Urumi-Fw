# Z and A in the planner

Z and A on the Pico planner (`lib/planner`, Core 1's follower), so that jogs,
lifts, plunges, tangential cutting and mesh levelling run on the planner and
the MicroSegment path can be retired. Follows docs/plans/pico-planner.md
(its "Later" list) and docs/plans/motion-sessions.md (jogs). Planner scope
only; the job layer (header, `TOOL` record, framing) is motion-sessions' and
uses what this plan builds.

## Decisions

### Axes

* **Free axes and derived axes.** A line moves any of X, Y, Z, A freely (jog,
  lift, plunge, rotate). While XY moves (cut or travel), Z and A are derived
  from the XY path: Z from the work Z plus the mesh, A from the path heading.
  The wire stays 2D: the host sends XY geometry only.
* **The selected head's Z and A are streamed; the other head's Z is parked by
  node park legs.** The stream byte carries 4 slots (X, Y, Z, A;
  `core1/emit/follower.cpp:18`); two heads have 6 axes. The unselected head
  only has to stay parked, which a node park leg does on its own, both heads
  in parallel (motion-sessions J1b). A head switch lifts the old head through
  the planner while it is still selected, then `select` rebinds slots 2 and 3.
* **No 3D cutting.** Z is never a free axis during an XY move; the mesh is the
  only Z under XY motion.
* **Moves on the same set of axes join by junction deviation;** on a single
  axis (Z or A) that is full speed straight on and a stop on a reversal. A
  change of axis set (XY to Z, Z to A, …) stops: there is no angle between
  mm and degrees, and a knife stops there anyway. Z and A step-jog clicks
  then join like XY ones.

### A

* **No machine range on A.** A's position is unbounded. A tool can limit
  how far A may rotate (the knife's cable); the job passes that tool's range
  to the planner.
* **Wraps are an integer count.** A is a bounded heading plus an integer
  wrap count k of the tool's period P = 360°/n. The step target is computed
  absolutely, round((heading + k·P) · steps/deg), in Q32.32 like the
  followers; never shifted by a rounded amount. A period that is not a whole
  number of steps then costs at most half a step per re-pick, without
  accumulating, so no tool is refused for it. No float grows on a long spiral.
* **Heading choice at lifts.** At a lift (contour start, a corner the tool
  lifts at) any equivalent mod P may be chosen. Mid-cut A follows the path.
* **Cable range.** The planner picks the start equivalent that keeps a sweep
  inside the range, and answers whether a stretch's sweep fits. The sweeps
  come from the job: XY-only geometry the host computes from tangents and
  corner turns, sent with every `START` and `BREAK` as two intervals relative
  to the heading there: to the next `BREAK`/`END` (stretch) and to `END`
  (rest of contour). The Pico lifts at a lift point when the rest fits,
  otherwise checks each stretch before its `BREAK` and lifts there when the
  next stretch would leave the range; a single stretch wider than the range
  is refused at ingest. The wire change is the job's, not this plan's.
* **A limits XY speed** (from the seed session):
  * turn rate: v ≤ ω_max / κ_max;
  * turn acceleration: α = a_t·κ + v²·dκ/ds, so v ≤ √(α_frac·α_max / dκ_max)
    and a reduced tangential acceleration per block;
  * a junction limit where curvature jumps between blocks.
  * Units: A's `maxFeed`/`maxAccel` are deg/s, deg/s²; converted to rad
    against κ in 1/mm.

### Blade offset

* **One 2-vector per tool, in the knife's frame:** `d` along the cut (the
  trail) and `e` across it (misalignment, normally 0). It is the tool tip
  offset of coordinate-system.md. The knife axis follows `P + d·T + e·N`.
  Separate from the head offset (fixed, not rotating, already applied by
  `frames`).
* **XY limits are checked at the axis point,** which moves up to
  √(1 + (r·κ)²) faster than the tip on curves, r = √(d² + e²).
* **Reach:** the axis path lies inside the tip bbox grown by r, whatever the
  heading (swivels included). Preflight and the per-record hull check grow by
  r; that is the job's. The planner plans the axis path and needs nothing
  more for reach.

### Mesh

* **Applied to every XY move,** cut and travel, after the executor and before
  the followers (Marlin and Klipper `bed_mesh` likewise; no fade). Lift and
  plunge are pure Z lines to (lift height or work Z) + mesh at that point.
* **Z limits XY speed** too: v ≤ Z `maxFeed` / |slope| along the block, and
  Z's acceleration from the change of slope (block boundaries included).
  A load-time check reports the worst case; on this bed it should never bind.
* **Storage: `/mesh.bin` on LittleFS, apart from the config.** Fixed layout,
  so a probe can rewrite one point in place:

  ```
  magic, version, nx, ny, x0, y0, dx, dy   (machine frame, mm)
  float32 z[nx*ny]                          (mm, row-major)
  crc32
  ```

  Bilinear between points. Missing or a bad CRC means flat, reported by `get`.
  Written only at rest (no flash writes during motion). Filling it by probing
  (the BLTouch head) is a later plan.

## Branches

A dependent chain, one session: 1, 2, then 3 → 3b, and 4 after 2 (beside 3).

## Branch 1: `refactor/planner-axes`

### Plan

* Type: refactor.
* Purpose: `lib/planner` positions and limits from 2 axes to 4, XY behaviour
  unchanged; existing tests pass as they are.
* What the code has today:
  * `Vec2` and `AxisLimits` (`max_feed[2]`, `max_accel[2]`) in
    `lib/planner/planner/path.h:15`, `:20`.
  * `Line` (`planner/line.h:16`), `Bezier::p` (`planner/bezier.h:22`),
    `Block::origin` (`planner/planner.h:52`), `Planner::reset`/`pushLine`/
    `end` (`planner.h:73`, `:79`, `:119`), `Executor::reset`/`tick`/
    `position` (`planner/executor.h:33`, `:41`, `:58`) are all `Vec2`.
  * `makeLine` caps per axis over 2 (`line.cpp:23`); `pathOf` takes
    `min(max_feed[0], max_feed[1])` (`bezier.cpp:194`).
* Scope:
  1. A position type for block ends (`Block::origin`, `Path::end`), the
     executor's output and `reset`/`end`:
     `struct Pos { float x, y, z; float a; Vec2 xy() const; void setXy(Vec2); }`
     (mm; A in deg). Not a vector: its units are mixed and no maths runs
     across all four. `AxisLimits` over 4. XY geometry (Bézier control
     points, tangents, curvature) stays `Vec2`; Z and A pass through
     unchanged.
  2. Callers pass and take the wider types, using X and Y only:
     `core0/planner/queue.cpp` (`:27` `admit`, `:54`, `:182`),
     `core1/emit/follower.cpp:183`, `core0/cmd/lifecycle.cpp:116`,
     `core1/core1.cpp:49`, `core0/core0.cpp:140`.
  3. Tests reach XY through `.xy()` (`test_executor.cpp`, `test_bezier.cpp`);
     what they check is unchanged.
* Out of scope: any Z or A motion.
* Overlap: `lib/planner/`.
* Checks: `pio run -e pico`, `pio test -e native`.

### Status

Done.

### Outcome

* `Path::end` stays `Vec2`: `Path` is geometry (junctions read it). A
  block's Z and A live in `Block::origin`, now set for Béziers too; block
  ends take XY from `Path::end` and Z, A from `origin`.
* `admit` fills limits for X and Y only; Z and A stay 0 until branch 2.
* Found, out of scope: PlatformIO reports doctest suites that pass as
  SKIPPED, because it parses only the per-case blocks doctest prints for
  failures; a case-per-block doctest reporter in `test/main.cpp` would fix
  it. `test/main.cpp:2` still says "motion suite".

## Branch 2: `feature/planner-za`

### Plan

* Type: feature.
* Purpose: lines in Z and A (jog, lift, plunge, rotate), followed on Core 1;
  Z and A jogs move off `JOG_MAGIC`, which is retired.
* Depends on: branch 1.
* What the code has today:
  * The follower seeds and steps slots 0 and 1 only
    (`core1/emit/follower.cpp:123`, `:153`, `:195`); `plannerSpm[2]`
    (`ipc/shared_state.h:365`) is set in `queue.cpp:52`.
  * `JOG_MAGIC` (`core0/data_plane.cpp:149`, `:193`) carries Z and A through
    the MicroSegment ring; the web packs it (`web/src/wire/format/constants.ts:15`).
  * Step jogs (`core0/cmd/axis.cpp:642` `queueJog`) and continuous jogs
    (`data_plane.cpp:123`) are XY; the continuous packet reserves Z and A
    (`web/src/wire/format/cjog.ts:7`).
  * A's `stepsPerUnit` is steps per degree (45.98 in
    `config/controller-1head.jsonc:117`).
* Scope:
  1. Line blocks moving one axis set: XY (either or both), Z alone or A
     alone; a line mixing sets is refused (mm and degrees share no length).
     Junctions as in Decisions, Axes: each `Path` carries its axis set, and
     a Z or A line's direction is `{±1, 0}`, so the junction formula gives
     full speed straight on and a stop on a reversal; different sets stop.
  2. Follower: slots 2 and 3 (the selected head's Z and A), `plannerSpm[4]`,
     seeded from `machinePos`. Z and A limits and steps/unit from the head
     bound to each slot: `frames.cpp:115` `axisFor` exported.
  3. `Pos::a` becomes a heading in [0, 360) plus an integer `turns`; the step
     target is `turns · (360 · spm) + heading · spm` in Q32.32, so A's
     resolution does not fall with turns (a plain float is past a step after
     ~1000 turns). Branch 3 re-picks by whole periods within this form.
  4. Step `jog` and continuous jogs accept Z and A; `jogto` stays XY (J1b
     decides absolute Z and A). Soft range on Z through `framesCheckMove`,
     which covers slot 2; none on A. A CJOG
     packet holding more than one axis set is NACKed `MIXED_AXES` (0x0A);
     moving from one held set to another still brakes and starts anew.
     `JOG_MAGIC` refused, then removed with every web use of it
     (`web/src/operatorJog/`: `makeJog`, `clickJogSource`, `jogClick`,
     `jogTo`, and their tests); Z and A clicks become `jog z|a`.
  5. Docs: `docs/wire_protocol.md`.
* Out of scope: derived Z and A, tools.
* Overlap: `lib/planner/`, `web/src/wire/`, `src/rp2350/core0/cmd/table.h`.
* Checks: `pio run -e pico`, `pio test -e native`, `pnpm typecheck`,
  `pnpm test`. Human: Z and A step and continuous jogs on each head; a Z
  soft-limit refusal; `nodestat` deltas match the Pico.

### Status

Not started.

## Branch 3: `feature/planner-tangential`

### Plan

* Type: feature.
* Purpose: A follows the path heading on Béziers and XY lines when the tool
  is tangential; A's speed and acceleration limits; heading choice at lifts
  with period and cable range.
* Depends on: branch 2.
* Scope:
  1. Executor: A from the XY tangent, continuous across blocks of a stretch.
  2. Limits (Decisions, A): turn rate, turn acceleration and the reduced
     tangential acceleration per block, the curvature-jump junction limit.
  3. Heading choice: the equivalent mod P for a target heading; the start
     equivalent that fits a sweep in a range; whether a stretch fits.
  4. Inputs per push: tangential flag, period n, optional range. Decoding
     them from `tools` is the job's.
* Out of scope: blade offset (3b), the sweep's wire fields (jobs).
* Overlap: `lib/planner/`.
* Checks: as branch 2. Human: a test cut with offset 0 tracking curves and
  corners; a cabled range never exceeded.

### Status

Not started.

## Branch 3b: `feature/planner-blade-offset`

### Plan

* Type: feature.
* Purpose: the knife axis follows `P + d·T + e·N`; XY limits at the axis
  point.
* Depends on: branch 3.
* Scope:
  1. The axis point from the tip path and heading; swivels about the tip at
     corners.
  2. XY speed and acceleration checked at the axis point.
  3. Input per push: `(d, e)`.
* Out of scope: reach (the job's preflight grows the bbox by r).
* Checks: as branch 2. Human: opposite-direction slits coincide.

### Status

Not started.

## Branch 4: `feature/planner-mesh`

### Plan

* Type: feature.
* Purpose: Z from the mesh under every XY move, with Z's speed and
  acceleration limits; `/mesh.bin` read at boot.
* Depends on: branch 2.
* Scope:
  1. Mesh added between executor and followers; lift and plunge targets
     include it.
  2. Z limits along XY blocks; the load-time worst-case report.
  3. `/mesh.bin` decode, flat when missing or bad; `get` reports it.
  4. Web: a `mesh.bin` encoder (for bench meshes until probing exists).
* Out of scope: probing the mesh.
* Overlap: `web/src/wire/`.
* Checks: as branch 2. Human: a tilted test mesh followed on a line and a
  curve.

### Status

Not started.

## Open

* Tangential on XY travel lines: A follows, or rotates once at the lift?
* Mesh: the grid's extent and spacing defaults; whether the upload goes
  through the config file transfer.
