# Planner Audit — findings before the C++ port

**Started:** 2026-07-29
**Status:** In progress — every stage in the port's scope is audited; fixing in batches (see *Fix batches*).

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
3. **Port bug-for-bug.** ~~Output should match modulo `float32` rounding.~~
   **Superseded** — see *Numeric porting rule* below: the port stays in
   `double`, so the criterion is byte-IDENTICAL, not "close". Any
   structural difference is a transcription bug — a binary signal needing no
   judgement. The failing property tests stay red in both implementations, which
   is *evidence the port is faithful*.
4. **Fix in C++ once**, property tests as the gate. The golden moves then, with
   one variable in play.

**What would change this:** if a later stage turns out to be *actively* wrong on
the machine rather than latently wrong, "preserve today's behaviour" stops being
a useful baseline and that fix should go in immediately, ahead of the port.

---

## Numeric porting rule

**Decided:** 2026-07-29. This section AMENDS the sequencing decision above, which
assumed the port would narrow to `float32` and that C++ output would therefore
match TypeScript only *"modulo float32 rounding"*.

That assumption was never measured. It is measured now, and it does not hold.

### The rule

| quantity | type | why |
|---|---|---|
| geometry & kinematics — coordinates, velocities, accelerations, curvature, angles | **`double`** | affordable (below), and it makes the port byte-comparable to TypeScript |
| wire fields — `dx/dy/dz/da`, `interval`, `flags` | **integers** (`int32` / `uint32`) | already integral on the wire; going through float and rounding back is pure loss |
| `core1`'s per-step ramp loop | **stays `float`** | different budget entirely — see below |

`float32` is NOT used anywhere in the ported planner. TypeScript needs no
precision change, and **the golden does not move for this.**

### Why `double` is affordable

Two budgets exist on this machine and they are ~50× apart. Conflating them is
the mistake this section exists to prevent.

`core1.cpp`'s step loop carries the comment *"a bare 1.0 is a double and would
promote the expression onto the (much slower) double path"*, against a stated
`~5000-cycle step budget`. That is correct **for that loop** — it runs per STEP,
at up to 30 kHz. It is not a statement about the planner, which runs per
MICROSEGMENT.

Measured per-microsegment budget, decoded from the committed goldens
(duration = `max|steps| × interval / fCpu`):

| golden | packets | total s | mean rate | cycles/segment: p1 | p50 | min |
|---|---|---|---|---|---|---|
| `test_circle` | 824 | 4.652 | 177 /s | 272,182 | 551,592 | 213,451 |
| `fish` | 6,137 | 86.845 | 71 /s | 229,610 | 753,298 | 29,032 |

And the planner does **less than one sample's work per emitted microsegment** —
measured `samples/segment` is 0.92 (`test_circle`) and 0.67 (`fish`), because
choreograph and subdivision emit segments that cost no sampling at all.

Cost of one sample in `double`, counted from the source: `flatten` dominates at
~200 arithmetic ops plus ~2 `atan2`, ~2 `sqrt`, ~3 `hypot` (`dtAt` evaluates
`bezierDeriv1`/`2` and `curvature`; the F7 refine loop re-evaluates once or
twice); `constrain` + `plan` + `discretize` add ~100 ops and a few `sqrt`. Call
it **~300 arithmetic + ~8 transcendental** per sample.

On Cortex-M33 every one of those is a call — verified by compiling for the real
target (`-mcpu=cortex-m33 -mfpu=fpv5-sp-d16`), not from memory:

```
double kernel:  __aeabi_dadd ×4   __aeabi_dmul ×6   __aeabi_ddiv   sqrt  atan2  hypot
float  kernel:  vadd.f32  vmul.f32 ×3  vdiv.f32  vsqrt.f32         atan2f hypotf
```

The FPU is FPv5-**SP** — single precision only, so `double` is bootrom software
emulation (tens of cycles for add/mul, ~100 for div/sqrt, several hundred for a
transcendental) while `float` is one instruction. At those rates one sample
costs roughly **25,000 cycles** in `double`.

Against a p1 budget of ~230,000 that is **~11% utilisation**, and the planner
belongs on core 0 (core 1 is the time-critical streaming engine), which is
otherwise mostly idle. `float32` would cost ~2,000 cycles instead — so the
entire saving on offer is about 9% of a core that has nothing else to do.

The op count is an estimate; the budget and the codegen are measurements. The
conclusion survives the estimate being **4× too low**.

### What `double` buys, which is the actual argument

`float32` was never chosen for its own sake — it was assumed to be forced. Now
that it is optional, it is strictly worse here, because *"matches modulo
`float32` rounding"* is not a testable claim. It makes the port's pass/fail
signal a judgement call on a 3800× amplifier, where a rounding difference and a
transcription bug look the same.

In `double`, the port's criterion becomes **byte-identical to the TypeScript
golden** — binary, mechanical, needing no judgement. That is what the sequencing
decision wanted from the golden all along, and the goldens have already spent
their attribution budget on batches A–D.

This rests on the two `double` libms agreeing.

**CORRECTED — they do not.** This section originally recorded a 15-value probe
showing node/V8 and mingw g++ agreeing bit-for-bit on `hypot`, `atan2`, `acos`,
`cos` and `sqrt`. That probe was far too small: measured over 200,000 inputs
while porting `flatten`, mingw disagrees with V8 on **17.6% of `atan2`** and
**7.7% of `acos`** calls, by 1 ULP. At those rates, 15 values report a false
pass a few percent of the time, and did.

The fix is not tolerance — it is that the port **owns** `atan2`, `acos` and
`hypot` rather than taking them from whatever libm the toolchain ships. That
restores exact bit-equality AND makes the planner's output independent of the
toolchain, which matters far more: newlib on the Pico is a third answer, so
without this the firmware would not cut what the harness verified. See *Stage 4
ported* below for the measurements and `lib/motion/jsmath.cpp` for the
implementations. `sqrt` needs no such treatment — IEEE-754 mandates correct
rounding, and it measured 0 disagreements.

Byte-equality remains a **canary** rather than the contract: the contract is the
invariant tests, since a golden can say a byte moved but never why.

Host builds must force IEEE semantics — the local `g++` is `i686-w64-mingw32`,
so **`-msse2 -mfpmath=sse` is mandatory** or intermediates evaluate in 80-bit
x87 registers and drift from `double` in exactly the accumulator-heavy code the
planner is made of.

### `Math.round` is not `std::round`

Unrelated to precision, lands in the same transcription, and is silent:

```
JS    Math.round(-1.5) = -1      (ties toward +infinity)
C++   std::round(-1.5) = -2      (ties away from zero)
```

There are 24 `Math.round` sites in the port's scope and several take signed
values (`Math.round(tgtX)`, `Math.round(dxMm * stepsPerUnit)`). `std::round` is
the obvious thing to type and is wrong on half the number line; **the faithful
idiom is `std::floor(x + 0.5)`**.

Blast radius is uneven and the difference matters when triaging a diff. The
`Math.round(tgt) - Math.round(pos)` differencing pattern in `discretize` and
`xyJog` is self-correcting — it re-reads absolute position, so an error costs
one step and heals. `daTrue` and the jog step counts are not differenced that
way; those keep it.

### What would flip this

A measurement on real hardware showing the planner missing its refill deadline.
The response then is to narrow the hot path — almost certainly `flatten` alone —
to `float`, which is a localised change made against a working, byte-verified
port. Starting in `float32` inverts that: it spends a golden move up front and
permanently degrades the signal used to validate the transcription, to buy
headroom that is not currently needed.

---

## Port setup

Branch `cpp-port`. The C++ planner lives in `lib/motion/` and its tests in
`test/test_motion/`, run by PlatformIO's native test runner:

```
pio test -e native
```

`[env:native]` is the odd env in `platformio.ini` — it uploads nothing and
targets no board. `-msse2 -mfpmath=sse` there is mandatory rather than tuning:
the host `g++` is `i686-w64-mingw32`, whose default x87 path evaluates
intermediates in 80-bit registers and would quietly break the bit-equality
everything below rests on.

`lib/motion` is a normal PlatformIO library, so it is compiled into `env:pico`
only where `src/rp2350/**` includes it, and never for the AVR node envs — LDF's
default `chain` mode scans includes. Do not set `lib_ldf_mode = deep+`; that
stops being true.

doctest rather than the bundled Unity: the suite's whole method is comparing
failing test NAMES across mutations, and Unity offers neither named subcases nor
expression capture.

### The differential harness

The port's criterion is bit-equality with the TypeScript, so the two need to be
fed identical inputs with no decimal round-trip in between.
`web/test/port/cppRef.test.ts` generates reference vectors in which every number
crosses as its raw IEEE-754 bit pattern:

```
<fn> <nIn> <in..> <nOut> <out..>
GEN_CPP_REF=1 npx vitest run test/port/cppRef
```

