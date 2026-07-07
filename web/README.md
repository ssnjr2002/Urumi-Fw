# web/

Browser-side port of the host pipeline for the ATtiny3224 × RP2350 RS485 CNC motion controller. TypeScript, ESM, zero runtime dependencies.

Ports the Python host-side toolpath pipeline (SVG → step events) to TypeScript so it can run in a browser. The pipeline takes an SVG document and produces a flat list of `MicroSegment` wire events — per-axis integer step deltas + clock intervals — ready for serialisation to the RP2350 controller.

**Parity-verified:** the TS pipeline produces byte-for-byte identical `.bin` output to the Python pipeline for both `test_circle.svg` (641 packets) and `fish.svg` (8437 packets), baked with identical default config. See [Parity testing](#parity-testing).

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
│   │                      BusNode, ToolHead, PipelineConfig + factories + presets.
│   │                      ReferencePoint (xOffset/yOffset), LaserPointer,
│   │                      ToolOffset (tool tip from head center), REVOLVER_PEN
│   │                      preset (7-slot rotating pen module), slotOffsets.
│   ├── configLoader.ts    JSON → PipelineConfig parser. The production config
│   │                      source: machine calibration from config.json (required:
│   │                      fCpu, stepsPerUnit, nodeId, heads, tool), tool presets
│   │                      + quality from code defaults with optional JSON overrides.
│   │                      Returns { ok, config } | { ok: false, errors }.
│   ├── test-machine.json  Test fixture matching defaultMachine() (160/1200/51.667).
│   ├── config.test.ts
│   └── configLoader.test.ts
│
├── svg/                 — SVG ingestion (pipeline stages 1-2)
│   ├── ingest.ts          Parse: SVG text → CubicBezier[] (path commands,
│   │                      shapes, layers). Normalise: px → mm + Y-flip.
│   │                      Single XML parse shared between both stages.
│   │                      Layer-aware: nested <g> groups build '/'-separated
│   │                      keys (e.g. "pen_revolver/slot1") for the revolver pen.
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
│   │                        orientation at PATH_START with unwind support),
│   │                        aMoveTo (absolute A move — A-home, revolver slot
│   │                        selection), headOffsetJog (XY compensation jog
│   │                        for head switching)
│   └── tests/
│       └── choreograph.test.ts
│
├── wire/                — wire output format
│   ├── src/
│   │   ├── microsegment.ts  MicroSegment interface, flag constants
│   │   │                    (MICRO_PATH_END, MICRO_LIFT, MICRO_JOG),
│   │   │                    interval() — clock cycles per major-axis step
│   │   │                    with XY hypotenuse correction + per-axis rate limits
│   │   └── packet.ts        26-byte MicroSegment wire packet packer:
│   │                        crc8 (poly 0x8C), packMicrosegment, serialise,
│   │                        writeStream (length-prefixed framing), decodePacket
│   └── tests/
│       ├── microsegment.test.ts
│       └── packet.test.ts
│
├── production/          — SVG → .bin bake (full pipeline glue)
│   ├── svgToPackets.ts      subpathsToPackets (stage 3-8 chain) + bakeBin
│   │                        (SVG text → framed .bin bytes). Bridges config
│   │                        to each stage's focused options interface.
│   └── tests/
│       ├── svgToPackets.test.ts  smoke tests (chain runs, CRC valid, framing)
│       ├── parity.test.ts        byte-for-byte parity vs Python reference .bin
│       └── data/                 fixtures: SVGs, Python reference bins, config.txt
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

## Bake-time vs run-time motion

A fundamental split: **intra-block** motion is baked offline; **inter-block** motion is generated at run-time by a future orchestrator. A *block* is one SVG tool layer (or a merged group of same-tool layers) — the unit the pipeline bakes independently.

### Baked into the `.bin` (offline, per-block)

The pipeline (`subpathsToPackets`) bakes all motion *within* a block:

- **Cutting motion** — step deltas + intervals for the toolpath itself
- **Intra-block travel** — jog between subpaths within the same block (the `discretize` walk emits `travelJog` at each subpath transition)
- **A pre-orientation** — rotating A to the entry tangent before each subpath (`preOrient` at `PATH_START`)
- **Corner pivots** — lift-pivot-lower at sharp corners within a block (`pivot` when the tangent jump exceeds `cornerAngleDeg`)
- **Subpath Z lift/lower** — raise Z after each subpath, lower before the next (when `liftHeight > 0`)

This motion is self-contained: the pipeline's state (position accumulators, A rotation, velocity) is initialised at block start, used during execution, and discarded at block end. The pipeline has no knowledge of what came before or what comes next.

### Run-time (future orchestrator, not built yet)

Inter-block motion depends on the machine's actual position — runtime state an orchestrator tracks. The choreograph module provides stateless helpers for this:

- **Initial jog** from home to first block start — `travelJog(0, 0, firstX, firstY, ...)`
- **A-home to 0°** between blocks — `aMoveTo(0, aPhys, axes)` (absolute return, not unwind)
- **Z-lift/lower** at block boundaries — `zMove(+zSteps, ...)` / `zMove(-zSteps, ...)`
- **Head offset compensation** when switching heads — `headOffsetJog(fromHead, toHead, ...)`
- **Revolver slot selection** — `aMoveTo(slotOffsets[i], aPhys, axes)`
- **Block-to-block travel** — `travelJog(posX, posY, nextBlockX, nextBlockY, ...)`

The orchestrator holds global state (`posX`, `posY`, `aPhys`, `currentHead`) and calls these helpers at each block transition. The helpers are stateless — they take the current state as parameters and return new state. This matches the strategy doc (`docs/multi_tool_orchestration_strategy.md`): "Block-Scoped Pipeline, Global Orchestrator."

### Why the split?

The jog between blocks depends on where the machine actually is — which may diverge from the baked expectation if the operator paused, manually jogged, or resumed mid-job. Baking inter-block motion would be wrong the moment something interrupts execution. The run-time orchestrator generates it from actual machine state instead.

---

## Multi-tool and multi-head support

### Config model

The config model supports machines with 1-2 heads, an optional laser pointer reference, and tools with per-slot A-axis offsets (the revolver pen):

**Reference points** — `ReferencePoint` (`{ xOffset, yOffset }` in mm) is the shared interface for:
- `ToolHead.xOffset` / `ToolHead.yOffset` — head center vs machine reference
- `MachineConfig.laser?` — laser pointer position (passive alignment aid, no axes)
- `ToolProfile.toolOffset` — tool tip vs head center (fixed XY, applied as bake-time geometry shift)

Convention: whichever party has `(0, 0)` defines the machine reference. In a dual-head + laser setup, the laser is at `(0, 0)`, heads at `(-50, 0)` and `(+50, 0)`. In a single-head setup, the head is at `(0, 0)`, no laser.

**Tool types** — `ToolType` enum:
| Type | Value | Description |
|---|---|---|
| `PEN` | `0x01` | Non-tangential pen |
| `KNIFE` | `0x02` | Tangential knife (wired, unwind) |
| `CREASE` | `0x03` | Tangential crease wheel (free-spinning) |
| `REVOLVER_PEN` | `0x04` | 7-slot rotating pen module |

**Revolver pen** — `REVOLVER_PEN` preset: 7 slots at 360/7 ≈ 51.43° intervals, non-tangential (A is for slot selection, not tangent tracking). The `slotOffsets` array carries the A-axis angle for each slot. The orchestrator jogs A to `slotOffsets[i]` before cutting with slot i.

### SVG layer encoding

Layer names drive tool selection. The current convention:
- Single-level: layer name = tool name (`knife`, `pen`, `crease`)
- Nested (revolver): `<g inkscape:label="pen_revolver"><g inkscape:label="slot1">` → layer key `"pen_revolver/slot1"`

`loadSvgLayers` and `loadSvgMmLayers` build `'/'`-separated keys for nested groups. Single-level layers are unchanged (no leading `/`). Unnamed `<g>`s pass the parent layer through.

### Three offset layers

| Offset | Field | What it measures | Applied where |
|---|---|---|---|
| Head offset | `ToolHead.xOffset/yOffset` | Head center vs machine reference | Run-time (orchestrator head-switch jog) |
| Tool offset | `ToolProfile.toolOffset` | Tool tip vs head center | Bake-time geometry shift (`-toolOffset` to all paths) |
| Blade offset | `ToolProfile.offsetMm` | Knife caster (along travel direction, rotates with A) | Not yet implemented (raises in discretize if > tolerance) |

The total offset from machine reference to tool tip = `headOffset + toolOffset`. The bake-time geometry shift handles `toolOffset`; the run-time orchestrator handles `headOffset`. They compose without interfering.

---

## Config: code defaults vs config.json

The production config path is `configLoader.parse(jsonText) → PipelineConfig`. Machine-specific calibration (stepsPerUnit, fCpu, invert, node bindings, head layout, laser pointer) comes from a `config.json` file — **required**, no silent fallback to hardcoded machine values. The code provides universal defaults (tool presets, quality algorithm tuning) which the JSON can override but does not redefine from scratch.

**Required in JSON** (missing → error): `machine.fCpu`, `machine.x`, `machine.y`, `heads[]` (non-empty), each axis's `node.nodeId` + `stepsPerUnit`, each head's `tool` (must be a known preset name).

**Optional in JSON** (absent → documented code default): `jogFeed` (80), `zFeed` (20), `laser` (none), axis `maxRate`/`accel` (0 = unlimited), `invert` (false), `maxTravel` (0), `rotary` (false), head `xOffset`/`yOffset` (0), `defaultHead` (0), `peripherals` ([]), `tools` (no patches), `quality` (code defaults).

`parseConfig` returns `{ ok: true, config }` or `{ ok: false, errors }` with every problem found (not just the first). No validation yet (range checks, duplicate node IDs) — just parsing + required-field checking.

