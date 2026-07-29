# Planner Audit — findings before the C++ port

**Started:** 2026-07-29
**Status:** In progress — `flatten` audited, other stages pending.

Living document. One section per stage; append as each is audited. Findings stay
here until they are fixed or explicitly dismissed, and each carries a proposed
action rather than just a complaint.

---

## Why this exists

The planner is being consolidated into one C++ implementation shared by the host
(verification harness) and the Pico (production). Three implementations —
`pipeline/` (Python), `web/src/toolpath|production` (TypeScript), and the ramp
math already living in `src/rp2350/core1/core1.cpp` — collapse into one.

Porting is the moment defects get frozen into the surviving implementation, so
this is the cheap moment to find them. The audit is deliberately
evidence-driven: every finding below is a measurement against a real fixture,
not a code reading.

---

## Sequencing decision

**Fixes and the port must not land together.**

The output is a ~3800× amplifier — 81 Béziers become 3054 samples become 8437
microsegments. When C++ output differs from TypeScript output, the difference
must be attributable to exactly one cause. If bug fixes, `float32`, and 1600
lines of fresh transcription all change at once, it is not debuggable.

The agreed order:

1. **Write the property tests now, in TypeScript.** They fail. That is the
   point — the defect is pinned and named before anything moves. The tests are
   the durable artifact; they port to C++ as the real deliverable. The
   TypeScript fixes are not durable and mostly should not be written.
2. **Freeze the golden as archaeology, not authority.** It is self-referential
   by its own docstring and we now know it encodes at least one defect. It
   cannot tell us the planner is correct. Its only job is attribution during the
   port: *"this is what the machine that cuts fish today does."*
3. **Port bug-for-bug.** Output should match modulo `float32` rounding. Any
   structural difference is a transcription bug — a binary signal needing no
   judgement. The failing property tests stay red in both implementations, which
   is *evidence the port is faithful*.
4. **Fix in C++ once**, property tests as the gate. The golden moves then, with
   one variable in play.

**What would change this:** if a later stage turns out to be *actively* wrong on
the machine rather than latently wrong, "preserve today's behaviour" stops being
a useful baseline and that fix should go in immediately, ahead of the port.

---

## Findings

| # | Stage | Severity | Summary | Status |
|---|---|---|---|---|
| F1 | flatten | **defect** | Tangent cap SKIPPED at `\|B'\|→0` — 178° reversal in one step (89×) | open, **test red** |
| F2 | flatten / constrain | **inconsistency** | Intra-curve cusp is a corner to `discretize`, invisible to `constrain` | open, test documents |
| F3 | flatten | minor | Truncated final step manufactures degenerate near-zero-`ds` samples | open, test green |
| F4 | flatten | gap | `chordTol` and `dthetaMax` had no tests — two of three caps unverified | **resolved** |
| F5 | flatten | tuning | `chordTol` is near-vestigial: binds 0.8% of steps | note only |
| F6 | geometry | cleanup | `arcLength` (5-point Gauss-Legendre) has no production caller | open, test pins it |
| F7 | flatten | **contract** | All three caps are PREDICTORS, not bounds — `dsMax` soft by up to 8% | open, **test red** |

---

## Stage 4 — `flatten`

### How it actually works

Not recursive subdivision, and not quadrature. It is a **forward marcher with an
adaptive step predictor**: at each `t`, take the min of three geometric caps,
step, repeat.

```
1. chord deviation:  dt <= sqrt(8 * chordTol / |B''(t)|)
2. spacing:          dt <= dsMax / |B'(t)|
3. tangent step:     dt <= dthetaMax / (kappa * |B'(t)|)
```

Iterative, no recursion, O(1) state. This matters for the port: it is already
the right shape for lazy just-in-time flattening inside a bounded window
(`nextSample()` → advance `t`, emit). Recursive subdivision would need an
explicit stack to become resumable; quadrature needs the whole curve up front.
**The stage assumed hardest to port is the one already shaped for streaming.**

`ds` is the straight-line **chord** between samples, not true arc length. This
under-estimates path length, which errs safe — `plan` reads `ds` as available
braking distance, so it brakes marginally early.