The file carries INPUTS as well as outputs deliberately — C++ reads the inputs
and computes its own outputs, so the case list exists in one place and cannot
drift between the languages. There is **no epsilon anywhere in
`test_geometry.cpp`**; an epsilon would hide exactly the transcription slips the
file exists to catch.

`geometry.ts` is transcribed and green: **1929 assertions over 770 cases, all
bit-identical.**

### What it caught, and what it proves

On its first run the harness failed one case: `Math.round(-0.5)` is NEGATIVE
zero in JavaScript, and the obvious C++ transcription returns `+0`. Every caller
in the port's scope feeds that result to an integer step count, where the two
zeros are indistinguishable — so an exemption would have been defensible. It was
fixed instead (`copysign` on the zero branch), because a bit-equality harness is
worth having precisely because it has no exemptions to argue about, and the
first one is the expensive one to allow.

Mutation-validated, 8 mutants:

| mutation | result |
|---|---|
| `quadToCubic`: `2.0/3.0` → `2/3` (C++ integer division) | killed |
| `bezierPoint`: reassociate `3*mt*mt*t` → `3*(mt*mt*t)` | killed |
| `length`: `sqrt(x*x+y*y)` → `std::hypot` | killed |
| `angleBetweenDeg`: `(acos*180)/PI` → `acos*(180/PI)` | killed |
| `angleDelta`: `while (d > 180)` → `>= 180` | killed |
| `jsRound` → `std::round` | killed |
| `jsRound` → `floor(x + 0.5)` | killed |
| `curvature`: `(s*s)*s` → `s*(s*s)` | **survived — equivalent** |

The survivor is equivalent, not a gap: IEEE multiplication is commutative, so
both spellings evaluate to `round(round(s*s) * s)` bit-for-bit.

The four killed reassociation/idiom mutants are the load-bearing result. They
are the transcription errors a human review would wave through — the arithmetic
"obviously" means the same thing — and they are exactly what a golden diff on a
3800x amplifier cannot attribute. Catching them at the primitive is why the
port's own criterion is bit-equality rather than closeness.

### Still to port

`flatten` → `constrain` → `plan` → `discretize`, plus `choreograph`. Once
`discretize` lands, the same differential idea applies one level up: bake the
fixtures through the C++ chain and compare against the committed golden `.bin`
byte-for-byte, with no epsilon there either.

---

### Stage 4 ported — and the transcendentals had to come with it

`lib/motion/flatten.cpp` is bit-identical to the TypeScript: **32 cases, 7,325
samples, every field and flag exact.** Suite total 67,401 assertions, 0.86 s.

Getting there overturned the libm assumption recorded above, which had been
measured far too weakly.

#### The platform libm is not a shared reference

The first `flatten` run diverged on `theta` by 1 ULP. Chasing it produced this,
over 200,000 inputs spanning the magnitudes the planner works in:

| function | mingw libm vs V8 | owned implementation vs V8 |
|---|---|---|
| `atan2` | 35,247 / 200,000 (**17.6%**) | **0** — fdlibm |
| `acos` | 15,329 / 200,000 (**7.7%**) | **0** — fdlibm |
| `hypot` | 44 / 200,000 (0.02%) | **0** — V8's own algorithm |
| `sqrt` | 0 | n/a — IEEE-754 mandates correct rounding |

All disagreements are 1 ULP. The earlier claim in *Numeric porting rule* that
node and mingw agree bit-for-bit came from a 15-value probe — a sample size
that, at a 17.6% rate, reports a false pass about 6% of the time and reported
one. **Corrected in place above.**

`atan2l` rounded to double reproduces mingw's disagreement set exactly, so that
comparison does not identify which side is correctly rounded — both are
presumably the same x87 path. It does not matter which is "right": V8 is the
reference because the TypeScript is the reference.

#### Why this is a production issue, not a testing one

Newlib on the Pico is a third answer again. Left alone, the same planner source
compiled for the host harness and for the RP2350 would produce **different
toolpaths** — which defeats the stated purpose of consolidating on one
implementation, since the machine would not be cutting what the harness
verified.

So the port owns all three (`lib/motion/jsmath.cpp`), each verified
bit-identical to V8 across the same 200,000 inputs. `atan2`/`acos` are fdlibm,
which V8's `src/base/ieee754.cc` derives from. `hypot` is NOT fdlibm — V8
implements `Math.hypot` itself as scale-by-max plus a Kahan-compensated sum of
squares, which is exactly why `std::hypot`, a different and equally good
algorithm, disagrees at all.

This also removes a dependency the port should never have had: output no longer
varies with the toolchain.

#### Mutation validation

13 mutants against `flatten` and the owned transcendentals, 9 killed:

| mutation | result |
|---|---|
| `dtAt`: `8 * chordTol` perturbed | killed |
| refine: drop the irreducible-cusp break | killed |
| `tsForCurve`: `dt / 2` -> `dt / 2.0001` | killed |
| `flatten`: `CURVE_BOUNDARY` gate `ci > 0` -> `ci >= 0` | killed |
| `flatten`: `ds` fill off-by-one | killed |
| `flatten`: `tangentDeg` fallback `prevTheta` -> `0` | killed **after** a fixture was added |
| `jsHypot` -> `std::hypot` | killed |
| `jsAtan2` -> `std::atan2` | killed |
| `jsAcos` -> `std::acos` | killed |
| `dtAt`: speed guard `1e-12` -> `1e-11` | survived — **equivalent** |
| `dtAt`: kappa guard `1e-9` -> `1e-8` | survived — **equivalent** |
| refine: `chord <= dsMax` -> `<` | survived — measure-zero boundary |
| refine: `prevTurn * 0.99` -> `0.999` | survived — **genuine gap** |

Three fixtures were added to close gaps the first round exposed: `tiny_speed`,
`tiny_kappa`, and `degenerate_after_curve`. Only the last of those killed its
mutant, and the two that did not are the interesting result.

**`dtAt`'s two epsilon guards are unobservable, provably.** Not for want of a
fixture — the caps they gate cannot bind anywhere in the guards' dead bands:

- The spacing cap `dsMax / speed` binds only when `speed > dsMax / dtMax` = 5
  with the shipped quality config, five orders of magnitude above the `1e-12`
  guard.
- The tangent cap inside the same branch needs `kappa > 0`, and `curvature()`
  has its OWN `speed < 1e-10` guard that returns 0 first. So in the whole band
  below `1e-10`, kappa is zero by construction and the tangent cap is skipped
  regardless.
- The kappa guard is the same story from the other side: the tangent cap binds
  only when `kappa * speed > (dthetaMax·pi/180) / dtMax` ~ 0.35, which at
  `kappa ~ 1e-9` needs `speed > 3.5e8`.

This is worth knowing because audit F1 treated those guards as load-bearing —
"gated on is-this-measurable, when at a cusp the opposite is correct". At the
magnitudes they actually test, they gate nothing. The cusp behaviour F1 cared
about comes entirely from the enforcement loop, which is what batch D
concluded by a different route.

The one real gap is the `0.99` tolerance in the irreducible-cusp detector.
Removing that clause is caught; changing its constant to `0.999` is not, so its
exact value rests on judgement rather than on a fixture. Killing it needs a
curve whose successive turn measurements shrink by between 0.1% and 1%, which
has not been constructed.

---

### Stage 5 ported — and `cos` joined the owned set

`lib/motion/constrain.cpp` is bit-identical to the TypeScript: **75 cases,
21,339 ceilings, 6,096 direct function cases.** Suite total 146,289 assertions.

Constrain is a smaller surface than flatten — one loop, no adaptive stepping —
and it ported cleanly. Two things came out of it that were not in the stage
itself.

#### `Math.cos` is the worst libm offender so far

Measured over 200,000 inputs, half in the shape `junctionCap` actually calls it
with (`cos((|turnDeg|·pi/180)/2)`, so `[0, pi/2]`) and half a wider sweep:

| | mingw libm vs V8 | owned implementation vs V8 |
|---|---|---|
| `cos` | 5,596 / 200,000 (**2.8%**) | **0** — fdlibm |

The number that matters is not 2.8% but the magnitude: **up to 26 ULP**, where
`atan2`, `acos` and `hypot` were all capped at 1. That is not a last-place
rounding difference, it is a worse answer — x87's `fcos` reduces its argument
against a 66-bit approximation of pi, so accuracy decays with magnitude, while
fdlibm reduces against a multi-word pi. Which is "right" remains irrelevant
(V8 is the reference because the TypeScript is the reference), but this one was
never merely cosmetic.

