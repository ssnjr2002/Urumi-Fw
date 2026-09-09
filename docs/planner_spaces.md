# Planner Spaces

**Status:** DESIGN NOTE — describes the current pipeline and argues for a set
of re-placements. Nothing here is implemented. The measurements in §7 were
taken against the code at `b9a922c`; the defects in §5 are live.

The planner moves data through three representations. Most of the hard bugs
found so far are not bugs inside a stage — they are operations performed in
the wrong representation, where the information they need has already been
destroyed. This note names the representations, states what each boundary
costs, and gives a rule for deciding where an operation belongs.

Companion to `planner_audit.md` (which catalogues defects) and
`tool_duty_limits.md` (whose §5 tier 2 is the design this note unblocks).

---

## 1. The three spaces

**Bézier space.** Cubic subpaths in mm. Continuous, analytic, exact.
Curvature and its derivative have closed forms and can be evaluated at any
parameter. This is the input to `compileBlock`, and — importantly — it is
still in hand for the whole compile.

**Sample space.** `Sample[]`: positions along the path carrying `ds`, `kappa`
and flags, produced by `flatten` against `chordTol` and `dsMax`. Real-valued
mm. An ordered chain, which is what makes sequential reasoning possible.

**Packet space.** `MicroSegment[]`: integer per-axis step deltas plus an
integer clock interval. This is where time becomes real and where the machine
is actually commanded.

There is a fourth, the packed 26-byte wire frame, but it is an encoding of
packet space with no further loss beyond range clamping. It is not interesting
for this discussion.

---

## 2. What each boundary costs

The two boundaries are not the same kind of thing, and the difference is the
whole point of this note.

**Bézier → sample is discretisation, not quantisation.** Sample positions stay
real-valued; nothing snaps to a grid. What is lost is *between-sample*
information: a curvature peak between two samples is invisible, and that loss
is bounded by `chordTol`. Crucially it is **recoverable** — the Béziers still
exist, so any region can be re-evaluated or re-sampled at any time, at any
density.

**Sample → packet is genuine quantisation, and it is irreversible.** Position
snaps to the step grid (160 steps/mm on X and Y), time to clock cycles
(150 MHz), A to 51.667 steps/deg. Many sample streams map to the same packet
stream; the map cannot be inverted, and what fell between the grid lines is
gone.

Everything in §3 follows from that asymmetry.

---

## 3. The rule

> **Information flows down. Decisions flow up.**
>
> Packet space is where you *measure*. Sample and Bézier space are where you
> *decide*. Never edit in packet space — not because it is difficult, but
> because the information required was destroyed on the way in.

Two corollaries worth stating separately, because both were learned the hard
way:

**Carrying is not reconstructing.** A quantity computed upstream may be
carried across a boundary as data and used downstream. What fails is
attempting to *recompute* it downstream from what survived. `segAccel` is the
worked example (§7.2).

**Re-sampling is always available.** Because the first boundary is lossless,
"a later stage needs detail an earlier one did not produce" always has an
answer: go back to the analytic curve and ask for more points. There is no
equivalent move at the second boundary.

---

## 4. Where each operation belongs

Legend: ✓ correctly placed · ⚠ misplaced or missing.

### Bézier space — continuous, exact, re-evaluable

| Operation | Today | Belongs in | |
|---|---|---|---|
| C1 repair / gap close (`enforceC1`) | Bézier | Bézier | ✓ |
| Tool-offset shift | Bézier | Bézier | ✓ exact translation |
| Corner detection (tangent jump) | Bézier joins → sample flags | Bézier | ✓ from control points |
| Adaptive sampling density | the boundary itself | Bézier | ⚠ one-shot; should be re-runnable per region |
| κ evaluation | sample | **Bézier** | ⚠ |
| κ′ (curvature gradient) | sample, finite difference | **Bézier** | ⚠ |

### Sample space — ordered chain, real values

| Operation | Today | Belongs in | |
|---|---|---|---|
| Centripetal ceiling `sqrt(aMax/κ)` | sample | sample | ✓ |
| Curvature-gradient ceiling | sample | sample | ✓ |
| A-slew ceiling `ω/κ` | sample | sample | ⚠ too loose |
| Forced stops | sample | sample | ✓ right place, unused |
| `segAccel` budget | sample | sample | ✓ but carried, not discarded |
| Centripetal accel reduction (audit P1) | sample | sample | ✓ |
| Backward / forward feasibility sweeps | sample | sample | ✓ |
| Subpath bracketing | sample | sample | ✓ |
| Decide *where* lifts happen | sample | sample | ✓ |
| Time per sample | — absent — | **sample** | ⚠ |
| Decide where duty breaks happen | packet (stage 9) | **sample** | ⚠ |

