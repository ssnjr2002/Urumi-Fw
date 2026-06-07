# Pipeline Prototype Post-Mortem

Seven stages, Python-only, no hardware. SVG in, binary packets out, with a full Pico motion reference in between.

| Stage | What it does |
|---|---|
| 1 | SVG `<path>` → cubic Bezier namedtuples |
| 2 | Pixel coords → mm + Y-flip |
| 3 | C1 continuity repair at joins |
| 4 | Arc length (Gauss-Legendre) + curvature κ |
| 5 | Trapezoidal velocity planner |
| 6 | Bezier → MicroSegments (integer steps + clock intervals) |
| 7 | Binary SplineTile / ToolConfig packet serialiser |

126 tests. No external dependencies.

---

## 1. Tweaks from the plan

**Stage order was wrong.** The plan describes a single pipeline 1→6. We built it that way, then realised stage 7 (the actual host output) belongs after stage 3 — stages 4–6 are Pico-side logic, not host-side. This isn't actually a problem: the stages are independent modules that import from each other, not a coupled sequence. Re-ordering them in the final system means nothing more than changing which platform runs which file. The Python implementations of 4–6 remain valid as the ground-truth reference for the C++ port.

**`dt_vel` formula.** Plan says `dt ≤ Δv_max / (a · |ds/dt|)`. Implemented literally, this produced 55k segments on a simple snake because `|B'(t)|` is large for long curves. Replaced with numerical `dv/dt` from the velocity profile — zero subdivisions during cruise, fine subdivisions during accel/decel only.

---

## 2. Plan was ambiguous but shouldn't have been

**`v=0` at PATH boundaries.** Plan says "v_entry=0 at PATH_START" but never addresses what happens to `interval` when velocity is literally zero — it overflows. Resolved with `V_MIN=0.5 mm/s` as a hard floor. The plan mentions overflow only as a premortem item (P4), not as a design decision that touches every stage downstream.

**Zero-gap corner blends.** Plan says "insert a blending cubic" at tangent breaks but doesn't address joins where endpoints are identical (no spatial gap). A zero-chord blend collapses to a point, destroying the tangent. Resolved with a minimum handle length of 1mm.

---

## 3. Harder than expected

**Velocity subdivision.** Looked like one line from the plan. Took three iterations — literal formula, v-based rewrite, then numerical differentiation of the profile — because the interaction between Bezier parameter space, arc length, and mm/s speed isn't obvious until segment counts blow up in practice.