`jsCos` is fdlibm's `__kernel_cos` / `__kernel_sin` plus argument reduction.
The reduction is ported for **medium range only** (`|x| < 2^20·pi/2`); fdlibm's
150-line multi-precision path beyond that is not, because the only caller never
leaves `[0, pi/2]`. The unported branch returns NaN rather than a plausible
wrong number, so a future caller that reaches it fails the differential test
loudly instead of shifting a toolpath quietly.

#### The mutation harness reported a false green, again

The first mutation run came back **25 survivors out of 25** — and that was the
script, not the port. Its kill detector matched a regex against PlatformIO's
output that never matched, so every run read as a pass. It was caught only
because 25/25 is not a believable result and a hand-run mutant died.

Two fixes, both worth keeping in any future mutation script:
- decide kill/survive from the **`[FAILED]` suite names and the exit code**,
  with an explicit `HARNESS-BROKE` branch when neither a PASSED nor a FAILED
  line appears — silence must never read as "survived";
- **verify the mutation applied** (checksum before/after) before believing its
  result. One mutant in this batch was a no-op that would otherwise have been
  filed as equivalent.

This is the third time in this port that a verification step passed for a
reason unrelated to what it claimed to verify (the 15-value libm probe, and
before it the geometry `const char*` print). The pattern is consistent enough
to name: **a check that has never been seen to fail is not evidence.**

#### Mutation validation

25 mutants, 14 killed, 1 no-op (excluded), 10 survived:

| mutation | result |
|---|---|
| `junctionCap`: `jsCos` -> `std::cos` | killed |
| `junctionCap`: drop the `feedMax` min | killed |
| `kappaPrime`: drop the `i` flag test | killed |
| `kappaPrime`: drop the `i+1` flag test | killed |
| `kappaPrime`: span guard `1e-6` -> `1e-7` | killed |
| `kappaPrime`: span `i-1,i` -> `i,i+1` | killed |
| `constrain`: `aRate` gate `> 0` -> `>= 0` | killed |
| `constrain`: `aAcc` gate `> 0` -> `>= 0` | killed |
| `constrain`: corner stop `>=` -> `>` | killed |
| `constrain`: ignore `hasCornerStopAngle` | killed |
| `constrain`: drop the `CURVE_BOUNDARY` gate | killed |
| `constrain`: A-slew cap `aRateRad / kappa` -> `* kappa` | killed |
| `jsCos`: `n & 3` case 1 sign | killed |
| `jsCos`: `kernelSin` `iy` branch ignored | killed |
| `jsCos`: kernel split `0x3FD33333` -> `0x3FD00000` | killed |
| `constrain`: kappa gate `1e-9` -> `1e-8` | survived — **equivalent** |
| `constrain`: kappa-prime gate `1e-9` -> `1e-8` | survived — **equivalent** |
| `constrain`: `angleDelta` args swapped | survived — **equivalent** |
| `junctionCap`: straight gate `>=` -> `>` | survived — measure-zero boundary |
| `junctionCap`: reversal gate `<=` -> `<` | survived — measure-zero boundary |
| `constrain`: junction gate `> 1e-6` -> `>= 1e-6` | survived — measure-zero boundary |
| `constrain`: `vMin` `<` -> `<=` | survived — measure-zero boundary |
| `jsCos`: `qx` `0.28125` -> `0.28` | survived — algebraically equal, low bits |
| `jsCos`: drop the 3rd reduction iteration | survived — **genuine gap** |

**The two epsilon guards are unobservable, and this is the same finding as
flatten's.** `kappa > 1e-9` gates two caps that cannot bind anywhere near it:
the centripetal cap binds only above `kappa > aMax/feedMax²` = 800/3600 ≈ 0.22,
and the A-slew cap above `kappa > aRateRad/feedMax` ≈ 0.21 — both about eight
orders of magnitude above the guard. The `kp > 1e-9` guard is the same shape:
its cap binds above `kp > aAccRad/feedMax²` ≈ 0.017. In the entire dead band
the ceiling is `feedMax` with or without the guard.

