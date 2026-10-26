# A in jobs

A in a job: tangential tools steer along the path, corners lift and swivel,
travel turns A to the next start, the cable range holds, and a revolver
selects its chamber. Gathers the A decisions from docs/plans/planner-za.md
(A, Blade offset, branches 3 and 3b), docs/plans/job-z.md (corner and cable
rows of What lifts) and docs/plans/motion-sessions.md; this doc is the
reference for them from now on. Builds on job-z's lifts, plunges, duty stops
and its hold-and-insert machinery (ZC2).

## Decisions

### A's position

* **Unbounded in machine terms.** `Pos` holds a heading in [0, 360) plus whole
  `turns` (`lib/planner/planner/path.h:303`); the follower targets steps
  absolutely from both (`core1/emit/follower.cpp:413`), so no float grows on
  a long spiral. MPos A is the unbounded count; folding is display only
  (which readout folds: settle in AA).
* **Homed A is 0** and the cable is untwisted there.

### How A is tracked

```
wire (X,Y curves) → Core 0 queue    → planner ring  → executor        → follower → motor
       ↓                 ↓                 ↓              ↓                  ↓
     no A          picks the start    start & end    follows the       turns that
                   A (nearest         A per block    heading per tick  position into
                   equivalent)                       (linear on a      steps, counts
                                                     travel line)      them → readout
```

* The readout is the follower's step count (`framesMPos`,
  `core0/ops/frames.cpp`): MPos unfolded, WPos folded to (−180, 180]. The
  planner and executor are bookkeeping ahead of the motor; the block-end
  check keeps them agreeing.

### Units

| layer | units |
|---|---|
| config, wire, planner storage, readout | mm and degrees |
| inside the cap math (`bezier.cpp`, `planner.cpp`) | radians, converted on entry, never stored |
| follower and `frames` | steps, via `stepsPerUnit` |

### A's limits, in words

* **Turn rate.** On a tight curve A spins fast to keep up: tighter curve (κ)
  × faster cut = faster spin. A's top speed slows the cut on tight curves:
  v ≤ ω_max/κ.
* **Turn acceleration.** Where the curve tightens along its length, A must
  speed up its spin, which takes acceleration A's motor has a limit on, so
  the cut slows there too (the dκ cap); part of the XY acceleration is held
  back for it (the reduced a_t).
* **Curvature jump at a join.** Where two curves meet smoothly but one is
  tighter, A would have to change spin speed instantly. It lags and the
  blade points briefly off the path; the cut slows at the join until that
  lag stays under the misalignment it tolerates (e_max, start 2°, tuned on
  the bench by cutting gentle-to-tight joins at a few speeds).
* **Travel line.** A turns during the travel; on a short hop with a big
  turn A is the slow one and the travel slows to let it finish.
* **Units.** Motor limits are in degrees, curvature is per mm; degrees are
  converted to radians so the formulas give correct numbers.

### Tool fields (config, decoded by the Pico)

| field | meaning | today |
|---|---|---|
| `tangential` | A follows the path heading | decoded |
| `periodCount` | n: the tool looks the same every 360/n° (knife 1, crease 2); tangential only, default 1 | new |
| `cornerAngleDeg` | a BREAK turning more than this lifts and swivels | schema only |
| `offsetMm` | blade trail `d` along the cut (AD) | schema only |
| `aMaxDeg` | cable: \|A\| ≤ `aMaxDeg` either way from home; absent = no cable | new |
| `chamberOffsets` | slot angles of a revolver; not tangential | decoded |

* Resolved with the layer header into `LayerMotion` (`core0/ops/layer.h`),
  which already checks the tool is mounted; mount only confirms the tool and
  applies its offset. Blob and schema version bump. `periodCount` with `tangential: false`, or `chamberOffsets`
  with `tangential: true`, is a load error.
* `unwind` and `minRadiusMm` are read by nothing; the web-only cleanup
  (task_8236c64d, not started) removes them. `unwind` is superseded by
  "has an `aMaxDeg`".
