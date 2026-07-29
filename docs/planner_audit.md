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
| C1 | constrain | **defect** | No lower bound on `vCeiling` — a cusp yields 3.2e-3 mm/s, 166× under `vMin` | open, test documents |
| C2 | constrain | ok | All four caps hold as per-sample properties on every fixture | verified |
| P1 | constrain + plan | **defect** | Axis accel budget spent twice: centripetal and tangential each capped at `aMax`, nothing owns the sum (→ √2·aMax) | open, **test red** |
| P2 | plan | **contract** | A stream without `PATH_START`/`PATH_END` is silently unplanned — `v = vCeiling`, no error | open, **test red** |
| P3 | plan | consequence of C1 | Carries unexecutable ceilings through; ~⅕ of the below-`vMin` span is self-inflicted by the sweeps | open, **test red** |
| P4 | compileBlock | tuning | A non-tangential tool still pays the A-axis curvature cap — ~8× accel loss on a 5 mm arc | open, test documents |
| P5 | plan | ok | Two O(n) sweeps, no convergence loop; feasibility, monotonicity and endpoint pinning all hold | verified |
| D1 | discretize | **defect** | Empty segment (all deltas 0) emitted with `interval = fCpu` — a full second. Reachable at a corner AND at every `PATH_END` | open, **test red** |
| D2 | discretize | **defect** | Sub-segment speed interpolated linearly in *distance*, not `sqrt(v0²+2as)` — timing error up to 1.51×, worse the finer it subdivides | open, **test red** |
| D3 | discretize | **contract** | `interval`'s per-axis rate floor is a second, unmodelled speed governor; executed ≠ planned timeline | open, **test red** |
| D4 | discretize | **inconsistency** | Corner rule ungated on `CURVE_BOUNDARY` unlike constrain's — this is F2, now measured | open, **test red** |
| D5 | discretize | gap | Every tool ships `liftHeight = 0`, so the entire Z lift/lower path was dead and untested | **resolved** (tests) |
| H1 | choreograph | **defect** | `aMove`'s decel ramp exceeds the A accel limit by 1.26–1.65× and never reaches rest — stops dead from up to 39 deg/s. Chunk-start rate sampling is conservative going up, anti-conservative coming down | open, **test red** ×3 |
| H2 | choreograph | **defect** | `travelJog` / `headOffsetJog` emit one segment at full feed — 0→80 mm/s in zero distance, ignoring `x.maxAccel` entirely | open, **test red** |
| H3 | choreograph | known | `zMove` is likewise unramped (0→24000 steps/s); acknowledged by the module TODO, and `z.maxAccel` is 0 so there is no limit to check against | open, **test red** |
| H4 | choreograph | **contract** | `aMove` silently invents 180 deg/s + 2000 deg/s² when the A ceilings are 0 — `load.ts` refuses to invent calibration, this invents limits | open, **test red** |
| H5 | choreograph | cleanup | Three redundant guards all defend `v ≥ v0`; each is an equivalent mutant | note only |

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

**CORRECTION.** The original write-up claimed the cusp sample "gets
`cap = feedMax`" — full speed through the reversal. **That is wrong.** Measured
against the `CUSP` fixture, κ at the reversal is 1.5e+1 (large, not zero), so
the curvature caps bite hard and the ceiling collapses to ~3e-3 mm/s. Constrain
does not race through the cusp; it *crawls*. The consequence is C1 below, not a
dynamics violation.

The structural claim stands: the corner-stop branch is gated on
`CURVE_BOUNDARY`, `flatten` only sets that at curve JOINS, so an intra-curve
reversal is never *considered* for a corner stop while `discretize`'s ungated
`dtheta` check treats it as one.

**Proposed action:** make the two agree. Natural fix is for `flatten` to mark
cusps with a flag the way it already marks curve joins, so both stages read the
same signal from the same place.

**Open question:** is an intra-curve cusp a corner (lift-pivot-lower) or a
tangency event to be slowed through? Decide before either stage changes. The
measurement above argues for lift-pivot: a tangential knife physically cannot
track a 178° tangent change while moving, so the "slow through it" branch is
asking for a crawl that C1 shows is not even executable.

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

## Stage 5 — `constrain`

Materially healthier than `flatten`. Every cap holds as a per-sample property on
every fixture including the cusp — **zero violations** — so the min() chain and
its formulas are sound. One real defect, and it is about what the ceiling means
rather than how it is computed.

### C2 — the caps hold (verified, no action)

Asserted per sample over all nine fixtures and `snake.svg`, not at one
hand-picked mid-sample as before:

```
centripetal  v <= sqrt(aMax / kappa)          0 violations
A-slew       v <= rad(aRateDegS) / kappa      0 violations
feedMax      v <= feedMax                     0 violations
corner stop  v == 0 exactly at CURVE_BOUNDARY samples, and nowhere else
```