### Measurements (fish.svg, 83 curves, 2971 steps, default quality config)

```
binding cap:  chord=24  dsMax=1443  dtheta=1213  dtMax=291
chordTol=0.01mm    worst actual deviation = 0.00219mm   (0.22x)
dthetaMax=2deg     worst actual dtheta    = 180.00deg   (90x)
dtMin bound the step 0 times
tiny truncated final step: 2 of 83 curves

worst dtheta by cause: {"none": 2.12, "k<=1e-9": 180, "speed<=1e-12": 116.57}
```

---

### F1 — Epsilon guards invert the tangent cap at cusps

**Severity: defect. Latent today.**

```ts
if (speed > 1e-12) {
    dt = Math.min(dt, dsMax / speed);
    const k = curvature(c, t);
    if (k > 1e-9) {
        dt = Math.min(dt, ((dthetaMax * Math.PI) / 180) / (k * speed));
    }
}
```

Both caps are gated on *"is this quantity measurable."* At a cusp the correct
behaviour is the opposite: near-zero speed is exactly where the tangent is least
stable and the cap matters most. Instead both are skipped, `dt` falls back to
`dtMax`, and the marcher steps straight over the cusp.

Evidence: where both caps apply the worst overshoot is **2.12°** against a 2.0°
cap — a 6% prediction error, benign, caused by evaluating κ at the step start.
Where a cap is skipped: **180°** (`k<=1e-9`) and **116.57°** (`speed<=1e-12`).
Six steps on the fish.

This is the same family as the bug `PLAN_pipeline_redesign.md` §1 was written
about (`|B'|→0`, a 17.8 mm curve dragged to 0.30 mm/s). That fix changed the
planning quantum; these guards are the same pathology one stage earlier.

**Confirmed by test** (`flatten: cap 3`) with the purpose-built `CUSP` fixture —
an exact cusp, `B'(0.5) = 0`, derived rather than hand-tuned:

```
quarter_circle_r5: sample 35 turned   2.004deg  (1.0x dthetaMax)
near_cusp:         sample 63 turned   2.807deg  (1.4x dthetaMax)
cusp:              sample 47 turned 178.035deg  (89.0x dthetaMax)
snake.svg:         sample 124 turned  2.062deg  (1.0x dthetaMax)
```

**Revision to the earlier write-up:** the fish measurement suggested the
predictor error was a benign ~6%. Against fixtures chosen to stress it, it
reaches **1.4×** on `near_cusp`. So there are two separate problems sharing one
test, and they need different fixes — the graded predictor error is F7; the
catastrophic skip is F1.

**Proposed action:** when `speed` or `κ` falls under the epsilon, clamp `dt`
*down* rather than leaving it at `dtMax`. A cusp must force fine sampling.

**Open question:** what is the right floor? `dtMin` (1e-6) would emit ~10⁶
samples across a cusp. Needs a bound that is fine enough to resolve the reversal
but cannot blow a sample-count-bounded window on the Pico. Note the current
`CUSP` fixture emits only 78 samples, so whatever the floor is, it is not being
reached — the marcher walks over the cusp rather than into it.

---

### F2 — `constrain` and `discretize` disagree about what a corner is

**Severity: inconsistency. Consequence of F1.**

`constrain` only inspects a tangent jump when the flag is set:

```ts
if ((s.flags & CURVE_BOUNDARY) && i > 0) {
```

An *intra-curve* cusp carries no `CURVE_BOUNDARY`, so `constrain` never looks.
κ there is ≤1e-9, so the centripetal and A-slew caps are skipped too. The sample
gets `cap = feedMax`.

`discretize` **does** catch it — it computes `dtheta` between consecutive samples
ungated and sets `isCorner`. So for a tangential tool a lift-pivot-lower is
inserted at a sample `plan` never decelerated into, violating the assumption
stated in `discretize` itself: *"Corners (v~0 both ends, dtheta huge) also stay
k=1."*

For a non-tangential tool `isCorner` is false and nothing catches it: an
instantaneous XY direction reversal at full feed.

**Proposed action:** make the two agree. Natural fix is for `flatten` to mark
cusps with a flag the way it already marks curve joins, so both stages read the
same signal from the same place.