The sweeps deserve a note: sample space is the *only* space that has both an
ordered chain to iterate over and real-valued speeds to iterate on. Bézier
space has no chain; packet space has quantised values and arrives too late.

### The boundary — sample → packet

| Operation | Today | Belongs in | |
|---|---|---|---|
| Subdivision count `k = ceil(\|Δv\|/dvMax)` | sample | **boundary, iterated** | ⚠ |
| Per-axis rate floor (`tRate`) | packet, silent clamp | **sample as ceiling, packet as assertion** | ⚠ |

### Packet space — quantised, irreversible, but time is real

| Operation | Today | Belongs in | |
|---|---|---|---|
| Step-delta quantisation + residue carry | packet | packet | ✓ nothing else can |
| Interval computation | packet | packet | ✓ |
| Choreograph realisation (`rampChunks`) | packet | packet | ✓ |
| Duty marker stamping | packet | packet | ✓ it is a flag on a packet |
| *Measure* true timing (`segmentSeconds`) | packet | packet | ✓ only place it is real |
| Wire packing, sequence numbers | wire | wire | ✓ |

---

## 5. The misplaced operations

**5.1 κ should be analytic.** Curvature has a closed form on a cubic.
Evaluating it only at sample points hides peaks between samples. Both
`constrain`'s ceilings and `plan`'s `segAccel` consume κ, and both actually
want *max κ over an interval* rather than κ at a point — `segAccel` already
approximates this with `Math.max(s0.kappa, s1.kappa)`, which is a two-point
estimate of something computable exactly.

**5.2 κ′ is the pipeline's shakiest input.** `kappaPrime` is a finite
difference over neighbouring samples on an *adaptive* grid, feeding the
ceiling `sqrt(rad(aAccelDegS2)/|κ′|)` — which tightens without bound as κ′
grows. A numerical derivative on a non-uniform grid is precisely where noise
becomes a spurious speed cap. Analytic κ′ removes the failure mode entirely.
*Judgment, not measurement: this has not been observed misbehaving.*

**5.3 The A-slew limit is enforced in two spaces and they disagree.**
`constrain` caps `v ≤ ω/κ` using continuous local κ. `interval()` independently
floors segment time with `tRate`, using the *quantised integer* `da` over a
packet. The packet-space form is stricter, because a packet's `da/ds` can
exceed local κ through both step rounding and curvature varying within the
segment.

The consequence is measured in §7.4: the floor engages on 26–31% of knife
cutting packets, and every `dvMax` violation in a fast stretch sits at an edge
where it engages or disengages. `plan` produces a smooth profile; the clamp
then carves a notch out of it with vertical walls, because nothing ramped into
a constraint `plan` never saw.

This is structurally the same defect as audit P1 — two components each
bounding one thing correctly, nobody owning the seam.

**Fix.** Two candidates.

*Option A — give `constrain` the quantisation margin.* With `q` = one A step in
radians, require `v ≤ ω·ds/(κ·ds + q)`, i.e. the existing ceiling scaled by
`1/(1 + q/(κ·ds))`. Cheap. The wrinkle is that `ds` there is the *packet*
length and `constrain` runs before subdivision, so it can only bound it
conservatively — a margin tuned against a quantity the stage cannot observe,
which is the same kind of seam that produced the bug.

*Option B — one feedback pass.* Emit, find the clamped packets, fold their
effective speeds back into the sample ceilings, re-run. Exact, no fudge
factor, and it terminates because the clamp only ever lowers v. Costs one
extra compile per block; a test can assert the fixed point directly.

**B is recommended.** Beyond correctness, it makes `plan`'s profile *true* —
after it, planned speeds are what the machine will actually deliver, which
matters because several other designs here rest on that identity.

Either way, `interval()`'s floor should become an assertion rather than a
silent clamp. A limit belongs where it can be planned around.

**5.4 Subdivision count.** `k = ceil(|Δv|/dvMax)` is a sample-space decision
about a packet-space property. It is the one row where neither space alone is
right: it has to be proposed, emitted, checked, refined.

