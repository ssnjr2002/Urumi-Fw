# Porting a planner stage to C++ — the actual workflow

**Last updated:** 2026-07-31 (after stage 7)

This is reconstructed from the session transcript, not from memory. Where it
says a step cost N calls, that is a count. Where it says a step was wasted,
that is a step that actually happened.

Remaining stages: `choreograph` (its own 61 contract tests; the module
itself is ported, as stage 7's dependency).

---

## The test tree

The two kinds of test live in two suites, and PlatformIO runs them separately:

```
test/
  main.cpp              doctest runner — test_dir root, shared into both suites
  support/              bits.h  curves.h  quality.h  svgfix.h
  data/                 generated reference vectors + SVG fixture geometry
  test_contract/        properties, invariants, scaling laws
  test_parity/          bit-equality, no epsilon
```

```bash
pio test -e native -f test_contract
```

Split because the two answer different questions and have different lifespans.
The parity suite fails on any optimisation, correct or not, so it is scaffolding
with a known end date; the contract suite is what survives it. Keeping them apart
means the contract suite can be run alone, without the `*_ref.txt` vectors
present — which is exactly the state the port will be in once bit-parity retires.

**`test/data/` is generated and gitignored.** The generators
(`web/test/port/cppRef*.test.ts`) are the tracked definition; the outputs are
10+ MB and would be re-committed on every fixture change. One command builds all
of them, and every test that reads one fails with that command in its message:

```bash
cd web && GEN_CPP_REF=1 npx vitest run test/port
```

The contract suite needs exactly one of these files — `svg_fixtures.txt`, which
is INPUT geometry rather than expected output (see step 5). So "runs without the
reference vectors" means without the answers, not without the artwork.

Anything shared goes in `test/support/`. `quality.h` duplicates
`web/src/config/defaults.ts` by hand ON PURPOSE: a generated copy would track the
code under test silently, and a contract test whose thresholds move with the
implementation is not a contract.

---

## The headline result

| stage | tool calls | first differential run | outcome |
|---|---|---|---|
| 4 — `flatten` | 51 | **failed** (1 ULP on theta, 9 steps to localise) | 32 cases bit-exact |
| 5 — `constrain` | 50 | **passed** | 75 cases bit-exact |
| 6 — `plan` | ~35 | **passed** (contract tests ran first) | 77 cases bit-exact + 13 contract cases |
| 7 — `discretize` | ~40 | **passed** | 194 cases / 75,797 segments bit-exact + 14 contract cases |

`constrain` is a third the complexity of `flatten`, ported cleanly, and passed
on the first run — **and cost the same.** That is the single most useful thing
in this document, because it predicts the cost of the remaining stages better
than any estimate based on reading the TypeScript does.

The cost did not go into porting. It went into establishing that the
verification was real. In stage 4 that showed up as a test failure that took 9
calls to diagnose; in stage 5 it showed up as a mutation harness that reported
25 survivors out of 25 and took 6 calls to disbelieve.

**Estimate remaining stages by their verification surface, not their line
count.** I estimated `constrain` as "a smaller surface" and was wrong about the
thing that mattered.

---

## The sequence that works

Derived by diffing stages 4, 5 and 6 against each other, then reordered after
stage 6. Steps 2 and 5 are the two places the order was found to matter.

### 1. Size it, and read it whole (2 calls)

```
wc -l constrain.ts        # 244
Read constrain.ts
```

Read the entire TypeScript file before writing anything. Both stages did this
and neither regretted it. The doc comments in `web/src/toolpath/*.ts` carry
audit findings that explain why lines are the way they are — those comments are
the porting spec, and several became C++ comments verbatim.

### 2. Inventory the transcendentals, and measure them BEFORE writing code (3 calls)

**This is the ordering stage 5 got right and stage 4 got wrong.**

Stage 4 wrote `flatten.cpp`, wrote the test, ran it, watched it fail on
`theta`, and spent 9 calls tracing 1 ULP back to `atan2`. Stage 5 grepped the
TypeScript for `Math.*`, saw `Math.cos`, and measured it first:

```
node gencos.js                    # 200k V8 outputs, in the call shape the
                                  # stage actually uses, as IEEE-754 hex
g++ coscheck.cpp && ./coscheck    # mingw:  5,596/200,000 differ, max 26 ULP
g++ coscheck2.cpp && ./coscheck2  # fdlibm: 0/200,000
```

Three calls, before a line of the port existed, and the stage then passed on
its first run.

Rules that came out of this the hard way:

- **200,000 inputs, not 15.** A 15-value probe on `atan2` reported a false
  pass; the real disagreement rate was 17.6%.
- **Sample in the shape the caller actually uses**, plus a wider sweep. The
  `cos` probe used `cos((|turnDeg|·pi/180)/2)` because that is what
  `junctionCap` computes.
- Assume every new transcendental is a mismatch until measured. Four for four
  so far: `atan2`, `acos`, `hypot`, `cos`.
- fdlibm is the first thing to try — V8's `src/base/ieee754.cc` derives from
  it — but not blindly: `Math.hypot` is **not** fdlibm, it is V8's own
  scale-by-max plus Kahan.

### 3. Read the call site, not just the stage (2 calls)

```
grep -rn "constrain(" web/src        -> compileBlock.ts:129
sed -n '115,150p' compileBlock.ts
```

This is what makes the fixtures real rather than invented. `compileBlock`
supplies `pathFeed`, `xyAccel`, `cornerStop` — so the default generator case
uses the numbers that actually bake the golden, not rounder ones chosen for
readability. Stage 4 skipped this and its fixtures are weaker for it.

### 4. Write the four files (4-5 calls)

Always the same four, in this order:

| file | note |
|---|---|
| `lib/motion/motion/<stage>.h` | options struct + declarations |
| `lib/motion/<stage>.cpp` | the transcription |
| `web/test/port/cppRef<Stage>.test.ts` | generator, skipped unless `GEN_CPP_REF=1` |
| `test/test_motion/test_<stage>.cpp` | differential, no epsilon |

The generator is the reference: it emits **inputs alongside outputs**, so the
case list exists in exactly one place and cannot drift between the languages.
Doubles cross as raw 16-hex-digit IEEE-754 — never a decimal round-trip.

Transcription rules, each of which has already caught something:

- `double` everywhere, never `float`.
- Every `Math.x` goes through `motion/jsmath.h`, never `std::x`.
- **Preserve operation order and association.** `min` chains are not
  associative under signed zero; polynomial evaluation order changes low bits.
- `2.0/3.0`, never `2/3`.
- TypeScript optionals need care. `constrain`'s `cornerStopAngleDeg` gets an
  explicit `bool hasCornerStopAngle` because `undefined` means "no corner
  stops" while `0` means "stop at every sample" — collapsing them to a `0`
  sentinel is a catastrophe, not a rounding error. Other optionals whose TS
  destructuring default is `0` and which are then gated on `> 0` can safely use
  `0` as the sentinel.

### 6. Generate the differential vectors, build, run (3-4 calls)

```
cd web && GEN_CPP_REF=1 npx vitest run test/port/cppRefConstrain
pio test -e native
./.pio/build/native/program.exe -s     # assertion counts; pio hides them
```

### 5. Port the contract tests — BEFORE any bit-parity vectors

**This step did not exist for stages 4 and 5, and its absence was a hole.**

Bit-parity proves agreement, never correctness, and it cannot survive an
optimisation — an optimisation is exactly a change that moves the bits. A port
whose whole purpose is to be optimised later cannot have bit-parity as its only
safety net.

So, per stage: port the stage, port its contract tests from
`web/test/toolpath/<stage>.test.ts`, get them green, and only then generate the
differential vectors.

The tests port well because they are already written as properties, not value
pins — the TypeScript files say so in their own headers ("INVARIANTS — ... These
are what the C++ port must reproduce"). Port the invariants and the scaling
laws; use tolerances freely here, because this is the layer that is *supposed*
to survive a rewrite.

Scope: 246 tests across the six port-scope files, not the 709 in the suite (the
rest is wire, config, svg, orchestrate, jog — host stack).

| file | tests |
|---|---|
| `choreograph.test.ts` | 61 |
| `plan.test.ts` | 52 |
| `constrain.test.ts` | 41 |
| `discretize.test.ts` | 39 |
| `geometry.test.ts` | 28 |
| `flatten.test.ts` | 25 |

Evidence that the order matters, from stage 6: the contract tests caught one
real error, and once green the differential passed **first try**. In stage 4 the
differential went first and its first failure was a 1-ULP divergence that took
nine steps to localise, because a bit comparison cannot tell a numeric question
from a semantic one. Contract tests fail with a sentence
(`32 violation(s): straight_line: decel jump at 14`); differentials fail with
two hex strings.

**The debt is paid.** Stages 4 and 5 owed 94 contract tests (`geometry` 28,
`flatten` 25, `constrain` 41); all 94 are ported, plus 2 more that the mutation
run forced (see below), for 96. Every stage in the port now has both suites.

#### What paying the debt actually taught

Porting the 94 owed tests cost ~20 calls, far less than porting a stage, because
the TypeScript originals are already written as properties and the fixtures
already existed. Two things came out of it that were not visible before:

**Real artwork is not optional.** Three of flatten's tests and two of
constrain's assert the caps against `test_snake.svg`, and the C++ has no SVG
parser or `enforceC1` — both are host stack, outside the port. Skipping them
would have dropped the only fixtures that ever caught anything: every cap failure
in the audit (F1, F7) was on real geometry, which accelerates across a step far
harder than a hand-built curve. Solved with
`web/test/port/cppRefFixtures.test.ts`, which exports the repaired Bezier
subpaths and **no expected outputs** — the C++ flattens and constrains them
itself. When the caps were mutated, those SVG tests failed alongside the
synthetic ones, which is the evidence the fixture is load-bearing rather than
decorative.

**Monotonicity has a blind spot, and mutation found it.** The TypeScript declines
to check the curvature-gradient cap directly, because `kappaPrime` is internal,
and asserts monotonicity plus a scaling law instead. Two mutants of `kappaPrime`
survived all 94 ported tests: halving the arc-length span, and dropping the guard
that stops a finite difference straddling a curve boundary. Both are invisible by
construction — they only ever LOWER a ceiling, so monotonicity cannot see them,
and the scaling law is a ratio, so a constant factor on `|k'|` cancels exactly.
The fix was two tests that are two-sided: one asserting the cap binds at exactly
`sqrt(alpha/|k'|)` where it is the active constraint, one asserting the cap
changes nothing at a sample whose difference span straddles a kappa break. Both
are stated in physics terms and neither needs `kappaPrime`.

That is worth noting for the remaining stages: **"tightening never raises" is a
one-sided property and cannot detect a cap that is too tight.** The TypeScript
suite has the same hole, and it is the first defect this port has found in the
tests rather than in the code.

**And it recurred in stage 7, in a different disguise.** `discretize`'s test for
"no segment spans a speed change greater than dvMax" re-derived the subdivision
count `k` with its own `ceil` and checked the result against `dvMax` — so it
audited the ARITHMETIC of the rule while being blind to whether the stage used
that rule. Swapping the stage's `ceil` for `floor` left it green. The general
statement, which now covers both C3 and D6:

> **A test that re-derives the quantity it is auditing agrees with the
> implementation by construction.** It does not matter whether it looks like a
> monotonicity claim (C3) or like a direct check (D6). The tell is that the test
> recomputes something the stage also computes, instead of measuring what the
> stage emitted.

The fix is the same both times: measure the OUTPUT, in the units the next
consumer sees. For C3 that was re-deriving `|k'|` from the public sample stream;
for D6 it was deriving each segment's speed the way the firmware will execute it.
Budget for one such test per cap, and expect to need a noise floor — a derived
speed is quantised to ~1/major, so short segments must be excluded, and the
exclusion needs its own "did anything survive the filter?" assertion.

#### What stage 7 added

**A stage can depend on a later stage.** `discretize` calls `choreograph` at
every transition, so stage 7 could not land without porting stage 8's module.
Do not treat the stage numbering as a dependency order — check the imports
before estimating.

**The config layer does not have to cross.** `discretize` is the one stage whose
TypeScript signature takes config objects rather than a flat options struct, and
it resolves them itself. The port takes the RESULT of that resolution
(`DiscretizeOptions`), so schema/loader/validator/tool-catalogue stayed in
TypeScript. State the cost in the header: the port cannot reproduce a defect that
lives in the fallback chain, only one in what the chain produces.

**Reference vectors have a size budget nobody set.** The first generator emitted
20 MB, because every fixture repeated its full input sample list beside each of a
dozen option variants. Two fixes, in this order: emit the inputs once and
reference them by name (`samples` / `use`), and run the full option sweep only on
the SMALL fixtures — every branch is reachable on a 4 mm line and an elbow, and
the option knobs do not care how long the path is. 20 MB → 10 MB with no coverage
lost. Check the breakdown before cutting: the second half of the reduction was
flat across ~190 cases, so there was nothing left to cut without losing coverage.

**Integer outputs make a differential easier to satisfy and easier to be
complacent about.** Stage 7 emits step counts; most of them agree for reasons
unrelated to the arithmetic. The `interval` and `rampChunks` records exist
because that is where the actual doubles are.

### 7. Mutation-validate — and validate the validator (6-12 calls)

The mutation script lives in the scratchpad and is the least trustworthy part
of this whole workflow. It has now produced a false green **twice**, in
different ways.

Non-negotiable properties, all three learned from a failure:

1. **Restore sources from a `.bak` copy, never `git checkout`.** A prior
   session lost uncommitted work that way.
2. **Verify the mutation applied** — checksum before and after the `sed`. One
   stage-5 mutant was a no-op that would otherwise have been filed as
   equivalent.
3. **Decide kill/survive from `[FAILED]` suite names and the exit code, with an
   explicit `HARNESS-BROKE` branch when neither a PASSED nor a FAILED line
   appears.** Stage 5's first script grepped for a pattern that never matched,
   so silence read as "survived" and all 25 mutants passed. Counts are not
   evidence; names are.
4. **Verify the mutation landed in CODE, not in a comment.** A checksum proves
   the FILE changed, which is not the same claim. A first-occurrence
   string replacement of `3 * ` in `geometry.cpp` hit the doc comment on line 5
   that quotes the expression, and was filed as a survivor — a false finding
   that would have read as "nothing tests bezierDeriv1". Target a line number
   and assert the line is not a comment; re-run properly rather than filing it.
   Three mutants have now been malformed in three different ways, so treat a
   surprising survivor as a suspect harness first and a finding second.

Mutate the stage AND any transcendental it introduced. Roughly 25 mutants:
every epsilon guard, every comparison operator boundary, every gate, every
early return, and one substitution of the platform function for the owned one.

Since the reorder, run the mutants against the CONTRACT tests and record which
of them the contract tests alone can kill. For stage 6 that was 9 of 10 — the
differential was not needed for any. That ratio is the direct measure of how
much safety survives once bit-parity is retired, which is the only number that
matters for the optimisation phase.

### 8. Triage the survivors — they are findings, not gaps (3-6 calls)

A survivor is one of four things, and saying which is the point:

- **Measure-zero boundary** (`>=` -> `>` on an exact equality). Ignorable, say so.
- **Algebraically equivalent** (`(s*s)*s` -> `s*(s*s)`, `angleDelta` args
  swapped where both sites take `fabs`). Prove it, record it.
- **Provably unobservable** — the interesting case. Both stages produced these,
  and they are the same finding: a guard whose dead band sits orders of
  magnitude away from where the thing it gates can bind. Four such guards
  across two stages now. **Do a numeric argument, not another fixture** — stage
  4 wasted 2 calls adding fixtures for guards that no fixture could ever reach.
- **Genuine gap.** Say what would kill it and that it was not built.

### 9. Write the audit section, verify, commit (7-9 calls)

```
Edit docs/planner_audit.md          # measurement tables + mutation table
cd web && npx tsc --noEmit && npx vitest run
pio test -e native
git add <9 files> && git commit
```

Expected TS state: **709 passing, 2 failed, N skipped.** The 2 failures are
known and blocked (`H3: zMove ramps instead of slamming to zFeed`, `FINDING D3:
interval's rate floor is a second speed governor`). Check them **by name** —
matching the count alone would hide a swap. Skipped grows by 1 per stage, as
each generator skips without `GEN_CPP_REF=1`.

---

## What actually wasted calls

Honest accounting, so the next stage does not repeat it.

| waste | calls | fix |
|---|---|---|
| `pio` is not on PATH; tried Bash then PowerShell then hunted the exe | 3 | use `$USERPROFILE/.platformio/penv/Scripts/platformio.exe` directly |
| Bash cwd resets between calls; `cd web && ...` then a follow-up `cd web` failed | 3 | always `cd /c/Users/.../repo && ...` in one command |
| Mutation harness debugging | 6 | build the harness with the three properties above from the start |
| Fixtures added for guards that were unreachable in principle | 2 | do the numeric argument first |
| Generator imports written before the block that used them | 1 | — |

`flatten` also lost 3 calls to a `const char* names[]` that made doctest print
a pointer instead of the field name, which is the same species of problem as
the mutation harness: **the thing reporting the result was itself untested.**

---

## The one principle

Three times in this port a verification step passed for a reason unrelated to
what it claimed to verify:

- the 15-value libm probe (sample far too small to see a 17.6% rate),
- doctest printing a pointer instead of the diverging field name,
- the mutation script whose kill detector never matched anything.

**A check that has never been seen to fail is not evidence.** Before trusting a
new harness, break something on purpose and confirm it says so. Stage 5 caught
its broken mutation script only because 25/25 is not a believable number — a
plausible-looking wrong answer would have gone straight into the audit.