Unlike `flatten`'s caps (F7), these are computed from the sample's own κ and
applied to that same sample — no forward prediction, so nothing to drift.

### C1 — `vCeiling` has no lower bound

**Severity: defect. Affects timing, not safety.**

On the `CUSP` fixture the A-axis caps drive the ceiling to **3.24e-3 mm/s**.
`quality.vMin` is **0.5 mm/s**, and `discretize` clamps the step interval to it.
So:

```
planned speed at the cusp    0.00324 mm/s
executed speed at the cusp   0.5     mm/s      (vMin floor in discretize)
ratio                        ~166x
```

The plan and the machine disagree by two orders of magnitude at that sample.
Consequences, in order of how much they matter:

1. **Every timeline derived from the plan is wrong across a cusp.** This is not
   academic — `dutyBreaks` schedules enable-line resets against exactly that
   timeline, and `tool_duty_limits.md` §5 already establishes that a mis-measured
   window is how the knife overruns its budget.
2. The decel ramp `plan` builds into the cusp is real, but its floor is not
   executed, so the profile the machine follows is not the profile that was
   planned.
3. It is the redesign's founding bug wearing different clothes. `PLAN_pipeline_
   redesign.md` §1 was written about a curve dragged to **0.30 mm/s**; this is
   0.003 mm/s, 100× worse. The per-sample fix stopped one *sample's* spike from
   taxing a whole curve — it did not stop the spike itself from being unusable.

**Proposed action:** give `constrain` the same floor `discretize` already
enforces. A ceiling below `vMin` is not a ceiling, it is a stop that has not
admitted it — so clamp to `vMin`, or force 0 and let the corner machinery handle
it honestly. The second is probably right, and it is the same decision F2 asks
for.

**Open question:** `vMin` currently lives in `QualityConfig` and is only read by
`discretize`. If `constrain` needs it too, it should be passed in explicitly
rather than imported — the stage takes no config today and that property is
worth keeping.

### Test rewrite

39 tests, all green, same INVARIANTS / CONTRACT PROPERTIES split as `flatten`.
Added: purity (constrain documents itself as pure, and the port will reuse
buffers), determinism, sample-preservation, per-sample cap properties, real-SVG
coverage, and cusp documentation.

**Monotonicity instead of formula-copying.** Where checking a cap directly would
mean re-deriving internals (`kappaPrime` is not exported), the test asserts that
*tightening any limit never raises any ceiling*. That catches a botched `min()`
chain — the most likely transcription error in the port — without touching
internals.

**Scaling laws instead of directions.** `sqrt(aMax/κ)` and `aRateRad/κ` differ
only in shape, and pasting one into the other's branch is an easy slip. So the
tests pin exponents: 4× `aMax` must buy exactly 2× speed; 4× `aAccel` exactly 2×;
4× `junctionDeviation` exactly 2×. A dimensionally-wrong cap gives 4× and fails.

### Mutation-validated

The tests were checked by deliberately breaking `constrain` nine ways and
confirming each one goes red. All nine caught:

```
centripetal: sqrt dropped          4 failed
A-slew removed                     1 failed
gradient: sqrt dropped             2 failed
junctionCap: deviation dropped     3 failed
junctionCap: reversal guard gone   1 failed
forcedStops ignored                3 failed
junction cap not applied           1 failed
feedMax clamp loosened             4 failed
curvature caps disabled            7 failed
```

Worth repeating for later stages. A green suite says nothing about whether the
tests can *fail*; this is the cheap way to find out. Restore with
`git diff --exit-code` afterwards rather than trusting the edit was undone.

---

## Stage 6 — `plan`

### How it actually works

Two O(n) sweeps per subpath over the sample stream, and nothing else:

```
backward, last → first:   v[i] = min(v[i],  sqrt(v[i+1]² + 2·a·ds[i]))
forward,  first → last:   v[i] = min(v[i],  sqrt(v[i-1]² + 2·a·ds[i-1]))
```

with `v` initialised to `vCeiling` and the two endpoints pinned to 0. No
iterate-to-convergence, no retry, no global state. **This is the stage best
suited to the port as written** — it is already a streaming-shaped algorithm,
and the backward sweep is the only part needing lookahead (the windowing
question, premortem P1/P2).

Two structural notes worth carrying into C++:

- `segAccel` is symmetric and depends only on the two endpoint samples, so it
  can be computed once per segment as the stream arrives.
- The endpoint re-pin after the forward sweep is **dead code**: both sweeps only
  ever take a `min`, and `v[hi]` starts at 0, so it cannot be lifted. The
  comment above it claims otherwise. Harmless, but do not port the comment.

### P5 — the sweeps are sound (verified, no action)

Checked as properties over all nine fixtures (`CASES` + `CUSP`), not spot cases:

| Property | Result |
|---|---|
| `0 ≤ v ≤ vCeiling`, finite everywhere | holds |
| Every adjacent pair reachable and stoppable | holds |
| `PATH_START` / `PATH_END` plan to exactly 0 | holds |
| Subpaths planned independently | holds |
| Raising any accel or ceiling never lowers any `v` | holds, 0 violations |
| `subpathRanges` covers every sample once, no gaps or overlaps | holds |

Monotonicity is the one worth keeping in mind for the port: it is cheap to check
and it catches a botched `min()` chain, which is the likeliest transcription
error.

### P1 — the axis acceleration budget is spent twice

The tool's acceleration has two orthogonal components:

```
tangential   a_t = dv/dt        bounded by plan, via segAccel's per-axis projection
centripetal  a_c = v²·κ         bounded by constrain, via vCeiling ≤ sqrt(aMax/κ)
```

Each stage bounds its own component by `aMax`. Neither bounds the **vector
sum**, which is what an axis actually has to supply. The analytic worst case is
`√2·aMax`; measured, it is essentially attained:

```
fixture              |ax| max        |ay| max        |a| total
s_curve                557 (0.56×)    1014 (1.01×)     1128
near_cusp             1002 (1.00×)      41 (0.04×)     1002
cusp                   990 (0.99×)    1081 (1.08×)     1412   ← √2·aMax = 1414
```

Neither stage is wrong in isolation, which is why per-stage review missed it —
it is an **interface** defect, visible only when the two are composed. That
makes it exactly the kind of thing the port would otherwise carry across
silently into C++, where it is harder to see.

The overrun is modest on real geometry (1–8%) and severe only at cusps, so this
is not urgent. But note it compounds with F1: `flatten` steps *over* a cusp, so
the sample stream understates the turn that produces the centripetal term.

**Options, not yet decided:**

1. Budget the sum explicitly: have `constrain` leave headroom, e.g. cap
   centripetal at `aMax·sin(φ)` and tangential at `aMax·cos(φ)` for some split
   φ. Correct, and costs speed everywhere for a bound that binds at cusps.
2. Bound the sum in `plan`, where both terms are known: the backward/forward
   sweeps could clamp `v` so that `hypot(a_t, v²κ) ≤ aMax`. Fixes it where the
   information is, but makes the sweeps non-closed-form.
3. Accept it and document the real ceiling as `√2·aMax`, sizing `aMax` config
   accordingly. Cheapest; makes the config value mean something non-obvious.

Do **not** resolve this by lowering `aMax` until the test goes green — that
trades a stated bound for a tuned one, and the √2 will still be there.

### P2 — an unbracketed stream is silently unplanned

`subpathRanges` yields nothing for a stream carrying no `PATH_START`/`PATH_END`,
so both sweeps are skipped, the endpoints are never pinned, and `plan` returns
`v = vCeiling` verbatim. A 100 mm line comes back at **full feed from a standing
start**, with no error. The same happens to a subpath whose `PATH_END` is
missing: the range is dropped entirely.

`flatten` always brackets correctly, so production is safe **today**. The reason
this is filed rather than ignored:

- `plan` is an exported pure stage typed against arbitrary `ConstrainedSample[]`,
  and the port will give it callers that are not `flatten` — jog moves and
  streamed tiles both construct sample runs directly.
- The failure mode is the worst available: not a crash, but full-speed motion
  from rest. On metal that is a lost-steps or crashed-gantry event.
- It is nearly free to fix now (throw, or treat an unterminated run as ending at
  the last sample) and awkward to retrofit once callers rely on the current
  silence.

### P3 — `plan` carries C1's unexecutable ceilings through

Severity here must be measured as **arc length spent below `vMin`**, not as a
count of samples. Every ramp from rest necessarily crosses `(0, vMin)` on the
way up, so a sample landing in that band is sometimes legitimate — one does, at
`v = 0.498`, on the cusp fixture. The distance an honest crossing costs is
bounded and tiny:

```
vMin² / 2a = 0.5² / 2000 = 1.25e-4 mm
```

Against that yardstick the fixtures separate completely:

```
straight_line … full_circle_r30    0 mm          (0×)
near_cusp                          1.11e-1 mm  (887×)   slowest 1.6e-2 mm/s
cusp                               6.75e-2 mm  (540×)   slowest 4.8e-3 mm/s
```

**A hypothesis that did not survive measurement:** I expected `plan` to amplify
C1 — for the sweeps to spread one bad ceiling across a wide neighbourhood. The
sample *counts* are identical before and after planning (near_cusp: 72 ceilings
below `vMin`, 72 speeds below it). By arc length there is a real but modest
spread: on `cusp`, 1.40e-2 mm of the 6.75e-2 mm total (about a fifth) lands on
samples whose own ceiling was healthy.

That ratio is the useful part, because it localises the fix. **A `vMin` floor
belongs in `constrain`**, where the ceiling is set: it removes the four fifths
directly, and the remaining fifth goes with it, because the sweeps will have
nothing pathological left to ramp toward. Clamping in `plan` would only move the
same divergence one stage later — and clamping in `discretize`, which is what
happens today, is what produces the planned-vs-executed mismatch in the first
place.

### P4 — a non-tangential tool still pays the A-axis cap

`compileBlock` gates the A-axis limits it passes to `constrain` on
`profile.tangential`, but passes `axes.a.maxAccel` to `plan`
**unconditionally** (`compileBlock.ts:143`). So a pen — not tracking the tangent
at all — has its path acceleration cut by `rad(aAccel)/κ` on every curve:

```
fixture              worst ratio   mean ratio     (segAccel with A ÷ without)
quarter_circle_r5       0.124        0.155
full_circle_r30         0.749        0.923
s_curve                 0.524        0.895
```

An 8× accel loss on a 5 mm arc, for an axis that is not moving. The asymmetry is
called deliberate in `compileBlock`'s header; the cost of it is not stated
there. This is a tuning question, not a correctness one — but it is worth
settling *before* the port, since the gate lives in the config bridge and the
port is the moment that bridge gets rewritten.

### Test rewrite

`plan.test.ts` went from 12 tests to 48, on the same INVARIANTS / CONTRACT
PROPERTIES split as `flatten` and `constrain`. What the old suite lacked:

- No purity, determinism, or sample-preservation check.
- No monotonicity: nothing said more accel cannot plan slower.
- `segAccel` was tested with two loose assertions (`< A_MAX`, and a 10%-tolerance
  match). Now: closed forms for each axis, the exact `√2` diagonal, the A term's
  inverse-linear-in-κ shape, degenerate and unlimited-axis fallbacks, symmetry.
- Accel continuity was asserted only by recomputing `segAccel` — self-consistent,
  and blind to `segAccel` itself being wrong. That check is kept (it validates
  the sweeps) and paired with `axisAccelViolations`, which re-derives
  acceleration from planned speeds and geometry alone. **P1 is what the second
  measurement found and the first could not.**
- Fixture loops used `expect` inline, stopping at the first violation. Now
  `forEachFixture` aggregates and fails once.

Two closed forms replaced "goes down"-style assertions, both of which had been
wrong in an earlier draft of this file and were corrected by running them:
triangular peak is `sqrt(a·L)`, and a forced stop's decel ramp reaches back
`v²/2a` (to within one `dsMax` of sample quantisation).

### Mutation-validated

Thirteen deliberate breaks of `plan.ts`; all thirteen caught.

```
backward sweep: drop the 2*a*ds term        7      segAccel: A term uses min kappa      2
forward sweep removed                       7      segAccel: pathAccel ignored          2
backward sweep removed                      9      segAccel: y axis ignored             2
endpoints not pinned to zero                7      segAccel: degenerate returns 0       2
sqrt dropped in backward sweep              8      ds off by one in forward sweep       2
segAccel: per-axis projection dropped       2      subpathRanges: END not closing      11
segAccel: A term uses sqrt shape           22
```

Two methodology notes worth keeping:

- **Count the failing test *names*, not the number of failures.** The first run
  of this harness reported two survivors. Both were false: a mutation had
  flipped a known-red finding test green while turning another red, netting zero
  change in the count. Comparing name sets against baseline found them.
- **"y axis ignored" initially survived a test written to catch it.** The default
  config is square (`x.maxAccel == y.maxAccel == aMax`), so dropping the Y
  candidate returned the same number via the scalar fallback. Fixed with an
  explicitly non-square option set. Any test whose expected value coincides with
  a fallback proves nothing.

---

## Stage 8 — `discretize`

The bottom of the pipeline: what leaves here goes on the wire, so a defect here
is a defect on metal. It is also the largest stage by responsibility — per-pair
emit, velocity-aware subdivision, tangent tracking, and four choreography
transitions — and it had **six** tests before this pass.

### What holds (verified)

Purity, determinism, XY conservation (every fixture × both tools, multi-subpath,
and a real SVG through the repair chain), A conservation for a tangential tool,
integer deltas, `interval ∈ [1, fCpu]`, one `PATH_END` per subpath, per-axis
invert, and the `dvMax` subdivision bound. The float-accumulator-round-at-emit
design telescopes correctly — that is the stage's best idea and it works.

### D2 — sub-segment speed is interpolated linearly in distance

`discretize.ts:183-185` interpolates the speed across a sub-segment as

```
v(f) = v0 + (v1 - v0) * f          f = fraction of ARC LENGTH
```

Under constant acceleration — exactly what `plan`'s sweeps produce — speed is
not linear in distance:

```
v(s) = sqrt(v0² + 2·a·s)     i.e.    v(f) = sqrt(v0² + f·(v1² - v0²))
```

The consequences are sharp, and the isolation is clean (measured on a PEN, so no
A axis, on a plain 10 mm line):

```
dvMax        inf      24       6       3      0.75
ratio      1.0000  1.1027  1.2681  1.3608  1.5104      (emitted ÷ exact time)
```

At `k = 1` the pair-level mean `(v0+v1)/2` is **exactly** right for constant
acceleration, so the emitted time is exact. Every subdivision replaces that one
exact estimate with `k` wrong ones, and the error grows **monotonically the
harder it subdivides**. Subdivision exists to improve fidelity (premortem P3);
for timing it does the reverse, and the knob meant to buy accuracy is the knob
that spends it.

Worse at a ramp leaving rest: the linear model's time integral is

```
t = ds · ln(v1/v0) / (v1 - v0)      →  diverges as v0 → 0
```

against the true `ds / ((v0+v1)/2)`. For `v0 = 0, v1 = 10, ds = 0.5` that is
15× the correct time. `quality.vMin` is the only reason the number is finite —
that clamp is load-bearing by accident, which is also why C1's `vMin` story and
this one are entangled.

**The fix is small and local**: interpolate `v` as `sqrt(v0² + f·(v1² - v0²))`.
Each sub-segment's own mean then becomes exact and the sub-times sum back to the
pair time. Worth doing **before** the port — it changes emitted intervals, so it
needs one deliberate re-golden, and doing that once is cheaper than doing it
after a C++ rewrite has been validated against the wrong numbers.

Why the golden never caught it: goldens are long SVG paths, which cruise. The
error is concentrated where ramps dominate — 1.36× on a 10 mm line, 1.01× on a
500 mm one.

### D1 — an empty segment carrying a one-second interval

The interior-skip guard (`discretize.ts:177`) exempts two cases from being
skipped: a subpath's final sub-step, and a corner's last sub-step. Both can have
every delta zero, and `interval`'s `if (major === 0) return fCpu` then hands the
empty segment the largest interval representable:

```
dx = dy = dz = da = 0,  interval = 150,000,000  = one full second at fCpu
```

Both exemptions are reachable, and the second needs no strange geometry at all:

| trigger | reached by |
|---|---|
| corner | `cusp` + KNIFE — dx/dy zero (coincident samples), da zero (tracking gated on `!isCorner`) |
| final | `long_gentle_arc` + **PEN** at `dvMax = 0.05` — the last sub-step rounds to no motion and is emitted anyway because it carries `MICRO_PATH_END` |

So the marker that ends every path can itself be the empty segment. Whether the
firmware stalls a second on a zero-step segment or discards it is a wire-contract
question this stage should not be leaving open.

### D3 — `interval`'s rate floor is a second speed governor

`interval` floors each segment's duration so no axis exceeds
`maxFeed × stepsPerUnit`. The floor is correct and necessary. The problem is that
it is applied **after** planning and nothing upstream knows it fired: where it
binds, the executed timeline is slower than the planned one, and everything
derived from the plan's timeline is wrong with it — including the window stage 9
schedules the knife's enable-line resets against (`tool_duty_limits.md` §5).

The root-cause chain, measured rather than assumed:

```
flatten's tangent cap overshoots (F7)
    → actual sample-to-sample turn exceeds kappa*ds   (1.64x on near_cusp, 8.17x on cusp)
