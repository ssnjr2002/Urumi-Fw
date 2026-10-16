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

* Each START or BREAK record carries its stretch's unwrapped heading range
  relative to the stretch's start heading, `sweepMin`/`sweepMax` (i16 whole
  degrees, saturating), next to `nextBreakDist`; reverses planner-za's "no
  sweeps on the wire". The host computes the same tangent and unwrap as the
  Pico; the range check keeps a margin for rounding.
* At every point where the blade is up the Pico picks the equivalent that
  fits the coming stretch, centred, so unwinding folds into turns the tool
  makes anyway. Ignored for non-tangential tools.
* A resumed record keeps the record's sweep: the resume heading lies inside
  it, so the check stays conservative.

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

A dependent chain, run as a Single Session: AA → AB → AC → AD → AE → AF.

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
* Open for its Work: the curvature-jump bound on A's instant speed change;
  which readout folds A; the unwrap stored per block or kept by the executor.

### AC: revolver chamber

* Name: `feature/job-a-chamber`
* Purpose: a layer with a chamber turns A to `chamberOffsets[k]` (nearest
  equivalent, within `aMaxDeg`) during the START travel and holds it.
* Files: `core0/ops/layer.{h,cpp}`, `core0/planner/queue.cpp`.
* Depends on: AB.

### AD: blade offset

* Name: `feature/planner-blade-offset`
* Purpose: the knife axis follows `P + d·T + e·N`; XY limits at the axis
  point; preflight grows by r.
* Files: `lib/planner/`, `core0/planner/queue.cpp`, preflight.
* Depends on: AC.

### AE: sweep on the wire

* Name: `feature/job-a-sweep`
* Purpose: `sweepMin`/`sweepMax` per START/BREAK record; the equivalent that
  fits the range, chosen at every START and corner.
* Interface: the BEZIER record grows 4 bytes.
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