That is now **four** measurability guards across two stages that gate nothing
(F1's two in `dtAt`, and these two). They are not harmful, but the codebase
reads as though small-magnitude inputs are a handled hazard, and they are not
handled — they are simply never dangerous, because every cap here is a
*ceiling* and a ceiling computed from a tiny denominator is enormous. Worth
stating once rather than rediscovering per stage.

**`angleDelta`'s sign is unused.** Swapping its arguments survives because
`angleDelta` is antisymmetric and both call sites immediately take `fabs` —
including at exactly ±180°, where both orderings return +180. So `constrain`
sees turn *magnitude* only; it cannot distinguish a left corner from a right
one. That is correct for both caps as specified, and is noted only because the
code reads as if direction were available.

The real gap is `jsCos`'s third reduction iteration, reached only for arguments
very near a large multiple of pi/2. The sweep tops out at 1e3 and the only
caller works in `[0, pi/2]`, so it is unreachable today and untested; if a later
stage calls `cos` on a raw accumulated angle, it needs its own cases.

---

### Stage 6 ported — and the workflow was reordered first

`lib/motion/plan.cpp` is bit-identical to the TypeScript: **77 cases, 23,710
speeds, 1,554 direct `segAccel` cases.** Suite total 206,741 assertions.

More importantly, this is the first stage the C++ has **contract tests** for:
13 test cases ported from `web/test/toolpath/plan.test.ts`, covering purity,
boundedness, endpoints-at-rest, the feasibility contract of both sweeps,
profile shape, monotonicity, `segAccel`, `subpathRanges`, P1 and P2.

#### The order changed, and the change paid immediately

Stages 4 and 5 were ported as *stage, then bit-parity vectors, then commit* —
no contract tests at all. That was a hole, not a deferral: **bit-parity proves
agreement, never correctness**, and it cannot survive an optimisation, because
an optimisation is precisely a change that is allowed to move the bits. A
verification strategy that has to be deleted the moment optimisation starts is
not a verification strategy for a port whose stated purpose is to be optimised.

New order, per stage:

1. port the stage,
2. port its contract tests, and get them passing,
3. *then* generate the bit-parity vectors.

Evidence from this stage: the contract tests found one real error (a wrong
closed form in a ported test), and once they passed, **the differential passed
on its first run.** Compare stage 4, where the first differential run failed on
a 1-ULP `theta` divergence that took nine steps to localise, because a numeric
question and a semantic question look identical in a bit comparison.

Contract tests fail with a sentence. Dropping plan's backward sweep produces
`32 violation(s): straight_line: decel jump at 14, ...`. The same mutation
against the differential alone produces two different hex strings.

#### Mutation validation

10 mutants against `plan`, **9 killed by the contract tests alone** — the
differential was not needed to catch any of them:

| mutation | result |
|---|---|
| drop the backward sweep | killed — 32 named feasibility violations |
| drop the P1 headroom block | killed |
| `segAccel`: `aMax` fallback -> 0 | killed |
| `segAccel`: degenerate segment returns 0, not `aMax` | killed |
| `segAccel`: `jsMax(kappa)` -> `jsMin` | killed |
| `segAccel`: drop the `pathAccel` term | killed |
| drop the P2 bracket validation | killed |
| P1: `aMax^2 - ac^2` -> `+` | killed |
| sweeps: `2*a*ds` -> `a*ds` | killed |
| drop the endpoint re-pin after the sweeps | survived — **dead code** |

Two of the mutants in the first run were malformed (a `sed` that edited only a
comment, and one that could not match across lines). Both were re-run properly
rather than filed as survivors — the mutation script's no-op checksum caught
one, and the other was caught by disbelieving the result. See the stage 5
section: a check that has never been seen to fail is not evidence.

#### FINDING P6: plan's final endpoint re-pin is unreachable

`plan.ts` ends each subpath with

```ts
// endpoints stay pinned (forward pass may have lifted hi off 0)
v[lo] = 0;
v[hi] = 0;
```

Removing it changes nothing on any fixture, and not for want of coverage —
**it cannot change anything.** Both sweeps assign only through
`if (reachable < v[i]) v[i] = reachable`, and `reachable` is a `sqrt`, so it is
never negative. A value already pinned to 0 can therefore only stay 0. The
forward pass cannot lift `hi` off 0, and the comment asserting it might is
wrong.

Kept in the port anyway, because the port is a bug-for-bug transcription and
this is not a bug — it is three dead statements per subpath. Worth recording
because the comment reads as documentation of a real hazard, and a future
reader hardening the sweeps would reasonably believe it.

This is the third piece of load-bearing-looking code the port has found to be
inert, after `dtAt`'s two guards in flatten and constrain's two — see the stage
5 section. The pattern is consistent: **defensive code written against a
hazard nobody measured.**

---

### The contract-test debt paid — and a hole in the TypeScript's own tests

Stages 4 and 5 shipped with bit-parity only, because the reorder that put
contract tests first arrived at stage 6. That left 94 tests owed: `geometry` 28,
`flatten` 25, `constrain` 41. All 94 are now ported, and the C++ tree is split
into two suites — `test/test_contract` and `test/test_parity` — so the properties
can be run without the reference vectors present, which is the state the port
will be in once bit-parity retires.

Two things came out of the work that were not visible from the TypeScript.

**The real-artwork fixtures had to be carried across, and they earn their keep.**
Five of the ported tests assert the caps against `test_snake.svg`, and the C++
has neither an SVG parser nor `enforceC1` — both are host stack, outside the
port's scope. `web/test/port/cppRefFixtures.test.ts` therefore exports the
repaired Bezier subpaths and **no expected outputs**; the C++ flattens and
constrains them itself. This is not a convenience: every cap failure in this
document (F1, F7) was found on real geometry, which accelerates across a step
far harder than any synthetic fixture. When `flatten`'s refinement was mutated
away, the SVG tests failed alongside the synthetic ones — the evidence that the
fixture is load-bearing rather than decorative.

**FINDING C3 — monotonicity is one-sided, and the curvature-gradient cap was
unprotected because of it.**

`constrain.test.ts` declines to check the A angular-accel cap directly, on the
stated grounds that `kappaPrime` is internal, and asserts monotonicity plus a
scaling law instead. Mutation testing against the ported suite showed that
leaves a hole big enough to drive two defects through. Both of these survived all
94 tests:

| mutant | effect on a machine |
|---|---|
| `span = ds[i]` instead of `ds[i-1] + ds[i]` | `\|k'\|` doubles, so every curvature-varying move is slowed by √2 for nothing |
| drop the `KAPPA_BREAK` guard | a finite difference straddles a curve join, where `kappa` is genuinely discontinuous, and divides a large step by a near-zero span — a near-zero ceiling at **every curve join in every job** |

Neither is subtle, and neither is detectable by what was there:

- **Monotonicity cannot see them by construction.** Both mutants only ever
  LOWER a ceiling, and "tightening a limit never raises a ceiling" is satisfied
  by a cap that is far too tight. A one-sided property cannot detect a
  one-sided error in the same direction.
- **The scaling law cancels them exactly.** It is a ratio of two ceilings, so
  any constant factor on `|k'|` divides out.

Closed with two tests that are two-sided and still need none of constrain's
internals: one asserts the cap binds at exactly `sqrt(alpha/|k'|)` where it is
the *active* constraint (an equality, so a ceiling that is too low fails as
readily as one that is too high, with a guard that it binds at >10 samples so it
cannot go vacuous); the other asserts that enabling the cap changes **nothing**
at a sample whose difference span straddles a `kappa` break, which states the
guard as physics — a representation artefact where two curves meet is not a
rotation the A axis performs.

This is the first defect the port has found in the *tests* rather than in the
code, and the TypeScript suite has it too. Worth carrying into `discretize` and
`choreograph`: wherever a cap is verified only by monotonicity, it is verified
only against being too loose.

### Stage 7 (`discretize`) ported — and C3's shape recurred immediately

`discretize` is the first stage that is not pure geometry: it turns mm into
steps, so it is the first that has to know a machine exists. Three things came
out of porting it.

**The config layer did not cross, and did not need to.** The TypeScript's
`discretize(samples, machine, profile, quality, overrides)` resolves inside
itself — `resolvedAxes`, `resolveTargets`, and the `overrides ?? profile ??
machine` chain. The port takes a flat `DiscretizeOptions` holding what that
chain produces, so schema, loader, validator and tool catalogue all stay in
TypeScript. The honest cost: the port cannot reproduce a defect that lives IN
the chain, only one that lives in what the chain produces. The chain is `??`
operators over config and has its own tests.

**`choreograph` came along as a dependency, not as stage 8.** `discretize`
calls `travelJog`, `preOrient`, `pivot`, `zMove` and `zStepCount` at every
transition, so stage 7 could not be verified without them. `rampChunks` — the
trapezoidal generator every one of those wraps — is now ported and pinned
directly, both as a contract and by 409 differential cases. Stage 8's own 61
contract tests remain owed.

**No new transcendental, the first time that has happened.** The stage uses
`sqrt`, `hypot`, `round`, `ceil`, `trunc`, `min`, `max`, `abs`. `sqrt` is
correctly rounded by IEEE-754 and `hypot` is already owned as `jsHypot`
(V8's algorithm, not the platform's). Measured before writing code, per the
workflow, and it cost three tool calls to establish rather than nine to
diagnose.

Mutation, 30 mutants across `discretize.cpp`, `choreograph.cpp` and
`microsegment.cpp`, run against the CONTRACT suite alone:

| outcome | n | notes |
|---|---|---|
| killed by contract tests | 27 | including every `interval` mutant, the H1 chunk-duration rule, the unwind branch, and the D1 skip guard |
| measure-zero | 1 | `>= cornerAngle` → `>`; survived parity too |
| **genuine gap, now closed** | 1 | `ceil` → `floor` on the subdivision count (D6) |
| argued not closable by a property | 1 | the 256 subdivision clamp (D7) |

The `ceil` → `floor` survivor is **C3's shape exactly**, in a different stage.
The test asserting "no cutting segment spans a speed change greater than dvMax"
re-derived `k` with its own `ceil` and compared the result against `dvMax` — so
it audited the arithmetic of the rule while being blind to whether the stage
used that rule at all. Swapping the stage's `ceil` for `floor` left it green.
Closed by adding a second test that derives each emitted segment's speed the way
the FIRMWARE will (XY distance over `interval × major / fCpu`) and bounds the
step-to-step change, with no reference to `k`, to `dvMax`, or to anything
`discretize` computed. That kills the mutant.

Two lessons, and the second is the general one:

- The quantised reading needs a floor. Derived speed is quantised to ~1/major,
  so segments under ~10 steps read as 6 mm/s "jumps" that are an artefact of the
  integer interval — the ends of every path, where the tool leaves and returns
  to rest one step at a time. Filtered on step count, with an explicit
  "did anything survive the filter?" assertion, because a filter that quietly
  excludes everything is the failure mode a filter invites.
- **A test that re-derives the quantity it is auditing agrees with the
  implementation by construction.** C3 was this, in a form that looked like
  monotonicity. This was this, in a form that looked like a direct check. Both
  survived a mutation that changed the machine's behaviour. The tell is the same
  in both: the test recomputes something the stage also computes, instead of
  measuring what the stage emitted.

The 256 clamp is filed rather than closed, with the numeric argument. Any `k` at
or above `ceil(dv/dvMax)` produces a stream that honours the speed budget, so no
contract-level property distinguishes 256 from 1e9 — the clamp bounds *work*,
not output validity. Its observable effect is real but semantic-free: a different
`k` moves the sub-step positions `j/k`, so the same net motion is distributed
across segments differently, which is why the differential kills it and no
property test can. What IS assertable, and now is, is that the emitted count
saturates as `dvMax` falls (609 segments at the shipped 3.0, 22,426 at 1e-3,
23,099 at 1e-4 — a 10x tightening buying 3%), because a pair spans at most
`dsMax` of arc and `dthetaMax` of turn and so has at most ~80 XY steps and ~104
A steps to give, whatever `k` is.

**D3's stated cause no longer holds.** The finding was filed as a chain ending
"plan asks A for up to 16.8x its rate ceiling". Measured now, in both languages,
the plan is within the A ceiling on every fixture (worst 1.01x on `cusp`), and
that half of the TypeScript's D3 test passes. The stretch is still real and still
confined to `cusp` (1.101x) and `near_cusp` (1.867x) — the port reproduces both
numbers to within 0.01 — but its cause needs re-deriving rather than re-quoting.
Recorded in the table below.

---

### Stage 8 (`choreograph`) and `microsegment` tests ported — the port is complete

The last debt. Both modules were already ported and bit-parity verified as stage
7's dependency; what was owed was the contract suite, the layer that outlives
bit-parity. 61 tests from `choreograph.test.ts` and 12 of the 16 from
`microsegment.test.ts` (the other four test the 26-byte wire serialiser, which
is firmware-side and not in the port), plus 8 added here, in 14 new cases.

**Two functions had no caller at all.** `aMoveTo` and `headOffsetJog` are
reached from `orchestrate` in the TypeScript, which is host stack — so on the
C++ side nothing calls them, the reference vectors never exercise them, and
these contract tests are the only thing holding them. That is worth stating
plainly because it inverts the usual reassurance: for the rest of the port a
surviving mutant might still be caught by the differential, and for these two it
cannot be.

Mutation, 43 mutants across `choreograph.cpp` and `microsegment.cpp`, run
against the CONTRACT suite alone:

| outcome | n | notes |
|---|---|---|
| killed by contract tests | 34 | including every `rampChunks` boundary rule, both invert branches, the H4 refusal, and 8 of 11 `interval` mutants |
| algebraically equivalent | 4 | `N<=0`→`N<0` (at N=0 the marks set collapses to `{0}` and the chunk loop never runs); `n<=dAcc`→`n<dAcc` (both branches return `peak` at the boundary); `liftHeight<=0`→`<0`; the per-axis rate floor skipping idle axes (an idle axis contributes `0` to a `max`) |
| provably unreachable guard | 3 | the decel-branch `jsMax(v0², …)`, which can only bind for `n > N` and `vAt` is never called there; and the two `interval ≥ 1` clamps — the fastest rate any axis is commanded is 12800 steps/s against a 150 MHz clock, so the smallest interval reachable is 11718 |
| below the contract's resolution | 1 | `jsRound`→`std::trunc` on a chunk interval: a sub-cycle bias in ~10⁴, against timing tolerances of 0.1–5%. The differential kills it instantly; no property stated in seconds can |
| **genuine gap, now closed** | 4 | below |
| fidelity gap, now closed | 1 | `RAMP_CHUNKS` 16→8 |

**The C3/D6 shape recurred a third time, and this one would have cut wrong.**
`preOrient`'s non-unwind branch computes `angleDelta(currentTheta, entryTheta)`.
Swapping those two arguments negates the result — `angleDelta(a,b)` is the
rotation *from a to b* — so a crease tool would pre-orient the wrong way on
every path. It survived all 61 ported tests. The cause is that every non-unwind
assertion in the TypeScript reads `Math.abs(r.newAPhys)`: the shortest-delta
test, the never-more-than-180 test, and the ignores-accumulated-aPhys test all
measure magnitude, and the quantity the mutation changes is sign. Closed with a
test that asserts the signed value on both the wrapping case (170 → −170 is +20,
not −20) and the ordinary one.

This is the same failure as C3 and D6 wearing a third disguise. C3 destroyed the
information by taking a ratio, D6 by re-deriving the rule, this one by taking an
absolute value. The general statement holds for all three and is the one to
carry forward: **the audited quantity must survive the measurement.** Ask what
operation stands between the emitted value and the assertion, and whether the
defect could pass through it unchanged.

The other three genuine gaps were all fixtures that never varied one thing:

- **`headOffsetJog` was never tested on a single-axis offset change.** Both
  TypeScript fixtures move X *and* Y, so narrowing the "nothing to do" guard
  from `&&` to `||` — discarding every pure-X and pure-Y offset — passed. A
  revolver whose heads differ only in X would silently not compensate.
- **`zMove` was never tested at a non-positive feed.** The `1e-9` floor on the Z
  step rate is invisible at `zFeed == 0` (the divide gives `+inf`, which clamps
  to `fCpu` either way) and decisive at a negative one, where the unfloored form
  yields a negative cycle count that clamps to `interval == 1` — the *fastest*
  move the wire can express, from a feed that asked for the opposite.
- **`interval`'s three-argument overload was never called below `vMin`.** The
  one test of the legacy path uses full feed, so dropping its `vMin` floor
  changed nothing observable — though at `v = 0` it is the difference between a
  2-second segment and a one-second stall that has nothing to do with the
  requested motion.

**Ramp fidelity was pinned by nothing.** `RAMP_CHUNKS` 16 → 8 survived
everything. The accel property cannot see it — the midpoint convention is
scale-invariant, since a chunk twice as long carries twice the speed change over
twice the time — and the segment-count bound is one-sided (`≤ 33`), so a coarser
ramp passes it comfortably. That is C3's shape once more, in the mildest form:
a one-sided bound on a quantity whose defect is on the other side. Closed with a
lower bound on a full-trapezoid move. The knob is a bandwidth-versus-smoothness
choice rather than a correctness one; the point of pinning it is that changing
it has to be deliberate.

**`JUNCTION_V` 50 → 0 survives and is not a defect.** Starting and ending a
standalone move from true standstill is strictly more conservative than starting
at 50 steps/s, every emitted profile stays legal, and nothing in the contract
promises a non-zero junction speed. Recorded as a knob whose value is a choice.

---

## Findings

| # | Stage | Severity | Summary | Status |
|---|---|---|---|---|
| F1 | flatten | **defect** | Tangent cap SKIPPED at `\|B'\|→0` — 178° reversal in one step (89×) | **resolved** — enforcement, and the premise corrected (a cusp is a corner, not a sampling problem) |
| F2 | flatten / constrain | **inconsistency** | Intra-curve cusp is a corner to `discretize`, invisible to `constrain` | **resolved** (constrain's stop ungated) |
| F3 | flatten | minor | Truncated final step manufactures degenerate near-zero-`ds` samples | open, test green |
| F4 | flatten | gap | `chordTol` and `dthetaMax` had no tests — two of three caps unverified | **resolved** |
| F5 | flatten | tuning | `chordTol` is near-vestigial: binds 0.8% of steps | note only |
| F6 | geometry | cleanup | `arcLength` (5-point Gauss-Legendre) had no production caller | **resolved** (deleted) |
| F7 | flatten | **contract** | All three caps are PREDICTORS, not bounds — `dsMax` soft by up to 8% | **resolved** (measure-and-halve; +50% samples) |
| C1 | constrain | **defect** | No lower bound on `vCeiling` — a cusp yields 3.2e-3 mm/s, 166× under `vMin` | **resolved** (sub-`vMin` ceiling → stop) |
| C2 | constrain | ok | All four caps hold as per-sample properties on every fixture | verified |
| C3 | constrain **tests** | gap | A-accel cap verified only by monotonicity + a ratio — both blind to a cap that is too TIGHT; two `kappaPrime` mutants survived all 94 ported tests | **resolved** (two two-sided tests); the TypeScript suite still has it |
| P1 | constrain + plan | **defect** | Axis accel budget spent twice: centripetal and tangential each capped at `aMax`, nothing owns the sum (→ √2·aMax) | **resolved** (shared budget in `plan`) |
| P2 | plan | **contract** | A stream without `PATH_START`/`PATH_END` was silently unplanned — `v = vCeiling`, no error | **resolved** (throws) |
| P3 | plan | consequence of C1 | Carries unexecutable ceilings through; ~⅕ of the below-`vMin` span is self-inflicted by the sweeps | **resolved with C1** — residual is a ramp out of a stop, i.e. arithmetic |
| P4 | compileBlock | tuning | A non-tangential tool still pays the A-axis curvature cap — ~8× accel loss on a 5 mm arc | **resolved** (A term gated on `tangential`) |
| P5 | plan | ok | Two O(n) sweeps, no convergence loop; feasibility, monotonicity and endpoint pinning all hold | verified |
| D1 | discretize | **defect** | Empty segment (all deltas 0) emitted with `interval = fCpu` — a full second. Reachable at a corner AND at every `PATH_END` | **resolved** (skipped; `PATH_END` re-homed) |
| D2 | discretize | **defect** | Sub-segment speed interpolated linearly in *distance*, not `sqrt(v0²+2as)` — timing error up to 1.51×, worse the finer it subdivides | **resolved** (`sqrt` interpolation) |
| D3 | discretize | **contract** | `interval`'s per-axis rate floor is a second, unmodelled speed governor; executed ≠ planned timeline (`cusp` 1.101×, `near_cusp` 1.867×) | open, **test red**. **Cause re-derived:** the plan is legal (peak A 0.91× the ceiling on `near_cusp`); `discretize` picks `k` from the XY speed budget alone and then spreads A evenly by sub-step INDEX while sub-step DURATIONS are unequal, spiking the A rate. 100% attributable to the A axis. Fix belongs in `discretize`, not upstream |
| D4 | discretize | **inconsistency** | Corner rule ungated on `CURVE_BOUNDARY` unlike constrain's — this is F2, now measured | **resolved with F2** |
| D5 | discretize | gap | `DEFAULTS.tool.liftHeight = 0`, so the Z lift/lower path was dead *under test*. The deployed config sets `knife.liftHeight = 2.0`, so production did lift | **resolved** (tests) |
| D6 | discretize **tests** | gap | "no segment spans more than `dvMax`" re-derived `k` with its own `ceil`, so it audited the rule's arithmetic while blind to whether the stage used it; a `ceil`→`floor` mutant survived | **resolved** (second test measures emitted segment speeds firmware-style); same shape as C3 |
| D7 | discretize | note | The 256 subdivision clamp bounds work, not output validity — no contract-level property distinguishes it from uncapped | filed, not closable by a property test; saturation of the emitted count is asserted instead |
| H1 | choreograph | **defect** | `aMove`'s decel ramp exceeds the A accel limit by 1.26–1.65× and never reaches rest — stops dead from up to 39 deg/s. Chunk-start rate sampling is conservative going up, anti-conservative coming down | **resolved** (`rampChunks`) |
| H2 | choreograph | **defect** | `travelJog` / `headOffsetJog` emit one segment at full feed — 0→80 mm/s in zero distance, ignoring `x.maxAccel` entirely | **resolved** (same generator) |
| H3 | choreograph | **defect** | `zMove` was likewise unramped (0→24000 steps/s), and the 20 mm/s engage feed was itself 2× the axis's declared 10 mm/s ceiling — `zMove` never consulted it, and not going through `interval()` meant the per-axis rate floor never saw it either | **resolved** — ramped via `rampChunks`; feed and accel CLAMPED to the axis. `z.maxAccel` is now a declared 300 mm/s², **provisional and unmeasured** |
| H4 | choreograph | **contract** | `aMove` silently invented 180 deg/s + 2000 deg/s² when the A ceilings are 0 — `load.ts` refuses to invent calibration, this invented limits | **resolved** (throws) |
| H5 | choreograph | cleanup | Three redundant guards all defend `v ≥ v0`; each is an equivalent mutant | **resolved** — two vanished with the batch C rewrite; the `[1, fCpu]` interval clamp survives in `rampChunks` |
| H6 | choreograph **tests** | gap | `preOrient`'s non-unwind rotation DIRECTION verified by nothing — every assertion takes `Math.abs`, so swapping `angleDelta`'s arguments (which negates it) survived all 61 tests. A crease tool would pre-orient the wrong way on every path | **resolved** in C++ (signed assertions); the TypeScript suite still has it |
| H7 | choreograph **tests** | gap | Two fixtures that never vary one thing: `headOffsetJog` is only ever given a two-axis offset change (so a `&&`→`\|\|` guard discarding single-axis changes passes), and `zMove` is never given a non-positive feed (where the unfloored Z rate yields `interval == 1`, the fastest move the wire can express) | **resolved** in C++; the TypeScript suite still has both |
| H8 | choreograph **tests** | gap | Ramp fidelity pinned by nothing — `RAMP_CHUNKS` 16→8 survives, because the accel property is scale-invariant and the segment-count bound is one-sided | **resolved** in C++ (lower bound added); a knob, not a defect |
| H9 | microsegment **tests** | gap | `interval`'s three-argument overload only ever called at full feed, so its `vMin` floor is untested — at `v = 0` the difference is a 2-second segment versus a one-second stall | **resolved** in C++; the TypeScript suite still has it |

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

**The cause has been re-derived. The chain below is the ORIGINAL one and it is
false now — kept only so the correction is legible.**

```
[SUPERSEDED]
flatten's tangent cap overshoots (F7)
    → actual sample-to-sample turn exceeds kappa*ds   (1.64x on near_cusp, 8.17x on cusp)
constrain's A-slew cap is computed from kappa
    → it under-caps v
plan's timeline asks A for up to 16.8x its rate ceiling
interval silently rescues it by stretching the segment
```

The last two links no longer hold. The upstream fixes (F7's measure-and-halve,
C1, P1) took the plan back inside the A ceiling: measured per sample pair, the
peak A rate the plan asks for is **90.6 deg/s on `near_cusp` (0.91x the 100
ceiling)** and 100.8 deg/s on `cusp` (1.01x). The plan is legal. The symptom
survived anyway.

Attribution probe, per emitted cutting segment, asking which of `interval`'s
terms the `jsMax` picked and how much time the floor added:

| fixture | segments stretched | axis | share of the overrun |
|---|---|---|---|
| `cusp` | 60 / 168 | **A**, all of them | — (baseline optimistic; see below) |
| `near_cusp` | 100 / 384 | **A**, all of them | 98% |

X appears in a naive version of this probe and is an artefact: X's ceiling is
80 mm/s and the cut feed is also 80, so an axis-aligned segment at feed *ties*
with the floor without being stretched by it. Distinguishing a tie from a
stretch removes X entirely. (The `cusp` share exceeds 100% because the
counterfactual baseline — XY distance at full feed — is faster than what the
plan actually asked for there; the attribution is exact, the magnitude is an
upper bound. `near_cusp` is the clean case.)

**The A demand is redistributed, not inflated.** On `near_cusp` the plan turns
A by 180.00 deg and the emitted stream turns it by 180.00 deg — 1.000x, no
rounding drift in the tangent tracker. So nothing creates extra rotation; the
same rotation is packed into sub-segments that individually demand more than
100 deg/s.

The corrected chain, which lives entirely inside `discretize`:

```
k is chosen from the XY speed budget ALONE
    → k = ceil(|b.v - a.v| / dvMax)                      (discretize.ts:167)
    → the A axis has no vote in how a pair is subdivided
A is then distributed LINEARLY IN THE PARAMETER
    → thF = theta + dtheta * (j/k)                       (discretize.ts:181)
    → equal turn per sub-step
but sub-step DURATIONS are not equal
    → speed ramps across the pair, so the fast sub-steps are short
    → equal turn / unequal time = an A rate spike in the shortest sub-step
interval's floor stretches those sub-steps back to 100 deg/s
```

That is why it is confined to `cusp` and `near_cusp`: they are the fixtures
where `v` swings hardest across a pair, so the sub-step durations are most
unequal. A cruise pair has `k = 1` and cannot exhibit it at all.

**This relocates the fix.** Feeding the per-axis ceilings upstream into
`constrain`/`plan` does *not* address it — the plan already respects them. The
defect is created between the plan and the wire. Two candidate fixes, both local
to `discretize`:

- distribute `theta` in proportion to each sub-step's TIME rather than its
  index, so equal-time sub-steps get equal turn; or
- give A a vote in `k`, i.e. `k = max(ceil(dv/dvMax), ceil(turnRate/aRate))`,
  so a pair that would overdrive A is cut finely enough that no sub-step does.

The first is more precise and does not increase segment count; the second is a
smaller change and composes with the existing budget. Neither has been
implemented, and the D3 test stays red until one is.

This is still the second finding in this audit (with P1) whose cause and symptom
live in different places — but the distance is shorter than filed. The lesson
that survives is the one about re-measuring: a root-cause chain is only true as
of the code that was measured, and three of D3's four links were repaired by
fixes aimed at other findings without anyone noticing D3 had moved.

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

### H3 — `zMove` was unramped too — RESOLVED

Two defects in one line, and the second was not in the original filing.

**Unramped.** `zMove` emitted a single constant-velocity segment: 0 → 24000
steps/s in zero distance, the same thing H2 fixed for travel jogs and H1 for A.
It was last because the module refused to invent an accel (H4) and `z.maxAccel`
was a 0 placeholder. `rampChunks` — extracted for A, and exactly the "generic
`trapezoidalMove()` helper" the module TODO asked for — is now used for Z too.

**Over its own ceiling.** The shipped config carries two Z numbers that
disagree: `machine.z.feed = 20` (the engage feed `zMove` uses) against
`z.maxFeed = 10` (the axis ceiling). `zMove` computed `zFeed * stepsPerUnit`
directly and never consulted the axis, and because it does not go through
`interval()`, the per-axis rate floor that governs X/Y/A never saw it either.
`validate.ts` did warn — `exceeds axis ceiling 10 (clamped)` — and nothing
clamped. The one guard that fired reported a mitigation that did not exist.

Both targets are now clamped at the point of use. The policy split is
deliberate and worth stating, because it is not H4's:

- an **absent** limit is REFUSED (H4) — a trapezoid cannot be built from
  "uncapped", and guessing calibration is how you crash a machine;
- a limit that is **present and exceeded** is CLAMPED — the machine's own number
  is the answer, and using it is strictly safer than honouring the request.

`z.maxAccel` is now a declared 300 mm/s². **It is provisional and has not been
measured.** It was chosen as the smallest round value that lets a 2 mm lift at
10 mm/s reach cruise (ramp distance v²/2a = 0.17 mm per side, so 0.33 mm of a
2 mm move) and it is ~3% of g in torque terms — for a vertical leadscrew, a
rounding error on top of the static hold the motor already carries at rest. The
binding constraint on such an axis is stiction at breakaway, not inertia, which
is why no accel in this range is obviously wrong and why none of them is
obviously right either. Measure it: drive N up/down cycles at a candidate value
and read `CMD_GET_POS` for drift, bisecting on accel, with the real tool weight
and at both ends of travel. Test the DOWN move — down-decel and up-accel are the
tied-worst cases, both `m·(g+a)`.

One measurement fell out of the fix. `rampChunks` documents itself as "exact at
every chunk BOUNDARY regardless of RAMP_CHUNKS"; that is approximate, because
chunk boundaries are rounded to integer steps. Measured worst accel demand is
**1.0025× for Z and 1.0001× for A** — Z's ramp is only ~200 steps long, so
integer marks are coarser relative to it. Immaterial (300 vs 300.75 mm/s²) but
the contract tests state a measured tolerance rather than pretending to 1.000.

### H3 — the original filing (superseded)

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

| Suite | Passing | Red | Findings the red tests pin |
|---|---|---|---|
| `test/toolpath/flatten` | 25 | 0 | — |
| `test/toolpath/constrain` | 41 | 0 | — |
| `test/toolpath/plan` | 52 | 0 | — |
| `test/toolpath/discretize` | 38 | 1 | D3 |
| `test/toolpath/geometry` | 28 | 0 | — |
| `test/choreograph` | 62 | 1 | H3 |

Full suite: **709 passing, 2 red**, 4 skipped; `tsc --noEmit` clean. Both red are
intentional and each names its finding:

- **H3** — `zMove` is unramped, and `z.maxAccel` is absent from the fixture AND
  from the deployed `web/demo/config.json`. Blocked on characterizing the Z axis
  on hardware; there is no ceiling to ramp against until then.
- **D3** — `interval`'s floor still stretches `near_cusp` by 1.87×, but the cause
  has moved from an upstream A overdrive to sub-segment `da` distribution (see
  batch D). Wants its own measurement rather than a fix.

`CUSP` is now imported directly by the `flatten`, `constrain`, `plan` and
`discretize` tests. All four stages that consume it have been audited, so it can
move into the shared `CASES` registry whenever a fifth consumer wants it.

---

## Fix batches

Ordered by cheapness, and grouped so that each batch's golden diff is
attributable to one cause. Regenerating a golden is a script; *justifying* the
diff is the expensive part, so the goal is the fewest **unattributable** golden
diffs, not simply the fewest batches.

| Batch | Contents | Golden footprint | State |
|---|---|---|---|
| **A** | H5, F6, P2, H4 | none | **done** |
| **B** | D2, D1 | cutting segments | **done** |
| **C** | H1, H2 (H3 blocked) | non-cutting segments only | **done** |
| **D** | F1, F7, F2/D4, C1/P3, P1, P4 | everything | **done** |

`D3` and `P3` are deliberately absent as work items: both are downstream of
causes in batch D (the F1/F7 cap chain and C1 respectively), so they get
re-measured after D rather than fixed on their own.

### Batch B — done (one re-golden, fully attributable)

**D2** — `discretize` interpolates sub-segment speed as
`sqrt(v0² + f·(v1² − v0²))` (helper `subV`) instead of linearly in distance.
Each sub-segment's own mean is now exact, so the sub-times sum back to the
undivided pair time and subdivision is timing-neutral.

**D1** — every zero-motion sub-step is skipped, including the two that used to
be exempt (a subpath's final one, a corner's last one). `PATH_END` is not lost
with them: it is re-homed onto the last segment the subpath actually emitted.
That target is the last **cutting** segment, which genuinely differs from "the
last segment emitted" — a subpath ending on a corner emits pivot and Z-raise
segments after the cut, measured at 52 segments past the marker.

Measured, emitted ÷ exact cut time:

| fixture | before | after | note |
|---|---|---|---|
| `short_curve` | 1.215 | **1.000** | |
| `cusp` | 1.132 | 1.088 | residual is D3 |
| `near_cusp` | 1.909 | 1.861 | residual is D3 |
| 10 mm line, `dvMax` 6 / 0.75 | 1.268 / 1.510 | **1.000 / 1.000** | PEN, no A axis |

The two fixtures that did not go exact are precisely the two whose plan
overdrives the A rate ceiling (16.82× and 1.02×) — the precondition for
`interval`'s floor to stretch a segment. That is an attribution, not an
exemption: every fixture where D3 cannot fire is now exact, and D3's block
carries a red test for the two that remain.

Golden diff, characterised before regenerating:

| | packets | empty | cut seconds | Σ\|dx\| | Σ\|dy\| | Σ\|da\| |
|---|---|---|---|---|---|---|
| `test_circle` before | 640 | 0 | 4.6889 | 25600 | 25594 | 23247 |
| `test_circle` after | 640 | 0 | **4.6406** | 25600 | 25594 | 23247 |
| `fish` before | 8436 | 17 | 87.3852 | 157095 | 79237 | 374974 |
| `fish` after | **8419** | **0** | **86.1852** | 157095 | 79237 | 374974 |

Every step count is byte-identical — the geometry did not move, only the
timing. `fish` lost exactly its 17 empty packets (D1) and 1.20 s of cut time
(D2, −1.4%); `test_circle` had no empties, so its 1.03% is pure D2. Note the
seconds column *understates* D1: an empty packet clocks no steps, so it
contributes nothing to this metric while costing the firmware up to a full
second each.

Mutation-validated: 8 of 9 mutants killed by name. The survivor is
`flags | MICRO_PATH_END` → `flags = MICRO_PATH_END`, equivalent under any
reachable input (the relocation target is a cutting segment, which carries no
other flag). The `|` is kept as defence for the fallback path, where the target
could be a `zMove` carrying `MICRO_LIFT`.

### Batch D — done (the five decisions)

All five were settled deliberately; each is recorded with what it cost.

**3 — a ceiling below `vMin` is a stop (C1), and a cusp is a corner (F2/D4).**
`constrain` takes `vMin` as an explicit option (absent = disabled, like its other
switches — the stage still imports no config) and forces a sub-`vMin` ceiling to
0. Its corner STOP is ungated; the junction-deviation cap stays gated on
`CURVE_BOUNDARY`, because that models a vertex across a near-zero-length span and
applying it in-curve would double-count the centripetal cap.

**1 + 2 — the caps are enforced (F7), and F1's premise was wrong.** Each step is
now measured after being proposed: realised chord and realised turn, halve on
overshoot, up to `quality.maxRefine` times. Enforcement closes F1 without a
second epsilon rule.

But F1 asked for something unachievable. "A cusp must force fine sampling" cannot
work: a true cusp reverses the tangent at a single parameter value, so the
realised turn tends to 180° however small the step becomes. Refinement there buys
samples and changes nothing, so the loop detects the irreducible case and stops.
The cusp is then read as the CORNER it is — which is exactly what decision 3 made
`constrain` do. **F1 and F2 turned out to be one finding.**

| fixture | over `dsMax` before → after | over `dthetaMax` before → after | samples |
|---|---|---|---|
| `straight_line` | 99 → **0** | 0 → 0 | 201 → 302 |
| `s_curve` | 176 → **0** | 0 → 0 | 358 → 534 |
| `long_gentle_arc` | 506 → **0** | 0 → 0 | 1016 → 1522 |
| `quarter_circle_r5` | 0 → 0 | 22 → **0** | 46 → 68 |
| `near_cusp` | 54 → **0** | 40 → **0** | 205 → 302 |
| `snake.svg` | 142 → **0** | 31 → **0** | 356 → 531 |
| `cusp` | 11 → **0** | 29 → **1** (irreducible) | 78 → 118 |

Cost is ~50% more samples. `maxRefine` is the knob the firmware pins and the host
may raise; depth **4** already achieves every number above, so the shipped default
of 8 is margin rather than need.

**4 — one acceleration budget (P1).** `plan` reduces each segment's tangential
allowance by the centripetal load committed there, `a_t ≤ sqrt(aMax² − a_c²)`.
The cusp's 1412 mm/s² against a 1000 limit is now ≤ 1000. Cost in path time:
**+0.9% on `cusp`, under +0.3% everywhere else.**

The load is computed from the CEILING, not from a first pass's `v`. Both were
implemented and measured, and the choice is a real trade:

| headroom from | bounds the sum | monotone in accel | monotone in vCeiling | sweeps |
|---|---|---|---|---|
| planned `v` | yes | **no** (−0.12% reversal) | yes | 4 |
| `vCeiling` | yes | yes | **no** | 2 |

"More budget never plans slower" is about the machine's capability and is worth
more than the last fraction of a percent, so the ceiling form won. Its casualty is
the vCeiling-monotonicity test, and that is not a regression to hide: with a
SHARED budget, a ceiling that lets the tool take a curve faster genuinely leaves
less acceleration to speed up alongside it. The test now asserts the exact
property that survives — raising a ceiling never lowers speed where `κ = 0`, and
where it does lower one, `κ > 0` there.

**5 — a pen no longer pays the A cap (P4).** `compileBlock` gates the A term it
gives `plan` on `tangential`, matching the gate `constrain` already had. A
revolver pen does rotate A, but between operations, and choreograph emits that
motion against A's own limits — it never rides a cutting segment, so it has no
claim on the cutting path's budget.

#### Golden diff

| | packets | jog | cutting | total s |
|---|---|---|---|---|
| `test_circle` before | 576 | 33 | 543 | 4.627 |
| `test_circle` after | 823 | 33 | **790** | 4.643 |
| `fish` before | 5106 | 1056 | 4050 | 86.304 |
| `fish` after | 6136 | 1056 | **5080** | 86.820 |

Jog counts are byte-identical on both — choreograph was not touched, and the diff
is confined to where it should be. The cutting-segment growth (+45% / +25%) is
F7's enforcement buying finer sampling, and duration moves +0.35% / +0.6%.

#### Three things this batch found rather than fixed

**The D4 finding was measured one sample early.** `cornerIndices` returned the
sample BEFORE the tangent jump while the failure message printed the flags of the
sample AFTER it. `discretize` pivots on arrival at the LATER sample of the pair,
so that is the one which must be at rest. Corrected — and the finding was worse
than filed: at the cusp the approach sample read 4.84e-3 mm/s and the sample that
actually pivots read **2.14e-2**, 4.4× higher.

**D3's cause has moved.** The plan-level check is now green — `plan` no longer
asks the A axis for more than its rate ceiling — yet the emitted cut time on
`near_cusp` is still 1.87× the planned one, with A and X both sitting at exactly
1.000 of their ceilings. So `interval`'s floor is no longer rescuing a gross
upstream violation; it is binding at the SUB-SEGMENT level, where subdivision
distributes `da` unevenly across a pair and one sub-segment demands more than the
pair average. That is a different defect wearing D3's name and it wants its own
measurement.

**P3 dissolved into arithmetic.** Its instrument had to change with C1: the old
bound was `vMin²/2a` with `a` the NOMINAL acceleration, which assumes a ramp out
of a stop happens at full accel. Near a cusp the locally available accel is a
small fraction of that, so an honest ramp spends far more distance in the band —
2.45e-2 mm on the cusp, which the old bound called a 196× violation and which is
simply what accelerating from rest costs. The test now asserts what actually
separates the defect from the arithmetic: every below-`vMin` run must touch a full
stop at one end. Verified non-vacuous by disabling C1, which fails it with 107
samples crawling at 1.64e-2 mm/s and no stop at either end.

### Batch C — done (one re-golden; not one byte of cutting motion moved)

H1 and H2 were one defect wearing two hats: neither the A rotation nor the XY
jog derived its timing from an acceleration limit. Both now call one generator,
`rampChunks(N, v0, cruise, accel, fCpu)`, which cuts a move into equal-speed-
increment pieces and gives each the EXACT constant-accel time across it,
`dt = |v_end - v_start| / accel`.

What that replaced: a rate sampled at each chunk's START, with chunk length
`trunc(v / 100)`. Sampling at the start is the slowest point of an accelerating
chunk and the fastest point of a decelerating one — one line, opposite sign on
the two halves of the same move. Deriving the time from the kinematics has no
side to be wrong on.

#### The measurement convention had to change, and that needed justifying

`worstAccelRatio` charged each boundary's speed change to the PRECEDING slice's
duration. That is asymmetric by construction: climbing, the long slice precedes
each boundary; descending, the short one does. A slice's rate is its mean, i.e.
its speed at the slice's time MIDPOINT, so the machine actually has half of each
adjacent slice — `|dv| / ((dt_prev + dt_next) / 2)`.

Changing a metric while fixing what it measures is exactly how a fix gets
faked, so it was checked against the OLD emitter before being adopted:

| N | old, `dt_prev` | old, midpoint | new, midpoint | segments |
|---|---|---|---|---|
| 52 | 0.73 | 0.74 | **1.00** | 5 → 28 |
| 129 | 1.32 | 1.56 | **1.00** | 8 → 31 |
| 500 | 1.45 | 2.01 | **1.00** | 16 → 33 |
| 2325 | 1.65 | 2.70 | **1.00** | 52 → 33 |
| 18600 | 1.48 | 2.13 | **1.00** | 371 → **33** |

The midpoint convention is strictly HARSHER on the pre-fix code (worst 2.70×
against 1.65×, never below 0.74× at any size). Both conventions condemn the old
emitter; only the new one is symmetric. The new emitter lands on exactly 1.00 —
on the limit, not under it, which is the design: the ramp is meant to spend the
whole ceiling. The tests bound it at 1.001 rather than something slack, because
a loose bound here would stop pinning anything.

Segment count fell out for free: the bound is now flat (two ramps plus a cruise
piece, 33 max) where it used to grow with the move. A full turn costs 33
segments instead of 371.

H1b needed restating rather than just flipping. Terminal velocity is not
readable from the stream — the last chunk's rate is its MEAN, and the profile's
true end speed is `v0`. The property that matters physically is that the axis
can reach zero from whatever the final chunk commands, within that chunk's own
duration, and that is what the test now asserts.

#### Golden diff

| | packets | jog | cutting | total s | jog s |
|---|---|---|---|---|---|
| `test_circle` before | 640 | 97 | 543 | 4.641 | 0.964 |
| `test_circle` after | **576** | **33** | 543 | 4.627 | 0.950 |
| `fish` before | 8419 | 4369 | 4050 | 86.185 | 47.280 |
| `fish` after | **5106** | **1056** | 4050 | 86.304 | 47.399 |

The cutting-segment count is untouched on both fixtures and every step total is
identical — this batch could not move cutting motion and did not. `fish` sheds
**39% of its wire packets** while its total duration changes by +0.14%: the
ramps cost about a tenth of a second across the whole job, and buy an emitted
stream that no longer asks either axis for acceleration it does not have.

Mutation-validated: 10 mutants, 8 killed by name immediately. Both survivors
were real test gaps, not equivalence — the fixture gives x and y the SAME
`maxAccel` (so a jog could not tell `min` from `max`) and nothing asserted a
ramped diagonal jog still travels in a straight line. Two tests added; the first
survivor now dies. The second (`round(a) - round(b)` → `round(a - b)`) is kept
as a survivor on the record: the telescoping form is *provably* exact, the
mutant merely happens to land exactly on every input tried.

#### One fixture was wrong, not one caller

`dualHeadMachine` in `test/orchestrate/walk.test.ts` declared no `x/y.maxAccel`,
so the new refusal fired there. Its sibling `singleHeadMachine` and the deployed
`web/demo/config.json` both declare it; the fixture was an incomplete machine,
and it was fixed rather than the refusal being softened.

### Batch A — done (byte-neutral)

Four changes that make the code honest without moving a single emitted byte.
Verified: `test/production/data/*_golden.bin` untouched (`git status` clean) and
the 37 production tests pass unchanged.

- **F6** — `arcLength()` deleted from `geometry.ts` along with its two tests.
  Dead numerics are the most expensive kind of code to carry into C++. The
  `CASES.expected.arcLength` values are left in place as fixture reference data;
  nothing consumes them, which is worth a separate look.
- **P2** — `plan()` now validates that its samples are fully covered by
  contiguous `PATH_START`/`PATH_END` brackets and throws naming the offending
  index range. The check is *coverage*, not merely termination, so a gap between
  two well-formed subpaths is caught too — that case was equally invisible and
  had no test before.
- **H4** — `aMove()` throws rather than substituting 180 deg/s and 2000 deg/s²
  when the A ceilings are 0, naming which knob is missing. Two guards keep this
  from being a regression: an explicit `slew` target still satisfies an
  otherwise-uncapped axis, and `aMove(0)` stays a no-op — `load.ts` builds an
  *absent* axis with 0 ceilings, so a no-op call on a machine with no A axis
  must not throw.
- **H5** — the three redundant `v ≥ v0` guards are kept and commented as
  belt-and-braces, not deleted. The `[1, fCpu]` interval clamp in particular is a
  guard on the arithmetic and *not* a guarantee about `v`; the comment says so,
  because in C++ it would otherwise read as one. They remain equivalent mutants
  by design.

Confirmed while doing this, from `web/demo/config.json` (the deployed machine):

- `a.maxFeed = 100`, `a.maxAccel = 500` — both set, which is what made H4
  byte-neutral. Note the real accel is **500**, not the fixture's 2000.
- `z.maxAccel` is **absent** (⇒ 0), so H3 is blocked on characterizing the Z
  axis on the real machine, not just in the fixture.
- `knife.liftHeight = 2.0`. D5's original wording overstated the case: the Z
  path was dead *under test*, not in production. Corrected in the table above.

---

## Stages pending

| Stage | Status |
|---|---|
| 3 `repair` (`enforceC1`) | **out of scope** — host-side (`production/compileBlock.ts`); the Pico receives `SplineTile` Bezier packets, so the ported pipeline is flatten → constrain → plan → discretize plus `choreograph` |
| 6 `plan` | **audited** — P1–P5 above |
| 8 `discretize` | **audited** — D1–D5 above |
| — `choreograph` | **audited** — H1–H9 above. Not a stage; audited because `discretize` could only see it through one caller. H6–H9 are defects in the TESTS, found by porting them |
| 9 `dutyBreaks` | **out of scope** — host-side, same reason. One follow-up that is *not* an audit: batches B and C change the timeline it schedules knife enable-line resets against (`tool_duty_limits.md` §5), so re-check it once those land |

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
