# pipeline/

**NOTE:** This document is stale as of 02/07/26 20:40

SVG-to-MicroSegment motion planning pipeline. Pure Python, no external dependencies. Takes an SVG file and produces a flat list of `MicroSegment` step events ready for serialisation and transmission to the RP2350.

The pipeline plans velocity **per arc-length sample**, not per Bézier curve. An earlier per-curve ("tile") engine was retired once the per-sample engine passed on hardware — the sample engine subsumes it (lowering `a_max` reproduces tile's whole-arc conservatism) and maintaining two planners was pure tax. See [`PLAN_pipeline_redesign.md`](../PLAN_pipeline_redesign.md) for the tile-vs-sample rationale.

## Stages

The representation is **lowered exactly one level per stage** — curves → mm-curves → repaired-curves → samples → constrained-samples → planned-samples → step-events. After Flatten, the unit is the `Sample` (a point + tangent + local curvature + step-to-next), and nothing downstream re-derives curvature from the Béziers.

| # | File | Input | Output |
|---|---|---|---|
| 1 Parse | `stages/stage1.py` | SVG file | `list[CubicBezier]` — paths as cubic Béziers, SVG pixels |
| 2 Normalize | `stages/stage2.py` | curves + viewport | same curves in **mm**, Y-flipped (machine origin bottom-left) |
| 3 Repair | `stages/stage3.py` | mm curves | curves with C1 continuity enforced at joins |
| 4 Flatten | `stages/flatten.py` | repaired curves | `list[Sample]` — arc-length sample stream, **per-sample local κ** |
| 5 Constrain | `stages/constrain.py` | samples | samples + `v_ceiling[i]` — local speed cap per sample |
| 6 Plan | `stages/plan_lookahead.py` | samples + ceilings | samples + `v[i]` — look-ahead resolved, accel-continuous |
| 7+8 Discretize | `stages/discretize.py` | planned samples | `list[MicroSegment]` — step deltas (dx,dy,dz,da) + intervals + choreography |
| 9 Serialise | `../host/serialise.py` | MicroSegments | 26-byte wire packets (magic `0xAB`) |

Supporting modules (the spine and the shared primitives):

| File | Role |
|---|---|
| `stages/sample.py` | the `Sample` dataclass + `PATH_START`/`PATH_END`/`CURVE_BOUNDARY` flags — the currency of stages 4–8 |
| `stages/bezier.py` | cubic Bézier primitives: point, 1st/2nd derivative, arc length (GL5), curvature |
| `stages/microsegment.py` | the `MicroSegment` wire type, `MICRO_*` flags, per-axis `interval`, `angle_delta` |

Stages 4–8 are the ground-truth Python reference for a future C++ port on the RP2350 (local-production mode); the host/firmware handoff stays between stages 3 and 4 — repaired Béziers go over the wire, the firmware flattens and plans from there.

## Configuration

All tuning lives in `stages/config.py`. Three tiers:

| Tier | Class | Governs |
|---|---|---|
| `machine` | `MachineConfig` / `AxisConfig` | steps/unit, axis→node map, invert, **per-axis** `max_rate` / `accel`, Z/A resolution |
| `motion`  | `MotionConfig` | `feed_max`, `a_max` (lateral/centripetal accel + scalar fallback), `jog_feed`, `junction_deviation`, `lift_height`, `z_feed` |
| `quality` | `QualityConfig` | `chord_tol`, `ds_max`, `dtheta_max`, `dv_max`, `v_min`, dt limits, `angle_tol`, `gap_tol` |

`config.default()` returns the current physical machine (DM542/TMC drivers, GT2 20T pulley, X/Y = 160 steps/mm, Z = 1200 steps/mm, A = 51.667 steps/deg @ 1/16 micro-step, `x/z/a.invert=True`).

Each stage CLI sources its `argparse` defaults from `config.default()` and accepts per-flag overrides, so every stage is independently runnable.

## Running a stage standalone

Each stage is executable as a script taking an SVG file as the first argument; later stages run all earlier ones internally:

```sh
python stages/stage1.py          data/test.svg   # SVG → cubic Béziers
python stages/stage2.py          data/test.svg   # → mm coords, Y-flipped
python stages/stage3.py          data/test.svg   # → C1-repaired
python stages/flatten.py         data/test.svg   # → Sample stream (count, length, κ range)
python stages/constrain.py       data/test.svg   # → per-sample v_ceiling + corner-stop count
python stages/plan_lookahead.py  data/test.svg   # → resolved v + est. cut time
python stages/discretize.py      data/test.svg --tool knife   # → MicroSegments
```

Full production run (SVG → wire packets) is driven from `../host/svg_to_packets.py`.

## Stage detail

### 1 — Parse (`stage1.py`)
Handles `<path>`, `<circle>`, `<ellipse>`, `<rect>` (incl. rounded), `<line>`, `<polygon>`, `<polyline>`. Path commands M, L, H, V, C, S, Q, Z, absolute and relative. Lines/quadratics become degenerate cubics. `fill:none; stroke:none` elements are skipped. Returns subpath-aware `list[list[CubicBezier]]`.

### 2 — Normalize (`stage2.py`)
Reads `viewBox` + `width`/`height`, resolves units (mm/cm/in/pt/px), applies viewBox offset, scales to mm, flips Y so machine +Y is up.

### 3 — Repair (`stage3.py`)
At each join checks C0 (endpoints meet) and G1 (tangents parallel): gaps > `gap_tol` get a bridging cubic; angle > `angle_tol` is logged as a cusp and **left sharp** so the planner handles it (corner-stop / lift-pivot). This is the last stage where the Bézier is the unit.

### 4 — Flatten (`flatten.py`)
Walks each curve at an adaptive `dt` bounded by three geometry-only caps: chord deviation (`chord_tol`), facet length (`ds_max`), and — crucial for a tangential knife — **tangent change per sample** (`dtheta_max`, ≈2°). Emits a flat `Sample` stream carrying position, tangent θ, **local** curvature κ, and ds-to-next. Curve boundaries are kept as adjacent samples (ds≈0); a sharp corner appears as two samples with the same position but a large θ jump — the corner signal read downstream. This early flatten is what makes per-curve `kappa_max` conservatism structurally impossible.

### 5 — Constrain (`constrain.py`)
Pure per-sample ceiling, no propagation: `v_ceiling = min(feed_max, sqrt(a_max/κ), rad(a_rate)/κ, junction-deviation, 0-at-corner)`. Local κ means a degenerate curvature spike caps one sample, not a whole curve.

### 6 — Plan (`plan_lookahead.py`)
The look-ahead. Backward decel sweep + forward accel sweep over each subpath's samples resolve `v[i]` from the ceilings, **acceleration-continuous by construction** (junctions are no longer planning boundaries). Acceleration is per-axis: the tool-path accel over a segment is `min(x.accel/|uₓ|, y.accel/|u_y|)`, so a diagonal accelerates faster while each axis stays within its limit. `PATH_START`/`PATH_END` and corner-stops pin `v=0`; a zero-length corner gap propagates the stop to both sides (the lift-pivot precondition).

### 7+8 — Discretize (`discretize.py`)
Two jobs in one file, kept separate function-wise. **Choreograph** (the outer walk) inserts non-cutting motion around the cut: a travel jog between subpaths, A pre-orientation + Z-lower at each `PATH_START`, lift-pivot-lower at corners, Z-raise at `PATH_END`, and bounded A unwind for a wired tool — all driven by a `ToolProfile`. **Discretize** (the per-pair emit) turns each consecutive sample pair into MicroSegments: per-axis integer step deltas (float accumulators, invert applied to emitted sign only), tangent-tracking `da`, and an interval from the planned `v`. A velocity-aware sub-split keeps the speed change under `dv_max` within one segment, and a per-axis rate floor (`max_rate * steps_per_unit`) stops the A axis demanding multi-MHz rates on tight curves. Because Plan already brought the tool to `v=0` at every corner, between-curve corners and in-curve cusps collapse into one rule.

## Tool profiles

`config.py` defines `ToolProfile` presets keyed to tool **type**: `PEN` (no A tracking), `KNIFE` (tangential, wired → unwinds), `CREASE` (tangential, free-spinning). One knife model parameterised by `offset_mm`; an offset above `OFFSET_TOLERANCE_MM` needs blade-offset compensation (PLAN P6, not yet implemented) and is rejected rather than cut wrong. Adding a tool is a new preset, never a code change.

## Test data & tests

`data/` holds SVG fixtures (primitives, multi-segment paths, unit/viewport edge cases, Y-flip) plus `mock_curves.py` (analytically-known Bézier cases for the flatten/constrain/plan tests).

```sh
cd pipeline/stages
python -m pytest test_*.py -v        # or: for f in test_*.py; do python "$f"; done
```

76 tests, standard library only. The headline invariants under test: Flatten reproduces analytic arc length/curvature; Constrain ceilings are bounded and corner-stops fire; Plan is acceleration-continuous across every case; Discretize conserves net XY steps to the geometric endpoint. Host-level invariants (velocity/accel/interval bounds, step conservation, geometric fidelity) are checked end-to-end by `../host/validate_plan.py`.