* Not decided here: `e` (blade misalignment across the cut, planner-za Blade
  offset); 0 until a tool needs it.

### Equivalents

* A target heading h may be reached at any h + k·P, P = 360/`periodCount`
  (360 for a chamber angle). Chosen only where the blade is up: a contour's
  START (during travel), a corner swivel, an unwind. Mid-cut A follows the
  path continuously.
* The choice: the equivalent whose stretch sweep fits `[−aMaxDeg, aMaxDeg]`,
  centred (AE); before AE, or without a cable, the one nearest the current A.

### A rides along XY

* The planner refuses a move mixing axis sets (`axesOf` → `AXES_MIXED`,
  `line.cpp`); A stays fixed on XY blocks (`executor.cpp` `pointAt`). New: an
  XY block carries A as a function of distance along it, linear on a line
  (travel turn), the unwrapped tangent on a Bézier (tangential cut). The XY
  path stays the geometry; A's limits cap it as a `PathCap`, as the mesh
  caps Z.
* A's caps on a Bézier (planner-za, A): turn rate v ≤ ω_max / κ_max; turn
  acceleration α = a_t·κ + v²·dκ/ds, so v ≤ √(α_frac·α_max / dκ_max) and a
  reduced tangential acceleration; a junction limit where κ jumps between
  blocks of a stretch. A's `maxFeed`/`maxAccel` are deg/s, deg/s²,
  converted to rad against κ in 1/mm. On a travel line: v ≤ A's maxFeed ·
  length / |ΔA|, and the same for accel; the line takes max(XY, A) time.
