# pipeline/

SVG-to-MicroSegment motion planning pipeline. Pure Python, no external dependencies. Takes an SVG file and produces a flat list of `MicroSegment` step events ready for serialisation and transmission to the RP2350.

## Stages

| Stage | File | Input | Output |
|---|---|---|---|
| 1 | `stages/stage1.py` | SVG file | `list[CubicBezier]` — all paths as cubic Béziers in SVG pixel coords |
| 2 | `stages/stage2.py` | Stage 1 output + SVG viewport | Same curves in **mm**, Y-axis flipped (machine origin = bottom-left) |
| 3 | `stages/stage3.py` | Stage 2 output | Repaired curves with C1 continuity enforced at joins |
| 4 | `stages/stage4.py` | Stage 3 output | `list[CurveMetrics]` — arc length (mm) and curvature κ per curve |
| 5 | `stages/stage5.py` | Stage 4 output | `list[PlannedCurve]` — trapezoidal velocity plan (v_entry, v_cruise, v_exit) |
| 6 | `stages/stage6.py` | Stage 5 output | `list[MicroSegment]` — integer step deltas (dx, dy, dz, da) + clock intervals |

Stages 4–6 are the ground-truth Python reference for a future C++ port on the RP2350. The Python implementations and the C++ port must emit identical `MicroSegment` streams.

## Configuration

All tuning lives in `stages/config.py`. Three tiers:

| Tier | Class | Governs |
|---|---|---|
| `machine` | `MachineConfig` / `AxisConfig` | Steps/unit, axis→node map, invert, max_rate, Z/A resolution |
| `motion`  | `MotionConfig` | feed_max, a_max, jog_feed, junction_deviation, lift_height, z_feed |
| `quality` | `QualityConfig` | chord_tol, dv_max, v_min, dt limits, angle_tol, gap_tol, n_kappa |

`config.default()` returns the current physical machine (DM542 @ 1/32 micro-step, GT2 20T pulley, X/Y = 160 steps/mm, Z = 1200 steps/mm, A = 120 steps/deg).

Each stage CLI sources its `argparse` defaults from `config.default()` and still accepts per-flag overrides, so every stage is independently runnable.

## Running a stage standalone

Each stage is executable as a script. All take an SVG file as the first argument:

```sh
# SVG → cubic Béziers (stage 1)
python stages/stage1.py data/test.svg

# SVG → mm coords (stages 1+2)
python stages/stage2.py data/test.svg

# C1 continuity repair (stages 1–3)
python stages/stage3.py data/test.svg --angle-tol 5 --gap-tol 0.01

# Arc length + curvature (stages 1–4)
python stages/stage4.py data/test.svg

# Velocity plan (stages 1–5)
python stages/stage5.py data/test.svg --feed-max 80 --a-max 1000

# Full pipeline → MicroSegments (stages 1–6)
python stages/stage6.py data/test.svg --feed-max 80 --a-max 1000
```

## Stage detail

### Stage 1 — SVG parser
Handles `<path>`, `<circle>`, `<ellipse>`, `<rect>` (including rounded corners), `<line>`, `<polygon>`, `<polyline>`. SVG commands M, L, H, V, C, S, Q, Z — both absolute and relative. Lines and quadratic curves are converted to degenerate cubics. Elements with `fill:none; stroke:none` are skipped.

Returns `list[list[CubicBezier]]` (subpath-aware) or a flat `list[CubicBezier]`.

### Stage 2 — Coordinate transform
Reads `viewBox` and `width`/`height` from the SVG root. Resolves physical units (mm, cm, in, pt, px). Applies: (1) viewBox offset, (2) scale to mm, (3) Y-axis flip so machine +Y is up.

### Stage 3 — C1 continuity repair
At each curve join, checks C0 (endpoints meet) and G1 (exit tangent parallel to entry tangent):
- **Gap > `gap_tol`** → inserts a bridging cubic with tangent-preserving handles.
- **Angle > `angle_tol`** → logs a `"cusp"` repair. Sharp corners are left as-is so the velocity planner (stage 5) can apply junction-deviation cornering instead of producing a tiny loop.
- **Degenerate tangents** → inserts a fallback blend.

### Stage 4 — Arc length + curvature
Arc length: 5-point Gauss-Legendre quadrature of |B′(t)| over [0,1]. Curvature: κ(t) = |B′ × B″| / |B′|³, sampled at `n_kappa` evenly-spaced t values. Returns `CurveMetrics(curve, path_length_mm, kappa_max, kappa_samples)`.

### Stage 5 — Velocity planner
Forward + backward trapezoidal pass. Velocity caps (lowest wins):
1. `feed_max` — cruise ceiling
2. Centripetal limit — `sqrt(a_max / kappa_max)`
3. GRBL-style junction-deviation cornering at cusps

Flags: `PATH_START` (v_entry = 0), `PATH_END` (v_exit = 0), `MERGE_WITH_PREV` (curves share one velocity envelope). Returns `PlannedCurve(metrics, flags, v_entry, v_cruise, v_exit, merged)`.

### Stage 6 — MicroSegment evaluator
Adaptive `dt` driven by two constraints:
- **Geometric**: chord deviation < `chord_tol` (≈ 0.01 mm)
- **Velocity**: speed change < `dv_max` per segment (≈ 3 mm/s)

At each sample: integer step deltas via per-axis resolution + sub-step accumulator, tangential A-axis rotation (`da`), clock interval in RP2350 cycles. Travel jogs (between subpaths) and Z pen-lift moves are also emitted here.

Per-axis rate limiting (`max_rate * steps_per_unit`) floors the clock interval so no axis exceeds its physical step-rate ceiling — prevents the A axis from demanding multi-MHz step rates on tight curves.

## Test data

`data/` contains SVG fixtures used by the stage tests:

| File | Tests |
|---|---|
| `test_rect.svg`, `test_circle.svg`, `test_ellipse.svg`, `test_line.svg`, `test_polygon.svg` | Primitive element parsing |
| `test_snake.svg`, `test_star.svg`, `test_triangle.svg` | Multi-segment paths |
| `coord_mm_units.svg`, `coord_cm_units.svg`, `coord_px_units.svg` | Unit conversion |
| `coord_nonzero_origin.svg`, `coord_nonsquare.svg`, `coord_no_size.svg` | Viewport edge cases |
| `coord_yfliip_verify.svg` | Y-flip correctness |
| `test_saturate.svg` | Curvature saturation |

## Running tests

```sh
cd pipeline/stages
python -m pytest test_stage*.py -v
```

147 tests, no external dependencies beyond Python's standard library.