`defaultMachine()` in `config.ts` (the hardcoded 160/1200/51.667 machine) is a **test fixture only** — the parity tests use it. The production path uses `configLoader.parse(json)`.

### What stays as code vs what comes from config.json

| Stays as code (universal) | Comes from config.json (per-machine) |
|---|---|
| Tool presets (PEN/KNIFE/CREASE/REVOLVER_PEN) | `stepsPerUnit` per axis |
| `ToolType` enum values | `fCpu` |
| `OFFSET_TOLERANCE_MM` | `invert` per axis |
| `MICRO_*` flag constants | `maxRate`, `accel` per axis |
| `KAPPA` (Bezier constant) | `jogFeed`, `zFeed` |
| Quality defaults (chordTol, dvMax, etc.) | Head layout (offsets, mounted tool) |
| Wire format constants | Laser pointer position |
| Revolver slot count (7) | Node bindings (which BusNode drives which axis) |

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
| `**` operator | `speed ** 3` (calls C `pow()`, not correctly-rounded) | `speed * speed * speed` (IEEE 754 multiplication) | Python `**` differs from `*` in 25.77% of cases by 1-2 ULP; caused 3-packet divergence in fish.svg. Python fixed to match TS. |
| Head offset | `xOffset` only (Y not modeled) | `xOffset` + `yOffset` via `ReferencePoint` | Dual-head machines may have Y offset; symmetric interface |
| Laser pointer | Not modeled | Optional `MachineConfig.laser?: LaserPointer` | Needed as alignment reference for dual-head offset calculations |
| Tool tip offset | Not modeled | `ToolProfile.toolOffset: ToolOffset` (fixed XY from head center) | Revolver pen's active tip is offset from head center |
| Revolver pen | Not modeled | `ToolType.REVOLVER_PEN` + `slotOffsets` + `REVOLVER_PEN` preset | 7-slot rotating pen module; A axis selects slot |
| Nested SVG layers | Flattens to nearest group label | `'/'`-separated path (`"pen_revolver/slot1"`) | Enables revolver slot-per-layer encoding |

---

## Test fixtures

Mock curve fixtures live in `toolpath/tests/data/` and are shared across stages:

- **`repair.cases.ts`** — 8 named `CubicBezier[]` cases for stage 3 (perfect C1, sharp corner, G1-not-C1, gap, multi-bad joins, single curve, near-C1, cusp)
- **`curves.cases.ts`** — 8 named `CubicBezier[]` cases for stages 4-8 (straight line, quarter circles r50/r5, S-curve, short curve, long gentle arc, near-cusp, full circle r30) with expected `arcLength`/`kappaMax` where analytically known

Real SVG fixtures are in `pipeline/data/` (the Python source's test data) — tests read them directly, single source of truth, no duplication. Parity test fixtures (SVGs + Python reference `.bin` files) are in `production/tests/data/`.

**263 tests across 14 files**, all passing.

---

## Parity testing

`production/tests/parity.test.ts` compares TS-baked `.bin` output byte-for-byte against Python-baked reference `.bin` files. This is the headline correctness check — a passing test means the entire TS pipeline (stages 1-8 + wire packet packer) produces identical output to the Python pipeline.

### Fixtures (`production/tests/data/`)

| File | Description |
|---|---|
| `test_circle.svg` | Single circle, single subpath (641 packets) |
| `fish.svg` | Multi-path, multi-subpath fish (8437 packets) |
| `test_circle_knife_ref.bin` | Python-baked reference (checked in) |
| `fish_knife_ref.bin` | Python-baked reference (checked in) |
| `test_circle_knife_ts.bin` | TS-baked output (gitignored, written by test) |
| `fish_knife_ts.bin` | TS-baked output (gitignored, written by test) |
| `config.txt` | Documents the bake config (defaults) |

### Reference generation

```
python -m host.production.svg_to_packets test_circle.svg --out test_circle_knife_ref.bin
python -m host.production.svg_to_packets fish.svg --out fish_knife_ref.bin
```

Run from `web/production/tests/data/`. Uses default config (KNIFE profile, default machine, default quality — see `config.txt` for the full values).

### On mismatch

The test decodes the first divergent packet and prints a field-level diff (dx/dy/dz/da/interval/flags/seq/crc) so we can pinpoint which stage diverged. The fish.svg parity test caught a 3-packet divergence caused by Python's `**` operator (C `pow()`, not correctly-rounded) vs TS's `*` multiplication (IEEE 754) — fixed by changing Python to use multiplication.

---

## Tooling

| Tool | Version | Purpose |
|---|---|---|
| TypeScript | 5.9 | Type checking (strict mode, `noUncheckedIndexedAccess`) |
| Vitest | 2.1 | Test runner (Node environment, DOMParser polyfilled via `@xmldom/xmldom`) |
| ESLint | 9 | Linting (flat config, `typescript-eslint` recommended) |
| pnpm | 11 | Package manager |

No Vite dev server or browser UI yet — this is a library-only workspace. Vite will be added when UI work starts.