* A Bézier can turn past 180°: the heading is unwrapped tick to tick (the
  executor's `pointAt` is stateless today).
* Lift, swivel and plunge stay separate axis sets (Z, A, Z), which already
  stop between them.

### Where A turns

| moment | A does | branch |
|---|---|---|
| contour START | A-only swivel to the start heading's equivalent, after the travel, before the plunge | AA |
| contour START | the turn moves into the travel line (Z, XY+A, Z) | AB |
| BREAK past `cornerAngleDeg`, tangential tool | lift, A-only swivel to the next heading's equivalent, plunge | AA |
| BREAK under `cornerAngleDeg` with a heading jump | A-only swivel in the cut, XY at rest | AA |
| along a curve | A follows the tangent | AB |
| contour END | lift; A stays; the next START turns it | AA |
| layer with a chamber | the travel turns A to `chamberOffsets[k]`'s nearest equivalent and holds it | AC |
| stretch wider than the range | hold, lift, unwind by whole periods, plunge at the same heading | AF |

* A returning to 0 at the end of a job: no job-end record exists; left to
  the unwind work (AE/AF) to decide.
* A non-tangential tool without chambers never moves A in a job.

### Sweep on the wire

* Every record carries the unwrapped heading range of the rest of its
  stretch relative to its own start heading, `sweepMin`/`sweepMax` (i16 whole
  degrees, saturating, rounded outward), next to `nextBreakDist`; reverses planner-za's "no
  sweeps on the wire". The host computes the same tangent and unwrap as the
  Pico; the range check keeps a margin for rounding.
* At every point where the blade is up the Pico picks the equivalent that
  fits the coming stretch, centred, so unwinding folds into turns the tool
  makes anyway. Ignored for non-tangential tools.
* A resumed record's sweep is shifted by the trimmed part's turn, which the
  Pico computes; it still holds the trimmed part's swing, so the check stays
  conservative.

### Mid-cut unwind

* Only a stretch whose own sweep exceeds `2·aMaxDeg` needs it. Core 0 knows
  at the stretch's start (sweep) and Core 1 watches A against the range live
  (the fallback), as ZC2 does for duty: hold, lift, A-only unwind by whole
  periods to the equivalent nearest 0, plunge at the same XY and heading,
  continue from `s`. Reuses `Executor::release`, `beginFront`/`endFront`,
  `Block::resume_s`.
* Settle in AF: where Core 0 places a planned unwind inside a long stretch,
  and how it shares the stop with a duty lift.

### Blade offset

* One 2-vector per tool in the knife's frame: `d` (`offsetMm`) along the cut,
  `e` across it. The knife axis follows `P + d·T + e·N`; corners swivel about
  the tip. Separate from the head and tool offsets (fixed, applied by
  `frames`).
* XY limits are checked at the axis point, which moves up to √(1 + (r·κ)²)
  faster than the tip, r = √(d² + e²).
* Reach: the axis path lies in the tip bbox grown by r; preflight and the
  per-record checks grow by r.

### Out of range

* A has no machine range; `aMaxDeg` is the only A limit. A stretch that can
  never fit and has no unwind available (before AF) is refused at its record
  (`PQ_SOFT_LIMIT`, the record's NACK).

## Branches

A dependent chain, run as a Single Session: AA → AB → AA2 → AC → AD → AE → AF.

### AA: swivels

* Name: `feature/job-a-swivels`
* Purpose: a tangential tool turns A where its heading jumps, with A fixed
  between: an A-only swivel to the curve's start heading at every START
  (after the travel, before the plunge); at every BREAK a swivel to the next
  heading, lifted past `cornerAngleDeg`, in the cut below it. Equivalents mod
  P, nearest the current A. Polygons cut true; curves wait for AB.
* No `lib/planner` change: an A-only `pushMove` exists.
* The tool fields decoded and resolved into `LayerMotion`; a tangential tool
  on a head without an A axis refuses the layer header (`NACK_JOB`).
* Files: `core0/config/config_decode.{h,cpp}`, `core0/ops/layer.{h,cpp}`,
  `core0/planner/queue.cpp` (`plannerQueueRecord`, ring room);
  `web/src/machine/schema.ts`, `defaults.ts`, `json/load.ts`,
  `json/validate.ts`, `json/blob.ts`; `config/controller.jsonc`; fixtures;
  `docs/wire_protocol.md` if a NACK changes.
* Depends on: job-z (merged).
* Status: Done (merged 9226d06). Unblocks AB.
* Outcome:
  * Deviations: the swivel target is the equivalent nearest the current A,
    and the lift test is that swivel's size, not the drawn corner angle, so
    a crease reversal (period 180) does not lift. A swivel no equivalent
    inside `aMaxDeg` reaches is the record's `NACK_SOFT_LIMIT`. A BREAK
    with a duty stop swivels inside the duty lift. `plannerQueueRecord`
    takes the BREAK flag. The picker is inline in `queue.cpp`, untested
    natively like the rest of it.
  * Settled in Work: a swivel runs at A's `maxFeed`; a corner lift adds its
    lift and plunge to Core 0's on-time count (the tool stays on).
  * Interface: tool fields `periodCount` (required in the blob, default 1,
    crease 2), `aMaxDeg` (optional; knife 180), `cornerAngleDeg` now
    decoded; `LayerMotion` carries `tangential`, `periodDeg`,
    `cornerAngleDeg`, `aMaxDeg`. Blob and schema version 10.
  * Bench (knife on head 0, no material, `aMaxDeg` 540): a square swivels
    after the travel and lifts at every corner; two squares in one job
    climb to 495 and unwind to 225 at the corner where 585 would pass;
    a 24-gon swivels in the cut at its 15° corners, lifting once where the
    cable forced a 345° unwind (the case AE removes); a pause in a corner
    lift resumes from its record with the swivel. Not run: the crease, a
    powered tool through a corner lift.
  * Out of scope: `test_machine` still fails to link, as on main.

### AB: tangential following

* Name: `feature/planner-tangential`
* Purpose: A follows the unwrapped tangent along a Bézier (and is linear
  along a travel line); A's caps; the START turn moves into the travel line.
  A BREAK under `cornerAngleDeg` keeps AA's swivel.
* Files: `lib/planner/` (`path.h`, `line.cpp`, `bezier.cpp`, `planner.cpp`,
  `executor.cpp`) and native tests; `core0/planner/queue.cpp`.
* Depends on: AA.
* A per block: fixed (non-tangential, AC's chamber hold), linear (travel
  line), or tangent + k·P. `axesOf` allows XY+A only for these.
* Unwrap: the planner sums each Bézier's unwrapped turn while sampling κ
  (asserting κ·Δs ≪ 180° per sample) and stores `origin`/`target` A; the
  executor tracks A tick to tick (wrapped heading delta added to the last
  A), holding the previous heading where |B′(t)| is below a threshold. At
  block end a tracked-vs-`target` difference over a fraction of a degree
  is a fault, not a silent snap. A resumed block's `origin` A is the ring's
  actual A (the equivalent picked from it), never the bare heading.
* Per-block caps as in "A rides along XY": ω, dκ_max and the reduced a_t,
  with A's deg/s, deg/s² converted to rad first.
* Curvature jump at a join: v ≤ Δω_max / |Δκ| from `kappa_end`/`kappa_start`,
  folded into `max_entry_sqr`, with Δω_max = √(2·α_max·e_max) and e_max the
  blade misalignment the cut tolerates (planner constant, start 2°; bench
  tunes it). Lines count as κ = 0.
* Travel line: v ≤ ω_max·L/|ΔA| and a ≤ α_max·L/|ΔA|.
* Units: config, wire, planner storage and readout in mm and degrees;
  radians only inside the cap math, converted on entry, never stored; steps
  only in the follower and `frames`. New angle variables carry `_deg`/`_rad`.
* Fold readout: settled in AA (`WPos` folds, `MPos` does not).
* Status: Done (merged 67a93b3). Unblocks AC.
* Outcome:
  * Deviations: `pickA` picks the equivalent whose whole curve (the record's
    `bezierTurn` lo/hi) fits ±`aMaxDeg`, not only its start; a record no
    equivalent fits is `NACK_SOFT_LIMIT`. Per record only; the stretch is
    AE's. The turn sum lives in `bezierTurn` (the Pico takes host analysis
    through `checkBezier`, which samples nothing), sampled outside the
    planner lock with n = ⌈κ_max·L/(π/4)⌉, 4..1024.
  * Interface: `axesOf` gives XY for XY+A; `Path` carries `a_rate_start/end`
    and `a_jump`; `aRides()`; `pushBezierFrom(…, const BezierTurn* turn)`
    (a turn makes the block tangent, `Block::tangent`);
    `pathOf(Bezier, …, tangential)`; `bezierHeading`, `bezierTurn`;
    `Executor::aMisses()`, `get amiss`. Constants `kAMisalignRad` (2°),
    `kAAccelShare` (0.5), `kAMissDeg` (0.5°).
  * Settled in Work: the executor follows from its own `pos_` and the
    heading at the block's `s0`, so an in-place restart and a job resume both
    continue from the actual A; a travel line's A is linear over
    `s0 + length`.
  * Bench (knife on head 0, no material, `aMaxDeg` 540): a circle runs A
    0 → 360 with the heading; a clockwise circle from 360 starts at 180 and
    tracks the heading (176.8° at the expected point); an S turns −180 → 0
    during its travel, then 0 → 176 → 0; two laps cut to 540 and the next
    record is refused; a pause mid-circle lifts at 158°, resumes 2 mm back
    turning A during the travel, and ends at 360. `amiss` and `late` stayed
    0. Not run: the crease, a powered tool, a cut in material, e_max tuning.
  * Out of scope: a record refused mid-contour drains the ring with the
    blade down (Z at cut depth); worth a lift on refusal (AF, or its own
    fix). `test_machine` still fails to link, as on main.

### AA2: A's zero at the lock

* Name: `feature/job-a-zero`
* Purpose: machine A = 0 is the angle where the holder's lock button latches
  (46.7° CCW of the hall index on head 0, found by jogging), so a tool can
  only be mounted or unmounted at A = 0. A tool's edge direction there is a
  tool fact, `aOffsetDeg`.
* Head: `indexPos: -46.7` (the index's coordinate; the lock is then 0). No
  code: `home.cpp` already numbers the index from it.
* Tool: `aOffsetDeg`, the edge's heading at machine A = 0 (+X 0, +Y 90, −X
  180, −Y −90; the crease 0 or 90). Optional, default 0; tangential tools
  only. The mechanical team expects finished tools along ±X or ±Y; the
  field takes any angle.
* Applied:
  * `queue.cpp`: heading h → machine A = h − `aOffsetDeg` before `pickA`.
    The planner, executor and follower stay in machine A.
  * `frames.cpp` `framesWPos`: WPos A = MPos A + the mounted tool's
    `aOffsetDeg`, folded (the edge heading, as WPos XY is the tip).
  * The park: `lineTo` (`controller/seq/steps.cpp`) takes A to machine 0
    riding the line (XY+A; A-only if XY is already there) when asked.
    `probe_park` (`seq/jogto`) and the job's tool change
    (`seq/toolchange.cpp`) ask; plain `jogto` does not. A head without A
    parks XY only; A present but unhomed fails `a_not_homed`.
* `aMaxDeg` and AC's `chamberOffsets` count from machine 0, the lock.
* Not here: homing ending at the lock; a `tool mount`/`unmount` command
  family (its own plan).
* Files: `config/controller.jsonc`; `core0/config/config_decode.{h,cpp}`,
  `core0/ops/layer.{h,cpp}`, `core0/ops/frames.cpp`,
  `core0/planner/queue.{h,cpp}`, `core0/controller/seq/{steps,jogto,toolchange}.{h,cpp}`,
  `core0/controller/cmd/jogto.cpp`;
  `web/src/machine/schema.ts`, `defaults.ts`, `json/load.ts`,
  `json/validate.ts`, `json/blob.ts`; fixtures; `docs/wire_protocol.md`
  (WPos A), docs/plans/coordinate-system.md (A's zero).
* Depends on: AB.
* Bench (Human scope): home, jog A to 0, the button latches; with the knife
  mounted and `aOffsetDeg` set, A at WPos 0 points the edge along X+, at 90
  along Y+ (also checks +A is CCW, `invertDir`).
* Status: Done (merged dad85e3). Unblocks AC.
* Outcome:
  * Interface: `lineTo(x, y, feed, queued, aLock)`, `aLockReady()`
    (`seq/steps.h`); `plannerQueueLine(…, aZero)`; `jogtoStart(mx, my,
    aLock)`. `CFG_SCHEMA_VERSION` and `CONFIG_BLOB_VERSION` 11.
  * `web/src/machine/defaults.ts` untouched: the field is optional.
  * Bench: the button latches at A = 0 after homing; `probe_park 1` from
    WPos 100, 100, A 100 turns A to 0 along the XY line, ending together
    (`amiss` 0). Run with a temporary `probeSwitch` (200, 200), since
    removed.
  * Open for the user: head 0 has no `probeSwitch`, so `probe_park` and a
    job's tool change answer `no_position`; the knife's `aOffsetDeg` is 0
    until measured on mount.
  * Out of scope: homing ending at the lock; the `tool mount` plan.
    `test_machine` still fails to link, as on main.

### AC: revolver chamber

* Name: `feature/job-a-chamber`
* Type: feature.
* Purpose: a layer with a chamber turns A to `chamberOffsets[k−1]` (nearest
  equivalent, within `aMaxDeg`) during the START travel and holds it.
* Offsets: absolute per chamber, from the tool's latch key, which sits at
  machine 0 (the lock) when mounted; any finite value (375 = 15). No
  `aOffsetDeg` on non-tangential tools: one way to state each angle, and
  uneven spacing needs the list anyway.
* Changes:
  * `ops/layer.{h,cpp}`: `LayerMotion` carries the chamber angle;
    `layerResolve` fills it, and refuses a chamber tool on a head without A
    (`LAYER_BAD`, as tangential).
  * `planner/queue.cpp` `plannerQueueBezier`: on a START of a chamber layer,
    `aTo` from `pickA(cur, chamberDeg, 360, aMaxDeg, BezierTurn{})`; the
    `withA` travel turns it. BREAKs leave A. No fit → `PQ_SOFT_LIMIT`; no A
    limits → `PQ_NO_LIMITS`. Chamber changes and resumes ride the next START.
    A record of a chamber or tangential layer with A unhomed → `PQ_BAD_STATE`.
  * `test/test_config/test_layer.cpp`: the angle picked, a head without A.
  * `docs/wire_protocol.md`: the chamber turns A; the refusals.
* Bench: chamber switches across layers, the nearest-equivalent wrap, resume.
* Files: `core0/ops/layer.{h,cpp}`, `core0/planner/queue.cpp`,
  `test/test_config/test_layer.cpp`, `docs/wire_protocol.md`.
* Depends on: AA2.
* Status: Done (merged d17dc4b). Unblocks AD.
* Outcome:
  * Deviations: the unhomed-A refusal (`PQ_BAD_STATE`, `NACK_BAD_STATE`),
    added in Work, covers tangential layers too.
  * Interface: `LayerMotion::chamber`, `chamberDeg`. A chamber layer also
    needs A limits (`PQ_NO_LIMITS`), as tangential.
  * Bench (pen with `chamberOffsets` [15, 135, 375], `aMaxDeg` 180, head 0,
    no material; the config only for the run): unhomed A refused; layers on
    chambers 1, 2, 3, 1 turn A during the travel to 15.005, 135.001, 15.005
    (375 folded inside the cable), 15.005, held through each square; paused
    mid-layer on chamber 2 and resumed with a chamber 1 header, the START
    turns 135 → 15 at clearance before the plunge. `late` and `amiss` 0.
    Not run: a chamber no equivalent fits (`NACK_SOFT_LIMIT`).
  * Out of scope: A can't be jogged while a job is paused (`err bad_state`);
    a resume header may name a chamber other than the paused one, matched
    only by layer and record. `test_machine` still fails to link, as on main.

### AD: blade offset

* Name: `feature/planner-blade-offset`
* Type: feature.
* Purpose: the tip of a trailing blade lands on the sent path: the knife
  axis runs `d` (`offsetMm`) ahead of the tip along the blade's heading.
  `e` stays out (0) until a tool needs it.
* The offset is a pose conversion keyed to A, applied whenever a tool with
  `offsetMm` ≠ 0 is mounted (jobs, jogs, readout alike):
  axis = tip + d·(cos h, sin h), h = A + `aOffsetDeg`. The planner, ring and
  executor stay in tip coordinates; the conversion sits where the
  executor's position becomes steps, and its inverse where the ring resets
  from the motors (`queue.cpp` `resetIfIdle`). Curves, travel turns and
  swivels (about the tip) need no path of their own. WPos XY is the tip;
  jogging A swings XY about it.
* Z mesh under the tip: Core 1's live sample takes the tip (before the
  conversion), as the push-time walk already does.
* XY limits at the axis point: a curve caps v by 1 + d·κ_max; an A-only
  swivel caps ω so d·ω stays within XY's feed and accel; a travel line that
  turns A adds both.
* Reach: the per-record check (`data_plane.cpp` `framesCheckXY` on the
  control points) uses the soft range shrunk by d.
* Config: `offsetMm` decoded into `CfgTool`; ≠ 0 on a non-tangential tool
  is a load error (as `periodCount`). Blob and schema version 12.
* Host: `needsOffsetComp` and its tolerance removed; the host sends the tip
  path.
* Files: `lib/planner/` (caps) and native tests; `core0/config/config_decode.{h,cpp}`,
  `core0/ops/frames.cpp`, `core0/planner/queue.cpp`, `core0/data_plane.cpp`,
  the Core 1 step/mesh path; `web/src/machine/schema.ts`, `json/validate.ts`,
  `json/blob.ts`, `tools.ts`; fixtures; `docs/wire_protocol.md`.
* Bench (Human scope): opposite-direction slits coincide; a square's corners
  come out sharp; A jog holds the tip still.
* Depends on: AC.
* Status: Done (merged a267c12). Unblocks AE.
* Outcome:
  * Deviations: A turn from a tip outside the shrunk range is refused (A
    jog `err soft_limit x|y`, cjog and a job START's travel
    `NACK_SOFT_LIMIT`, the lock park `soft_limit`); an XY move from outside
    may hold or head in (`framesCheckMove`), else a knife mounted at the
    edge could not move. Added in Work.
  * Interface: `CfgTool::offsetMm`; `Planner::setBlade`, `bladeCaps`
    (`path.h`); `PlannerBlade plannerBlade`, `bladeLead` (`shared_state.h`);
    `framesBlade`, `framesTipMPos`, `framesTipRange`. `CFG_SCHEMA_VERSION`
    and `CONFIG_BLOB_VERSION` 12.
  * Settled in Work: the extra acceleration at the axis on curves and
    travel turns is covered by the 1 + d·rate slowdown, not checked exactly.
  * Bench (knife on head 0, `offsetMm` 10 for the run only, no material):
    mounting moves WPos by the lead at A (−4.506, +8.927 at −63.2°); A
    jogs 0 → 90 → 180 → 270 → 0 trace a 10 mm circle in MPos with WPos
    held to 0.005 mm; at the homed edge A jogs are refused until X and Y
    are jogged in, then accepted; `jog y-45` with 40 mm left is refused.
    Not run: opposite slits, sharp corners, a cut in material, the job
    START and park refusals.
  * Settled in Work: mounting or unmounting a knife moves WPos by the lead
    (the tip is a different point), as a `toolOffset` does; nothing moves.
  * Out of scope: `test_machine` still fails to link, as on main (its stubs
    lack `dutyEpoch` and `plannerQueueLift`).

### AE: sweep on the wire

* Name: `feature/job-a-sweep`
* Purpose: `sweepMin`/`sweepMax` per record; the equivalent that fits the
  range, chosen at every START and corner.
* Today `pickA` (`queue.cpp:459`) fits only the record's own turn, so a
  stretch of several records can be refused mid-stretch from an equivalent
  another choice would have fitted.
* Decided in Planning:
  * Every record carries the unwrapped heading range of the rest of its
    stretch (its p0 to the next BREAK or END), relative to its own p0
    heading. A trimmed resume takes off the trimmed part's turn, which the
    Pico computes itself; the bound still holds the trimmed part's own swing,
    so it is safe, a little wide.
  * i16 whole degrees, saturating, rounded outward (floor min, ceil max).
    The Pico grows the range by 0.1° each side for host/Pico float drift.
  * The pick: among the equivalents whose whole sweep fits ±`aMaxDeg`, the
    one putting the sweep's midpoint nearest A = 0; ties go to the nearest
    current A.
  * A sweep wider than 2·`aMaxDeg` is refused at its START or BREAK
    (`PQ_SOFT_LIMIT`) until AF.
  * The host's per-piece turn (lo/hi/total) matches the Pico's `bezierTurn`
    (`bezier.h:61`); the sum runs in `annotate.ts`'s stretch walk next to
    `nextBreakDist`.
* Interface: the BEZIER record grows 4 bytes (58 → 62): `[57..58]`
  `sweepMin`, `[59..60]` `sweepMax`, CRC at `[61]`. `plannerQueueRecord`
  takes the sweep.
* Type: feature.
* Files: `core0/usb_protocol.h`, `data_plane.cpp`, `queue.cpp`;
  `web/src/wire/format/`, the host's flatten (heading unwrap);
  `docs/wire_protocol.md`.
* Depends on: AD. A protocol change: merges before AF.

### AF: mid-cut unwind

* Name: `feature/job-a-unwind`
* Purpose: a stretch wider than the range unwinds mid-cut: planned by Core 0
  from the sweep, Core 1's live range watch as the fallback.
* Files: `lib/planner/` if needed; `core0/planner/queue.cpp`;
  `core1/emit/follower.cpp`; `ipc/core1_facts.h`, `ipc/shared_state.h`.
* Depends on: AE.

## Open

* Bench: A backlash after an unwind; how clean a re-plunge into the slit is.
* A back to 0 at the end of a job (no job-end record).