constrain's A-slew cap is computed from kappa
    → it under-caps v
plan's timeline asks A for up to 16.8x its rate ceiling
interval silently rescues it by stretching the segment
```

This is the second finding in this audit (with P1) whose cause lives in one stage
and whose symptom appears in another. Both were invisible to per-stage review.

### D4 — the corner rule is ungated, unlike constrain's

`constrain` gates its corner-stop on `(flags & CURVE_BOUNDARY)`; `discretize`
gates on nothing (`discretize.ts:137-138`). So `discretize` will lift-pivot at an
intra-curve tangent jump that `constrain` never stopped for — contradicting this
stage's own header ("velocity planning already brought the tool to v=0 at every
corner"). This is F2, now measured.

The gap is currently narrow: the only geometry that reaches it is a cusp, where
the curvature caps happen to have crawled `v` down anyway (4.8e-3 mm/s). So the
lift-pivot precondition holds **by accident, not by construction** — and D2's
`vMin` floor lifts that crawl to 0.5 mm/s regardless, so the pivot does execute
while moving.

Filed rather than dismissed because the accident is F1's doing: a cusp is exactly
where `flatten`'s tangent cap is skipped. Fix F1 so the marcher resolves cusps
properly, and this stops being a cusp-only case.

### D5 — the Z lift path was dead code under test — RESOLVED

Every tool profile ships `liftHeight = 0`, so `lift` is false and **nothing** in
the Z path executed: no lower-to-cut, no raise-at-end, no lift inside a corner
pivot. The lift-pivot-lower that the entire corner design rests on had never
actually lifted under test.

Found by mutation testing, not by reading: deleting the lower-to-cut line changed
nothing. Now covered through the documented `liftHeight` override — matched
lower/raise, net Z zero across many subpaths, the pivot's interior lift/lower
pair, and XY conservation under lift.

### Test rewrite

6 tests → 32, on the INVARIANTS / CONTRACT PROPERTIES split.

The measurement that mattered: `emittedSeconds` re-derives wall time from
`interval` and step counts **the way the firmware will**, not the way
`discretize` computed it. D2 and D3 are both only visible from that side of the
boundary. This is the same lesson as P1 — a check written in the implementation's
own terms cannot see the implementation's own error.

### Mutation-validated

Nineteen deliberate breaks of `discretize.ts`; all nineteen caught.

```
X invert not applied              5    position accumulator rounded      3
Y invert not applied              1    corner A rotation lost from aPhys 1
Z lift never lowered              3    pivot not emitted at corner       3
Z raise after stroke skipped      3    zero-motion skip removed          1
A invert not applied              2    vbar uses entry speed only        1
A tracking disabled               1    Z lower before cut skipped        3
corner detection disabled         2    preOrient skipped                 3
subdivision disabled (k=1)        2    travel jog skipped                1
PATH_END never set                3    theta not advanced per pair       4
dx rounding -> trunc              3
```

Three methodology notes, all of which changed the tests:

- **Two survivors were equivalent mutants — and one was a real finding.** The
  default machine has `y.invert = false`, so dropping the Y-invert branch is a
  no-op; and every profile has `liftHeight = 0`, so the whole Z path is dead.
  The first needed a non-default config to test against; the second **is** D5.
  An equivalent mutant is not always noise — sometimes it is telling you the
  production config never exercises the code.
- **A red finding test masks mutations in its own area.** "zero-motion skip
  removed" first showed as surviving because D1 was already failing, so no
  *newly* failing test appeared. The fix was a companion test scoped to the
  interior case only, which stays sensitive while D1 stays red.
- **`file` before `grep`.** A mutation pattern silently failed to apply for
  three runs because the source has CRLF endings and git-bash `grep` strips the
  `\r` from its output. `cat -A` through `grep` lied; `file` did not.

---

## `choreograph` — the non-cutting emitters

Not a pipeline stage: a peer module (`src/choreograph/choreograph.ts`) holding
every move that is **not** cutting — Z lift/lower, ramped A rotation, the
lift-pivot-lower at a corner, the travel jog between subpaths, A pre-orientation
at `PATH_START`, absolute A moves for homing and revolver slots, and the
head-offset compensation jog.

It was audited *after* `discretize` because `discretize` calls into it, and the
discretize audit could only see it through that one caller. Its other callers
(`orchestrate/walk.ts`, `orchestrate/schedule.ts`, `production/dutyBreaks.ts`,
`production/compileBlock.ts`) reach it directly.

### Why the existing tests missed all of this

The 26 tests here were **shape** tests: right flag, right sign, right count,
intervals within `[1, fCpu]`, steps telescoping to `|da|`. Every one of them
passes today and every one is worth keeping. Not one asked what the emitted
segments would *do* on a machine.

The measurement that found H1–H4 is the same one that found D2 and D3:
reconstruct the motion the way the **firmware** executes it — `|steps|` clocked
at `interval` cycles apiece — and ask whether a machine with the configured
limits could follow it. Expressed that way the emitter's own belief about its
velocity never enters the check.

```ts
function slices(segs) {                     // the firmware's view
    return segs.map(s => ({ steps: major(s),
                            v:  fCpu / s.interval,
                            dt: major(s) * s.interval / fCpu }));
}
// a rate change at a slice boundary is instantaneous; a machine limited to
// `limit` can only follow it if the change fits in the PRECEDING slice
worst = max(|v[i] - v[i-1]| / dt[i-1]) / limit
```

### H1 — `aMove`'s trapezoid brakes harder than the axis can, and never stops

`aMove` walks the rotation in chunks, choosing a rate for each from the distance
already travelled (`n`) or remaining (`N - n`):

```ts
if      (n < dAcc)      v = sqrt(v0² + 2·accel·n);        // accel ramp
else if (n >= N - dAcc) v = sqrt(v0² + 2·accel·(N - n));  // decel ramp
else                    v = cruise;
const chunk = min(max(1, trunc(v / 100)), N - n);
```

`v` is sampled at each chunk's **start**. On the way up that is the slowest
point in the chunk, so holding it for the whole chunk under-drives the axis —
conservative. On the way down the same point is the **fastest**, so holding it
over-drives — anti-conservative. One line, opposite sign depending on which
ramp you are on.

Measured against `a.maxAccel = 2000 deg/s²` (103334 steps/s²):

| N (steps) | deg | accel ramp | decel ramp | ends at |
|---|---|---|---|---|
| 52 | 1 | 0.73× | — *(never decelerates)* | 39.36 deg/s |
| 129 | 2.5 | 0.86× | **1.32×** | 27.84 deg/s |
| 258 | 5 | 0.88× | **1.33×** | 26.41 deg/s |
| 500 | 9.7 | 0.88× | **1.45×** | 17.62 deg/s |
| 1000 | 19.4 | 0.88× | **1.65×** | 8.85 deg/s |
| 2325 | 45 | 0.88× | **1.65×** | 8.85 deg/s |
| 4650 | 90 | 0.88× | **1.31×** | 29.20 deg/s |
| 9300 | 180 | 0.88× | **1.26×** | 35.21 deg/s |
| 18600 | 360 | 0.88× | **1.48×** | 15.27 deg/s |

Two separate consequences, so three red tests:

- **H1a** — the decel ramp demands 1.26–1.65× the configured A acceleration, on
  every size. The accel ramp never exceeds 0.88×.
- **H1b** — the move is designed to ramp down to `v0 = min(cruise, 50)` = 50
  steps/s = **0.97 deg/s** and then stop. It actually stops from between 8.85
  and 39.36 deg/s — up to **40× the intended terminal velocity**, as a hard
  stop. Note the column does not fall off with size; it is set by where the
  chunk grid happens to land.
- **H1c** — at `N = 52` (a 1° pivot, five chunks) the single "decel" chunk is
  *faster* than the cruise chunk before it: `50 → 457 → 1018 → 1761 → 2034`.
  The rotation **accelerates into its final chunk and then stops.** This is the
  clearest statement of the cause.

The obvious one-line remedy does not work. Sampling the decel rate at the chunk
**end** instead fixes the terminal velocity exactly (50 steps/s at every size)
but makes the acceleration worse, up to 2.28×, because the tail then runs in
1-step chunks whose durations are tiny. Measured, not assumed. A real fix has to
choose the chunk boundaries from the accel limit rather than from `trunc(v/100)`
— which is to say the `100` is the thing to remove.

Where it bites: `preOrient` runs at every `PATH_START` and `pivot` at every
tangential corner, so this is on the hot path for every knife job.

### H2 — travel jogs ignore the acceleration limit entirely

`travelJog` emits exactly one segment for the whole move, at full `jogFeed`:

```
200 mm jog → dx = 32000 steps, interval = 11718 → 12800 steps/s = 80 mm/s
```

`x.maxAccel = 1000 mm/s²` is configured, respected everywhere in the cutting
path, and not consulted here. Reaching 80 mm/s at that limit needs 0.08 s and
**3.2 mm** of ramp; the jog allows zero. The same is true of `headOffsetJog`,
which runs at every tool change.

Unlike H3 there is no TODO acknowledging this, and unlike H3 the limit it
violates is a real configured number rather than an uncharacterized 0.

### H3 — `zMove` is unramped too (known)

One segment, 0 → 24000 steps/s instantly. Already acknowledged by the TODO at
the top of the module. Recorded as a red test so it is *counted* rather than
only commented — and because `z.maxAccel` is `0`, characterizing the Z axis is a
prerequisite for fixing it. Nothing can check the fix until that number exists.

### H4 — `aMove` invents machine limits `load.ts` would refuse to invent

```ts
const cruise = Math.max((feed > 0 ? feed : 180) * aSpd, 1);
const accel  = Math.max((rate > 0 ? rate : 2000) * aSpd, 1);
```

`0` means "uncapped" for `maxFeed`/`maxAccel` (see `defaults.ts`), and
`DEFAULTS.axis` ships both as `0`. So a machine that declines to state its A
limits gets 180 deg/s and 2000 deg/s² substituted silently, at the emitter.

Measured: with `a.maxFeed = a.maxAccel = 0`, `aMove` peaks at **9301 steps/s =
180 deg/s** — exactly the invented floor, and **1.8× the real machine's stated
100 deg/s ceiling**. Declaring the axis uncapped makes it run *faster* than
declaring its true limit.

`load.ts`'s header is explicit that `stepsPerUnit`, `invert` and node ids have
no silent fallback, because guessing calibration is how you crash a machine.
These are the same class of number under the opposite policy. Whichever way it
is resolved, the two files should agree.

### H5 — three guards defending the same thing (cleanup)

Mutation testing left three survivors in `aMove`, all equivalent mutants and all
the same redundancy — `v` is floored at `v0` three times over:

| Removed | Why it changes nothing |
|---|---|
| `if (N === 0) return [];` | the `while (n < N)` loop already emits nothing |
| `v = Math.max(v, v0);` | both ramp branches already return ≥ `v0` |
| the `[1, fCpu]` clamp on `interval` | `v ∈ [50, cruise]` ⟹ `fCpu/v ∈ [29033, 3e6]`, always in range |

Harmless, but the third is load-bearing-looking code that cannot fire, and in
C++ that reads as a guarantee the port would be entitled to rely on. Worth
deleting *or* keeping with a comment saying it is belt-and-braces — not left
ambiguous.

### Test rewrite

26 → 56 tests, on the same INVARIANTS / CONTRACT PROPERTIES split as the stages.
All 26 originals survive in substance; the additions are the kinematic ones.

Verified holding: purity and determinism; step conservation across every size
and both signs (including that `newAPhys` always matches the steps actually
emitted, for both `preOrient` modes and `aMoveTo`); `pivot`'s Z lift and lower
cancelling exactly; integer deltas; `interval ∈ [1, fCpu]`; per-axis invert
isolated to its own axis and presentation-only; `preOrient`'s unwind mode
bounding `|aPhys|` no matter how far a cut wound A, and its non-unwind mode
always taking the shortest way round; `pivot` ordering lift → rotate → lower;
`travelJog` and `headOffsetJog` antisymmetry; `zMove` and `travelJog` timing
matching the requested feed; and `aMove` reaching — but not exceeding — the A
feed ceiling.

### Mutation-validated

34 deliberate breaks, 31 caught, 3 survivors — all three the equivalent mutants
of H5 above, i.e. the surviving mutations *are* the finding.

The first pass had 11 survivors. Eight were genuine test gaps, closed by adding:
`zStepCount` rounding on a non-integral height; `zMove`'s interval clamp at
absurd feeds; a long `aMove` actually *reaching* the feed ceiling (not merely
staying under it); an upper as well as a lower bound on total time against the
analytic trapezoid; the triangular clamp on short moves; a decel-exists
companion to H1b; a bound on segment count; and `travelJog` rounding each
endpoint rather than truncating.

Two process notes, both repeats of lessons from earlier stages:

- **A red finding test masks mutations in its own area.** `H1b` being red hid
  "decel branch removed" completely. The companion test — does a ramp-down
  exist at all, ignoring whether it is steep enough — catches it. Same shape as
  the `D1` / zero-motion-skip problem in stage 8.
- **The CRLF trap again**, and a new one: piping the mutation harness to `head`
  sends SIGPIPE and kills it *mid-mutation*, leaving the source file modified.
  That corruption then looked like four new test failures. `git diff --stat
  web/src/` before trusting any result — and never `| head` a script that edits
  files in place.

---

## Current test state

`npx vitest run test/toolpath/flatten` — 20 passing, 4 red, all 4 intentional:

```
× cap 2 — spacing <= dsMax          7 violations (F7)
× cap 3 — tangent turn <= dthetaMax 3 violations (F1 + F7)
× snake.svg — spacing cap holds     142 of 356 steps (F7)
× snake.svg — tangent cap holds     2.062deg vs 2.0 (F7)
```

`npx vitest run test/toolpath/constrain` — 39 passing, 0 red.

`npx vitest run test/toolpath/plan` — 43 passing, 5 red, all 5 intentional:

```
× no axis is asked for more acceleration than it has            3 fixtures (P1)
× refuses, or plans, a stream with no PATH_START/PATH_END       (P2)
× does not drop a subpath whose PATH_END is missing             (P2)
× spends no meaningful arc length below vMin                    2 fixtures (P3)
× introduces no unexecutable speed of its OWN                   cusp only (P3)
```

`npx vitest run test/toolpath/discretize` — 27 passing, 5 red, all 5 intentional:

```
× emits no segment with zero motion on any axis                 cusp/knife (D1)
× reaches the PATH_END marker on ordinary geometry              pen/arc    (D1)
× emitted cut time matches the exact constant-accel time        3 fixtures (D2)
× the plan never asks A for more than its rate ceiling          2 fixtures (D3)
× every corner it pivots at was stopped for by constrain        cusp       (D4)
```

`npx vitest run test/choreograph` — 50 passing, 6 red, all 6 intentional:

```
× H1a: aMove's decel ramp respects the A accel ceiling      8 of 9 sizes (H1)
× H1b: aMove comes to rest at its designed terminal velocity 9 of 9 sizes (H1)
× H1c: the shortest rotations ramp down at all               N=52         (H1)
× H2: travelJog ramps to its feed instead of stepping to it               (H2)
× H3: zMove ramps instead of slamming to zFeed                            (H3)
× H4: aMove does not invent A limits for an under-specified machine       (H4)
```

Full suite: **676 passing, 20 red**, 4 skipped; `tsc --noEmit` clean. All 20 red
are intentional and each names its finding; every other module is green.

`CUSP` is now imported directly by the `flatten`, `constrain`, `plan` and
`discretize` tests. All four stages that consume it have been audited, so it can
move into the shared `CASES` registry whenever a fifth consumer wants it.

---

## Stages pending

| Stage | Status |
|---|---|
| 3 `repair` (`enforceC1`) | not audited |
| 6 `plan` | **audited** — P1–P5 above |
| 8 `discretize` | **audited** — D1–D5 above |
| — `choreograph` | **audited** — H1–H5 above. Not a stage; audited because `discretize` could only see it through one caller |
| 9 `dutyBreaks` | not audited — next. Partially known: see `tool_duty_limits.md` §5, §11. Note it consumes the timeline D2 and D3 both corrupt, so audit those findings' impact here first |

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
