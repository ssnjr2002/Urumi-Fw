# web/

Browser-side port of the host pipeline for the ATtiny3224 × RP2350 RS485 CNC motion controller. TypeScript, ESM, zero runtime dependencies.

Ports the Python host-side toolpath pipeline (SVG → step events) to TypeScript so it can run in a browser. The pipeline takes an SVG document and produces a flat list of `MicroSegment` wire events — per-axis integer step deltas + clock intervals — ready for serialisation to the RP2350 controller.

---

## Quick start

```sh
pnpm install
pnpm typecheck   # tsc --noEmit
pnpm lint        # eslint .
pnpm test        # vitest run
pnpm test:watch  # vitest (watch mode)
```

Requires Node 18+ and pnpm. Toolchain: TypeScript 5.9, Vitest 2.1, ESLint 9.

---

## Folder structure

```
web/
├── config/              — calibration data types + defaults
│   ├── config.ts          MachineConfig, AxisConfig, ToolProfile, QualityConfig,
│   │                      BusNode, ToolHead, PipelineConfig + factories + presets
│   └── config.test.ts
│
├── svg/                 — SVG ingestion (pipeline stages 1-2)
│   ├── ingest.ts          Parse: SVG text → CubicBezier[] (path commands,
│   │                      shapes, layers). Normalise: px → mm + Y-flip.
│   │                      Single XML parse shared between both stages.
│   └── ingest.test.ts
│
├── toolpath/            — motion planning pipeline (stages 3-8)
│   ├── src/
│   │   ├── geometry.ts      Pt, CubicBezier, cubic, KAPPA, lineToCubic,
│   │   │                    quadToCubic, 2D vector algebra (sub, add, scale,
│   │   │                    length, normalize, angleBetweenDeg, angleDelta),
│   │   │                    Bezier math (bezierPoint, bezierDeriv1/2,
│   │   │                    arcLength, curvature), endpoint tangents
│   │   ├── repair.ts        Stage 3: C1 continuity at curve joins
│   │   ├── sample.ts        Sample interface + PATH_START/PATH_END/
│   │   │                    CURVE_BOUNDARY flags (the pipeline spine)
│   │   ├── flatten.ts       Stage 4: Bezier subpaths → Sample[] (adaptive
│   │   │                    sampling: chord deviation + spacing + tangent)
│   │   ├── constrain.ts     Stage 5: per-sample velocity ceiling
│   │   │                    (centripetal + A-slew + A-accel + junction dev)
│   │   ├── plan.ts          Stage 6: look-ahead feedrate planner
│   │   │                    (backward+forward sweeps, per-axis accel)
│   │   └── discretize.ts    Stage 8: Sample[] → MicroSegment[]
│   │                        (step deltas, tangent tracking, velocity-aware
│   │                        subdivision, calls choreograph at transitions)
│   └── tests/
│       ├── geometry.test.ts
│       ├── repair.test.ts
│       ├── flatten.test.ts
│       ├── constrain.test.ts
│       ├── plan.test.ts
│       ├── discretize.test.ts
│       └── data/
│           ├── repair.cases.ts   8 mock curve fixtures for stage 3
│           └── curves.cases.ts   8 mock curve fixtures for stages 4-8
│
├── choreograph/         — non-cutting motion (stateless, reusable)
│   ├── src/
│   │   └── choreograph.ts   travelJog, aMove (trapezoidal ramp), zMove,
│   │                        pivot (lift-pivot-lower), preOrient (A axis
│   │                        orientation at PATH_START with unwind support)
│   └── tests/
│       └── choreograph.test.ts
│
├── wire/                — wire output format
│   ├── src/
│   │   └── microsegment.ts  MicroSegment interface, flag constants
│   │                        (MICRO_PATH_END, MICRO_LIFT, MICRO_JOG),
│   │                        interval() — clock cycles per major-axis step
│   │                        with XY hypotenuse correction + per-axis rate limits
│   └── tests/
│       └── microsegment.test.ts
│
├── test-setup.ts        — DOMParser polyfill for Node test environment
├── package.json
├── tsconfig.json
├── vitest.config.ts
└── eslint.config.js
```

---

## The pipeline

```
SVG text
  │
  ▼
svg/ingest.ts — stages 1-2 (parse + normalise)
  │            SVG → CubicBezier[] in mm, Y-flipped (machine origin bottom-left)
  │
  ▼
toolpath/repair.ts — stage 3 (C1 continuity)
  │            Enforces tangent continuity at curve joins; bridges gaps,
  │            logs cusps. First stage that needs config (angle tol, gap tol).
  │
  ▼
toolpath/flatten.ts — stage 4 (flatten)
  │            CubicBezier[] → Sample[] (adaptive arc-length sampling with
  │            per-sample curvature). The representation drop: after here
  │            the pipeline sees samples, not curves.
  │
  ▼
toolpath/constrain.ts — stage 5 (constrain)
  │            Sample[] → ConstrainedSample[] (per-sample velocity ceiling:
  │            centripetal, A-slew, A-accel gradient, junction deviation,
  │            corner stops). Pure per-sample, no propagation.
  │
  ▼
toolpath/plan.ts — stage 6 (plan)
  │            ConstrainedSample[] → PlannedSample[] (look-ahead feedrate:
  │            backward decel + forward accel sweeps → acceleration-continuous
  │            by construction, per-axis accel projection).
  │
  ▼
toolpath/discretize.ts — stage 8 (discretize)
  │            PlannedSample[] → MicroSegment[] (step deltas, tangent tracking,
  │            velocity-aware subdivision). Calls choreograph at transitions
  │            (PATH_START, corners, PATH_END) for non-cutting motion.
  │
  ▼
MicroSegment[] — wire events ready for serialisation
```