**5.5 Time per sample is missing.** `dt = 2·ds/(v[i] + v[i+1])` is exact under
constant acceleration, and `plan` holds both terms the moment its sweeps
finish. Guards needed: `0/0 → 0` on zero-length corner pairs, and the same
`vMin` floor `interval()` applies. Without it, sample space cannot cost its
own decisions, which is what made duty scheduling awkward (§6).

**5.6 Duty break placement.** Currently both chosen *and* realised in packet
space. Choosing legitimately needs packet-space measurement — that is where
timing is real. Realising must move upstream to `constrain.forcedStops`, where
a re-plan can produce a correct ramp and re-sampling can supply the density
that ramp needs. See §7.3 for why the in-place alternative is dead.

---

## 6. What this means for the pipeline's shape

The obvious conclusion — "the linear stage chain is the bottleneck" — is
mostly wrong. Of the misplaced operations above, only **subdivision** actually
requires the pipeline to iterate. The rest are re-placements: moving an
operation to the space where its inputs are exact. Those need different call
sites, not feedback edges.

What the linear design is carrying should not be given up lightly:

- the byte-parity snapshot test depends on a fixed pipeline;
- the `Sample → ConstrainedSample → PlannedSample` type progression makes a
  skipped `constrain` a compile error rather than a silent full-feed-from-rest;
- every stage being pure is what makes them testable in isolation.

So the change is narrow: **the operations stay pure and stay ordered; the
driver may run the chain more than once.**

That is safe here for a specific reason. Every feedback edge identified has
the same form — *packet space discovered a ceiling that was too high; lower it
and re-run*:

- the A clamp only ever lowers v;
- a forced duty stop only ever lowers v;
- `constrain`'s ceilings only ever lower v.

The system is therefore a **descending fixed-point iteration on the velocity
ceiling field**. Ceilings only fall and are bounded below by zero, so it
terminates. In practice it should terminate in one extra pass, because the
perturbations are local (§7.1).

This single frame subsumes three designs previously treated as unrelated: the
A-clamp fix, duty tier 2, and the re-plan question.

---

## 7. Evidence

Fixtures are sinusoidal snakes at three tightnesses, compiled through
`compileBlock` for both `PEN` and `KNIFE` on the default config
(`fCpu` 150 MHz, X/Y 160 steps/mm, A 51.667 steps/deg, A cap 5166.7 steps/s,
`dvMax` 3.0).

**7.1 Speed is recoverable per packet, and stops are local.** CONFIRMED.
`v = hypot(dx/xSpu, dy/ySpu) / (major·interval/fCpu)`. Pen 791 packets, 68.4%
exact, remainder off by one clock cycle, **zero worse**, zero axis-rate
governed; knife 844 packets, 83.6% exact, same tails.

Forcing a stop perturbs the profile only within the ramp distance:

| fixture | back | forward | predicted `v²/2a` |
|---|---|---|---|
| gentle (pen) | 3.24 mm / 13 samples | 2.97 mm | 3.20 mm |
| gentle (knife) | 1.98 mm / 8 samples | 1.98 mm | 3.20 mm |
| tight | 0.86 mm / 3 samples | 0.49 mm | 1.12 mm |
| tighter | 0.55 mm / 2 samples | 0.49 mm | 0.69 mm |

**7.2 `sqrt(2as)` as a closed form is FALSIFIED.** Overshoot of a rewritten
ramp against the swept truth, always in the unsafe direction:

| fixture | `a = 1000` | `a = 0.9·1000` | curvature-corrected |
|---|---|---|---|
| gentle | +0.27 mm/s | 0 | 0 |
| tight | +2.56 | +1.27 | +2.50 |
| tighter | +4.31 | +4.26 | +4.28 |

Cause, decomposing `segAccel`'s minimum at the tightest ramp point:

```
gentle   budget 1000.0 = min(x:—, y:—, A:1652)      κ=0.021
tight    budget  597.8 = min(x:2512, y:1090, A:598) κ=0.058
tighter  budget  353.8 = min(x:2568, y:1086, A:354) κ=0.099
```

The binding term is the A-axis tangential-tracking limit `rad(aAccel)/κ`,
which scales as `1/κ`. No constant or safety factor can bound it. This is the
canonical instance of the rule in §3: the quantity was reconstructible only
from information that does not cross the boundary.

**7.3 In-place ramp rewriting in packet space is not viable.** Forcing a stop
and re-sweeping over the *existing* packet boundaries:

