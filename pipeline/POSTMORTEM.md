# Pipeline Prototype Post-Mortem

Six stages, Python-only, no hardware. SVG in, MicroSegments out, with a full Pico motion reference in between. Serialisation to wire packets is not a pipeline concern — it lives in `host/serialise.py`.

| Stage | What it does |
|---|---|
| 1 | SVG elements → cubic Bezier namedtuples, subpath-aware |
| 2 | Pixel coords → mm + Y-flip |
| 3 | C1 continuity repair at joins |
| 4 | Arc length (Gauss-Legendre) + curvature κ |
| 5 | Trapezoidal velocity planner |
| 6 | Bezier → MicroSegments (integer steps + clock intervals) |

147 tests. No external dependencies in the pipeline itself.

---

## 1. Tweaks from the plan

**Stage order was wrong.** The plan described a single pipeline 1→7, with stage 7 as the packet serialiser. In practice stages 4–6 are Pico-side logic, not host-side — the serialiser doesn't belong in the pipeline at all. The stages are independent modules; re-ordering them means nothing more than changing which platform runs which file. Serialisation moved to `host/serialise.py`. The Python implementations of 4–6 remain the ground-truth reference for the future C++ port.

**`dt_vel` formula.** Plan says `dt ≤ Δv_max / (a · |ds/dt|)`. Implemented literally, this produced 55k segments on a simple snake because `|B'(t)|` is large for long curves. Replaced with numerical `dv/dt` from the velocity profile — zero subdivisions during cruise, fine subdivisions during accel/decel only.

---

## 2. Plan was ambiguous but shouldn't have been

**`v=0` at PATH boundaries.** Plan says "v_entry=0 at PATH_START" but never addresses what happens to `interval` when velocity is literally zero — it overflows. Resolved with `V_MIN=0.5 mm/s` as a hard floor. The plan mentions overflow only as a premortem item (P4), not as a design decision that touches every stage downstream.

**Zero-gap corner blends.** Plan says "insert a blending cubic" at tangent breaks but doesn't address joins where endpoints are identical (no spatial gap). A zero-chord blend collapses to a point, destroying the tangent. Resolved with a minimum handle length of 1mm.

---

## 3. Harder than expected

**Velocity subdivision.** Looked like one line from the plan. Took three iterations — literal formula, v-based rewrite, then numerical differentiation of the profile — because the interaction between Bezier parameter space, arc length, and mm/s speed isn't obvious until segment counts blow up in practice.