Stage 7 (choreograph) is not a sequential step — it's called *during* stage 8 at transitions to insert non-cutting motion (jog, Z lift/lower, A pivot, unwind).

---

## Type progression

Each stage produces a richer type that extends its input. The compiler catches skipped stages — `plan` takes `ConstrainedSample[]` and refuses a bare `Sample[]`, so a bug that skips `constrain` can't silently produce infinite-speed planning.

```
Sample                    { x, y, theta, kappa, ds, flags }
  └→ flatten output
ConstrainedSample         Sample + { vCeiling }
  └→ constrain output
PlannedSample             ConstrainedSample + { v }
  └→ plan output
```

`Sample` carries only geometry — no velocity fields, no sentinel `Infinity`/`0` defaults. `ConstrainedSample` and `PlannedSample` are defined in the stages that produce them (`constrain.ts`, `plan.ts`), extending `Sample` via `interface ... extends Sample`.

---

## Design principles

### Stages are pure

No stage imports the config module. Each stage declares an `XxxOptions` interface with only the config fields it consumes — a focused subset, not the full config type. The caller bridges config to stage at the call site:

```typescript
import { qualityConfig } from "./config/config.js";

const q = qualityConfig();
const repaired = enforceC1(curves, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol });
```

Stages 3-6 use focused options interfaces (`RepairOptions`, `FlattenOptions`, `ConstrainOptions`, `PlanOptions`). Stage 8 (`discretize`) takes `MachineConfig` + `ToolProfile` + `QualityConfig` directly as typed parameters — it needs ~20 config values, and a 20-field options object would be unwieldy. The stage is still pure (no `defaultConfig()` calls, no hidden defaults).

### Immutable types

All interfaces use `readonly` fields. Downstream stages return new arrays with updated fields via object spread (`{ ...s, vCeiling: cap }`), never mutating input samples.

### Config-tier comments

Each stage's options interface has a comment documenting which config tiers its fields come from, useful when constructing overrides:

```typescript
/**
 * Six parameters spanning three config tiers:
 *   ToolProfile    -> feedMax, cornerStopAngleDeg
 *   MachineConfig  -> aMax (X/Y accel), aRateDegS (A maxRate), aAccelDegS2 (A accel)
 *   QualityConfig  -> junctionDeviation
 */
export interface ConstrainOptions { ... }
```

---

## Deviations from the Python source

| What | Python | TypeScript | Why |
|---|---|---|---|
| `MachineConfig.active_head` | Runtime state mutated via `replace()` | Renamed to `defaultHead` — static declaration, not runtime selection | Config shouldn't track runtime state |
| `Sample` mutability | Mutable dataclass, accretes `v_ceiling`/`v` in place | Readonly → `ConstrainedSample` → `PlannedSample` type progression | Compiler catches skipped stages; no sentinel defaults |
| Stage config access | `enforce_c1` reaches into `config.default()` when args are `None` | Pure: takes required `XxxOptions`, no config import | Decouples stages from config module; testable in isolation |
| `MicroSegment` location | Inside `pipeline/stages/` | Top-level `wire/` folder | Shared output type for both choreograph and discretize |
| Choreograph | Closures inside `discretize.py` | Top-level `choreograph/` module, stateless functions | Reusable for tool-changing, path-stitching, manual jogging |
| `parse` + `normalise` | Separate files, double XML parse | Merged into `svg/ingest.ts`, single parse | `parseSvgRoot` shared; `loadSvgMm*` walks root once |
| Z axis accel | Single-segment constant-velocity Z moves | Match Python (no ramp) + TODO comment for future trapezoidal refinement | Needs `z.accel` characterized first (currently 0 placeholder) |

---

## Test fixtures

Mock curve fixtures live in `toolpath/tests/data/` and are shared across stages:

- **`repair.cases.ts`** — 8 named `CubicBezier[]` cases for stage 3 (perfect C1, sharp corner, G1-not-C1, gap, multi-bad joins, single curve, near-C1, cusp)
- **`curves.cases.ts`** — 8 named `CubicBezier[]` cases for stages 4-8 (straight line, quarter circles r50/r5, S-curve, short curve, long gentle arc, near-cusp, full circle r30) with expected `arcLength`/`kappaMax` where analytically known

Real SVG fixtures are in `pipeline/data/` (the Python source's test data) — tests read them directly, single source of truth, no duplication.

**176 tests across 10 files**, all passing.

---

## Tooling

| Tool | Version | Purpose |
|---|---|---|
| TypeScript | 5.9 | Type checking (strict mode, `noUncheckedIndexedAccess`) |
| Vitest | 2.1 | Test runner (Node environment, DOMParser polyfilled via `@xmldom/xmldom`) |
| ESLint | 9 | Linting (flat config, `typescript-eslint` recommended) |
| pnpm | 11 | Package manager |

No Vite dev server or browser UI yet — this is a library-only workspace. Vite will be added when UI work starts.