**Open question:** is an intra-curve cusp a corner (lift-pivot-lower) or a
tangency event to be slowed through? Decide before either stage changes.

---

### F3 — Truncated final step manufactures degenerate samples

**Severity: minor today, load-bearing for the port.**

```ts
t = Math.min(t + dt, 1);
```

The last step of every curve is truncated to land on `t=1`, so a curve whose
final step overshoots emits an arbitrarily small sliver — 2 of 83 curves on the
fish. This is a second source of `ds ≈ 0` samples, distinct from the deliberate
near-duplicates at curve joins.

Matters on the Pico: a lookahead window bounded in millimetres has no bound on
sample count, and slivers are what overflow it.

**Proposed action:** distribute the remainder across the last two steps instead
of emitting a sliver.

**Test status: green.** The sliver test passes on the `CASES` fixtures and
`snake.svg` — no interior `ds` below 1e-6. The 2-of-83 slivers were measured on
`fish.svg`, which is production snapshot data rather than a toolpath fixture.
The test is therefore not currently guarding this finding. Either promote a
fish-derived subpath into the fixture set, or lower the sliver threshold to
something relative (e.g. `dsMax * 1e-3`) that the existing fixtures can trip.
**Do not** mark F3 fixed on the strength of a green test that never sees the
geometry that produces it.

---

### F4 — Two of the three caps were untested — RESOLVED

**Severity: gap. This is why F1 survived.**

`flatten.test.ts` covered arc length, endpoints, curvature, flags,
multi-subpath, `dsMax` spacing (on the straight line only — the one fixture
where it is trivially satisfied), corners, and a real SVG. There was **no test
asserting chord deviation ≤ `chordTol`, and none asserting Δθ ≤ `dthetaMax`.**

Rewritten. The file now separates INVARIANTS (true of any correct flattener;
must survive the port unchanged) from CONTRACT PROPERTIES (the three caps that
define the stage), and asserts the caps over every fixture including cusps and
real artwork. Cap 1 passes; caps 2 and 3 are red, which is the point.

Deleted, with reasons:

- **`"near_cusp length reasonable (chord sum >= GL5)"`** — asserted the wrong
  direction. A chord sum can never exceed true arc length; the test only passed
  because GL5 *under*-reports a near-cusp. It pinned an error in the oracle and
  called it a property of `flatten`. Replaced with a dense-summation reference.
- **`"snake.svg — total 150-200mm"`** — a 50 mm-wide window that substantially
  broken output would pass.
- **`expect(samples.every(s => typeof s.x === "number")).toBe(true)`** — a
  tautology; the type system already guarantees it.

Measurement method changed too: chord deviation is now a one-sided Hausdorff
distance from a densely probed true curve to the emitted polyline. The obvious
alternative — re-running `dtAt` in the test — would only prove the predictor is
self-consistent. This form needs none of flatten's internals and ports to C++ as
an acceptance gate unchanged.

**Test-design lesson worth keeping.** The first version put `expect` inside the
fixture loop, so it threw on the first violation and never reached the rest — it
reported `near_cusp` at 2.8° and silently skipped `cusp` at 178°. A property
test that hides its worst case behind its first case is worse than no test,
because it looks like it ran. All fixture-table tests now collect violations and
fail once with the full list.

---

### F5 — `chordTol` is near-vestigial

**Severity: note only. No action proposed.**

It binds **24 of 2971 steps (0.8%)** and the worst realised deviation is 4.5×
*under* tolerance. Density is set almost entirely by `dsMax` and `dthetaMax`.

Worth recording because it is the knob an operator would reach for to improve
accuracy, and at current settings turning it does almost nothing.

---

### F7 — The caps are predictors, not bounds

**Severity: contract. Found by the rewritten tests, not by the original audit.**

Every cap in `dtAt` is evaluated at the **start** of a step and then applied
across the whole step. Where the curve speeds up or bends more over that
interval, the realised value overshoots the cap. This is systematic, not float
noise.

`dsMax = 0.5mm`, worst realised step per fixture:

```
straight_line       99 over, worst 0.5000000748  (excess 7.5e-8mm, 0.000015%)
long_gentle_arc    506 over, worst 0.5000394601  (excess 3.9e-5mm, 0.0079%)
quarter_circle_r50  78 over, worst 0.5005625179  (excess 5.6e-4mm, 0.11%)
full_circle_r30    184 over, worst 0.5009231191  (excess 9.2e-4mm, 0.18%)
s_curve            176 over, worst 0.5032277692  (excess 3.2e-3mm, 0.65%)
snake.svg          142 of 356 steps over,        worst +1.50%
near_cusp           54 over, worst 0.5347009343  (excess 3.5e-2mm, 6.9%)
cusp                11 over, worst 0.5395852777  (excess 4.0e-2mm, 7.9%)
```

Even `straight_line` overshoots — that fixture's control points are 33.333 /
66.667 rather than exact thirds, so |B'| ranges 99.999 → 100.0005 and the
predictor is fractionally stale. Real artwork is far worse: `snake.svg` has
**142 of 356 steps over the cap.**

The same mechanism drives the graded half of F1: `quarter_circle_r5` at 1.0×
`dthetaMax`, `near_cusp` at 1.4×.

**Is it harmful?** Probably not on its own. `dsMax` exists to guarantee enough
samples on long straights for smooth accel ramps, and 1.5% coarser than asked is
irrelevant to that. This is filed as a *contract* problem rather than a defect:
the code documents three caps as bounds and implements them as estimates, and
the port is the wrong moment to carry an undocumented approximation into the
surviving implementation.

**Two honest options, both fine — pick one deliberately:**

1. **Enforce.** After stepping, measure the realised chord / turn and halve-and-
   retry on overshoot. Turns all three caps into guarantees. Costs a re-
   evaluation on the rare miss; keeps the iterative structure intact and still
   ports cleanly to a streaming Pico window.
2. **Restate.** Document them as targets with a stated tolerance, and assert
   that tolerance in the tests instead of equality.

Option 1 is worth more than it costs *if* the cusp fix (F1) lands anyway, since
the retry machinery is the natural place to hang a "this step turned too far"
check. Option 2 is honest but leaves the Pico-side window sizing resting on a
soft bound.

**Do not** simply loosen the test epsilon until it goes green. That converts a
known approximation into an invisible one.

---

### F6 — `arcLength` has no production caller

**Severity: cleanup.**

`geometry.ts` implements a 5-point Gauss-Legendre quadrature of `|B'(t)|`. Only
tests call it — the pipeline uses chord sums throughout.

**Proposed action:** decide its fate before the port. Either give it a caller
(true arc-length `ds` instead of chord) or drop it. Carrying dead quadrature
into C++ is pure cost.

---

## Current test state

`npx vitest run test/toolpath/flatten` — 20 passing, 4 red, all 4 intentional:

```
× cap 2 — spacing <= dsMax          7 violations (F7)
× cap 3 — tangent turn <= dthetaMax 3 violations (F1 + F7)
× snake.svg — spacing cap holds     142 of 356 steps (F7)
× snake.svg — tangent cap holds     2.062deg vs 2.0 (F7)
```

Full suite: 575 passing, 4 red, `tsc --noEmit` clean. The `CUSP` fixture is
deliberately outside the shared `CASES` registry, so `constrain` / `plan` /
`discretize` tests are unchanged and still green — a cusp regression in a later
stage will be attributable to that stage.

---

## Stages pending

| Stage | Status |
|---|---|
| 3 `repair` (`enforceC1`) | not audited |
| 5 `constrain` | not audited — next |
| 6 `plan` | not audited |
| 8 `discretize` | not audited |
| 9 `dutyBreaks` | partially known: see `tool_duty_limits.md` §5, §11 |

When auditing a later stage, consider adding `CUSP` to that stage's fixtures
deliberately. It is the geometry every stage handles worst, and it is currently
only exercised against `flatten`.

---

## Reproducing the measurements

The numbers above came from throwaway probes under `web/test/`, run with
`npx vitest run <path>` and deleted afterwards. They re-derive `dtAt` locally and
densely sample each step to measure *actual* deviation and tangent turn, rather
than trusting the predictor.

If a probe is worth keeping it should become a property test under F4, not live
on as a script.