| fixture | a=354 | a=600 | a=1000 |
|---|---|---|---|
| pen gentle | 13.3 (4.4×) | 17.3 (5.8×) | 22.4 (7.5×) |
| pen tight | 13.4 (4.5×) | 17.5 (5.8×) | 22.6 (7.5×) |
| knife gentle | 13.3 (4.4×) | 17.3 (5.8×) | 22.4 (7.5×) |
| knife tight | 16.6 (5.5×) | 21.6 (7.2×) | 27.8 (9.3×) |

Against `dvMax = 3.0`. The worst offender is always the last packet before the
stop, which must jump `sqrt(2a·ds)` straight to zero. With `ds` ≈ 0.07–0.15 mm
this is 6–28 mm/s in one packet and no budget rescues it. A ramp needs
*increasing* packet density approaching zero; the un-stopped bake has no
reason to have put density there. Re-sampling (§3) is the only real answer.

**7.4 The A clamp, and where its discontinuities come from.**

| | dvMax violations (fast stretches) | packets at the A cap |
|---|---|---|
| pen gentle | 0 | 0 |
| pen tight | 20, worst 3.09 | 0 |
| knife gentle | 104, worst 6.86 | 516 / 1669 (31%) |
| knife tight | 40, worst 4.07 | 880 / 3336 (26%) |

Classifying the violations by whether the clamp was active:

```
knife gentle:  both-capped 0 | one-capped 100 | neither 4
knife tight :  both-capped 0 | one-capped  40 | neither 0
adjacent capped pairs: |da| differs by <=1 in 168/168 and 320/350
```

**Zero violations occur while A is capped.** Inside the clamped region the
profile is smooth. Every violation is at an engage or disengage edge — which
is what proves the clamp value and `constrain`'s ceiling disagree, rather than
the clamp merely being noisy.

The pen has no A-capped packets and its worst violation is 3.09 against 3.0 —
3% over, matching what step quantisation alone predicts (0.99 mm/s, plus
interval truncation). Quantisation is a real but minor contributor; it is the
whole story for the pen and roughly a fifth of it for the knife.

**7.5 Interval truncation is one-signed and negligible.** Cutting packets use
`Math.trunc` on the interval, so the machine runs each marginally *faster*
than planned — bounded by `Σmajor / fCpu`, sub-millisecond per 30 s burst.
`rampChunks` uses `Math.round` and contributes no systematic component. An
open-loop timing model over a burst is therefore sound, and conservatively
biased (predicted ≥ actual).

---

## 8. Live defects found alongside this work

Independent of everything above, and independent of each other.

1. **Silent no-op duty break.** `runWalk` calls `hooks.onDutyBreak?.(...)`.
   With no hook installed the batch still splits, the machine still parks and
   still resumes — a textbook-looking break that never touched the relay. The
   tool runs through its limit and fails later, far from the cause. Should
   throw.

2. **An operator pause leaves the tool powered.** The peripheral enable
   follows the mounted phase, not the machine state (`web/demo/comms.js:1747`).
   The baked schedule counts *motion* time; the hardware limit is *wall* time.
   A coffee break inside a 30 s burst blows the 40 s limit outright. This
   invalidates the scheduling premise regardless of which design wins, and is
   the only one of the three that can damage a tool.

3. **`interval()`'s pure-rotation fallback uses X's steps-per-unit.** For a
   pure A or Z move with no rate floor, `majorRate()` computes `v * xSpu` —
   mixing units. Currently unreachable (all pure single-axis moves go through
   `rampChunks`, which never calls `interval()`), and guarded even if reached,
   but it is the obvious trap for a future caller. Delete the fallback in
   favour of a throw, or document that pure single-axis moves must go through
   `rampChunks`.

---

## 9. Open questions

- **Does one extra pass suffice?** Locality (§7.1) and monotonicity (§6) both
  support it, but the full loop with multiple interacting constraints has not
  been measured. This is the load-bearing assumption of §6.
- **Is κ′ actually noisy in practice?** §5.2 is reasoning, not measurement.
- **Is the A-clamp defect worth fixing?** It is a ~17% velocity step at clamp
  edges, order 100 times per block, knife only. Whether that is visible in the
  cut is a shop question, not a code one.
- **The `onDutyBreak` signature cannot express masked dwell.** `findIndex`
  matches `RELEASE | ASSERT` as one condition, so the §10 split layout fires
  the hook twice with no way to tell "cut power now" from "restore power now".
  Cheap to close by passing the matched flags through; must be closed before
  masked dwell is built.
