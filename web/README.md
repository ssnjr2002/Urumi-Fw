# web/

Browser-side port of the host pipeline for the ATtiny3224 × RP2350 RS485 CNC motion controller. TypeScript, ESM, zero runtime dependencies.

Ports the Python host-side toolpath pipeline (SVG → step events) to TypeScript so it can run in a browser or Node. The pipeline takes an SVG document and produces `MicroSegment` wire events — per-axis integer step deltas + clock intervals — grouped into a `.plan` file that the runtime orchestrator streams to the RP2350 controller.

**Parity-verified:** the TS pipeline produces byte-for-byte identical output to the Python pipeline for `test_circle.svg` (641 packets) and `fish.svg` (8437 packets). See [Parity testing](#parity-testing).

---

## Quick start

```sh
pnpm install
pnpm typecheck   # tsc --noEmit
pnpm lint        # eslint .
pnpm test        # vitest run
pnpm test:watch  # vitest (watch mode)
pnpm demo        # open browser demo (Vite, port 5173)
```

Requires Node 18+ and pnpm. Toolchain: TypeScript 5.9, Vitest 2.1, ESLint 9, Vite 8.

---

## Folder structure

```
web/
├── config/              — calibration data types + defaults
│   ├── config.ts          MachineConfig, AxisConfig, ToolProfile, QualityConfig,
│   │                      BusNode (nodeId + present flag), ToolHead (Z+A socket,
│   │                      optional seed mount), PipelineConfig + factories + presets.
│   │                      ReferencePoint, LaserPointer, ToolOffset,
│   │                      REVOLVER_PEN preset (7-slot rotating pen module).
│   ├── helpers.ts         Policy helpers: toolForLayer (layer name → ToolProfile),
│   │                      requiredAxes (which axes a tool needs), canRunTool
│   │                      (bus node-presence feasibility gate, not a mount check).
│   ├── configLoader.ts    JSON → PipelineConfig parser. Production config source:
│   │                      machine calibration from config.json (required: fCpu,
│   │                      stepsPerUnit, nodeId, heads). Returns { ok, config } |
│   │                      { ok: false, errors }.
│   ├── config.test.ts
│   ├── helpers.test.ts
│   └── configLoader.test.ts
│
├── svg/                 — SVG ingestion (pipeline stages 1-2)
│   ├── ingest.ts          SVG text → CubicBezier[] in mm, Y-flipped.
│   │                      Layer-aware: nested <g> groups build '/'-separated
│   │                      keys (e.g. "revolver_pen/slot1") for the revolver pen.
│   └── ingest.test.ts
│
├── toolpath/            — motion planning pipeline (stages 3-8)
│   ├── src/
│   │   ├── geometry.ts      Pt, CubicBezier, 2D vector algebra, Bezier math
│   │   ├── repair.ts        Stage 3: C1 continuity at curve joins
│   │   ├── sample.ts        Sample interface + PATH_START/PATH_END flags
│   │   ├── flatten.ts       Stage 4: Bezier subpaths → Sample[]
│   │   ├── constrain.ts     Stage 5: per-sample velocity ceiling
│   │   ├── plan.ts          Stage 6: look-ahead feedrate planner
│   │   └── discretize.ts    Stage 8: Sample[] → MicroSegment[], calls
│   │                        choreograph at transitions (PATH_START, corners,
│   │                        PATH_END)
│   └── tests/
│
├── choreograph/         — non-cutting motion (stateless helpers)
│   ├── src/
│   │   └── choreograph.ts   zMove, zStepCount, aMove (trapezoidal ramp),
│   │                        pivot (lift-pivot-lower), travelJog, preOrient
│   │                        (A pre-orientation at PATH_START with unwind),
│   │                        aMoveTo (absolute A move — A-home + revolver slot),
│   │                        headOffsetJog (XY compensation on head switch)
│   └── tests/
│       └── choreograph.test.ts
│
├── wire/                — wire output format
│   ├── src/
│   │   ├── microsegment.ts  MicroSegment, flag constants (MICRO_PATH_END,
│   │   │                    MICRO_LIFT, MICRO_JOG), interval() helper
│   │   └── packet.ts        26-byte MicroSegment wire packer (crc8 0x8C)
│   └── tests/
│
├── plan/                — the Block / Plan job model + .plan codec
│   ├── src/
│   │   ├── plan.ts          Block { profile, slot?, segments, startSteps },
│   │   │                    Plan { blocks }, planToolTypes, feasibleOn
│   │   │                    (node-presence gate, not a mount check)
│   │   └── planFile.ts      .plan binary codec: magic AB CD 50 02, tool manifest,
│   │                        slot-aware op headers (tool_type | slot | pkt_count).
│   │                        savePlan / loadPlan round-trip.
│   └── tests/
│       ├── plan.test.ts
│       └── planFile.test.ts
│
├── production/          — full pipeline glue: SVG + config → .plan
│   ├── compileBlock.ts    Stage 3-8 chain for one SVG layer. Applies -toolOffset
│   │                      to all path coordinates (bake-time geometry shift into
│   │                      head-center space) before running the stage chain.
│   │                      Returns { segments, startSteps }.
│   ├── bakePlan.ts        config + SVG text → { plan, bytes }. Walks SVG layers
│   │                      in document order; resolves layer names to tools; handles
│   │                      revolver slot sub-layers; calls compileBlock per block.
│   └── tests/
│       ├── bakePlan.test.ts      multi-layer, revolver slots, toolOffset shift,
│       │                         round-trip, error paths
│       ├── parity.test.ts        byte-for-byte parity vs Python reference .bin
│       ├── svgToPackets.ts       FROZEN parity harness — do not import from
│       │                         production code, do not clean up
│       └── svgToPackets.test.ts
│
├── orchestrate/         — mount scheduling + runtime walk
│   ├── src/
│   │   ├── schedule.ts    scheduleMounts(plan, headCount, seedMounted?) →
│   │   │                  Schedule. Greedy fill/execute/pause/swap loop:
│   │   │                  batches blocks into phases, each phase carrying
│   │   │                  { mount, blockIndices, swapIn, swapOut }. Document
│   │   │                  order preserved; revolver slots stay intra-phase.
│   │   └── walk.ts        walkSchedule(schedule, plan, machine, opts) →
│   │                      WalkEvent[]. Drives a Schedule: emits motion events
│   │                      (MicroSegment[]) and pause events (swapIn/swapOut)
│   │                      in execution order. Handles A-home, revolver slot
│   │                      rotation, inter-block travelJog, headOffsetJog.
│   └── tests/
│       ├── schedule.test.ts
│       └── walk.test.ts
│
├── demo/                — browser demo (Vite)
│   ├── index.html         Dark two-panel UI: config.json + SVG pickers → Bake
│   │                      button → summary + Download .plan
│   ├── main.js            parseConfig → bakePlan → blob download (JS, not TS)
│   ├── config.json        Real machine calibration (160 steps/mm, 51.667 steps/°,
│   │                      fCpu 150 MHz, X/Y/Z/A nodes 1-4)
│   └── demo.svg           Sample layered SVG (knife + crease layers)
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
  ▼  svg/ingest.ts (stages 1-2)
     SVG → CubicBezier[] per layer, mm, Y-flipped
  │
  ▼  production/compileBlock.ts  (per block)
  │    apply -toolOffset  →  head-center coordinates
  │    stage 3: repair    →  C1 continuity
  │    stage 4: flatten   →  Sample[]
  │    stage 5: constrain →  ConstrainedSample[]  (velocity ceilings)
  │    stage 6: plan      →  PlannedSample[]      (look-ahead feedrate)
  │    stage 8: discretize → MicroSegment[]       (step deltas + intervals)
  │         └── choreograph called at PATH_START / corners / PATH_END
  │              for intra-block non-cutting motion
  │
  ▼  production/bakePlan.ts
     assemble Block[] in layer order → Plan → savePlan → .plan bytes
  │
  ▼  orchestrate/schedule.ts  (offline, once)
     Plan + headCount → Schedule (fill/execute/pause/swap phases)
  │
  ▼  orchestrate/walk.ts  (runtime, per execution)
     Schedule + Plan + MachineConfig → WalkEvent[]
       motion events: MicroSegment[] ready to stream
       pause events:  swapIn/swapOut for operator
```

Stage 7 (choreograph) is not a sequential step — it is called *during* stage 8 at transitions, and independently by the runtime walk for inter-block motion.

---

## Bake-time vs run-time split

A fundamental split: **intra-block** motion is baked offline; **inter-block** motion is generated at run-time.

### Baked into each block (offline, `compileBlock`)

- **Cutting motion** — step deltas + intervals for the toolpath
- **Intra-block travel** — jog between subpaths within the block
- **A pre-orientation** — rotate A to entry tangent at each `PATH_START`
- **Corner pivots** — lift-pivot-lower at sharp corners
- **Z lift/lower** — around each subpath stroke

The block is fully self-contained. The stage chain initialises its state (posX/posY/aPhys = 0) at block start. **Crucially: compiled blocks assume aPhys = 0 at block entry** — the walk enforces this by A-homing before every tangential or revolver block.

### Run-time (orchestrator)

Inter-block motion depends on actual machine position — state the orchestrator tracks live.

`walkSchedule` emits between each block:
1. **A-home to 0°** — so the block's compiled `preOrient` is correct
2. **Revolver slot selection** — `aMoveTo(slotOffsets[slot], aPhys, axes)`
3. **Travel jog** — from current posX/posY to `block.startSteps`
4. **Head-offset jog** — when switching heads (from `headOffsetJog`)

At phase boundaries it emits a **pause event** (`swapIn`, `swapOut`) before the inter-block motion, so the caller can prompt the operator to swap tools.

---

## Tool offset (bake-time geometry shift)

`ToolProfile.toolOffset` is the fixed XY offset of the tool tip from the head center (e.g. the revolver pen's active tip is not at the head center). The machine always tracks the **head center** — so to cut the right path the coordinates must be re-expressed in head-center space.

`compileBlock` applies `−toolOffset` to all subpath coordinates **before** the stage chain. After this shift everything is in head-center coordinates permanently — there is nothing to revert at block end. `startSteps` is also computed from the shifted first point, so it is already in head-center coordinates when the walk uses it for the travel jog.

`headOffset` (the head's XY position relative to the machine reference) is the complementary runtime concern, handled by `headOffsetJog` on head switch.

| Offset | Field | Space | Applied by |
|---|---|---|---|
| Head offset | `ToolHead.xOffset/yOffset` | Machine ref → head center | Runtime (`headOffsetJog`) |
| Tool offset | `ToolProfile.toolOffset` | Head center → tool tip | Bake-time (`-toolOffset` shift in `compileBlock`) |
| Blade offset | `ToolProfile.offsetMm` | Along travel direction | Not yet implemented |

---

## Mount model

A `ToolHead` is a **socket** (Z + A wiring + XY position). `profile?` is an optional **seed mount** — which tool boots in that socket. It is not authoritative at runtime: operators swap tools without editing config.

The runtime orchestrator maintains a **mount table** (`Map<headIndex, toolType>`) seeded from `profile?` and updated on every swap. The walk takes a `headAssignment` map (`ToolType → headIndex`) that reflects the current physical state.

**Bake feasibility** (`feasibleOn` / `canRunTool`) is a **bus node-presence check**, not a mount check. For each tool a plan uses it verifies that the required axis nodes (`BusNode.present`) are wired on the bus:
- X and Y nodes: always
- Z node: if the tool lifts (`liftHeight > 0`)
- A node: if the tool steers A (`tangential` or `slotOffsets` present)
- Peripheral roles: each entry in `requiredPeripheralRoles`

`present` means wired/attached, not alive (no ping issued).

---

## Fill / execute / pause / swap loop

`scheduleMounts(plan, headCount, seedMounted?)` batches a plan into phases using a greedy algorithm:

1. **Fill** — from the current block, take the first `headCount` distinct tool types needed.
2. **Execute** — run all contiguous blocks whose tool is in the mounted set.
3. **Pause** — stop at the first block with an unmounted tool.
4. **Swap** — that block begins the next phase; compute the new mount set.

Each `Phase` carries `mount`, `blockIndices`, `swapIn` (tools to load), `swapOut` (tools to remove). Document order is always preserved — blocks are never reordered. Revolver slots share a single `toolType` so they stay intra-phase; the walk emits `aMoveTo(slotOffset)` between revolver blocks as needed.

---

## Config: code defaults vs config.json

The production config path is `parseConfig(jsonText) → PipelineConfig`. Machine-specific calibration comes from a `config.json` — **required**, no silent fallback. The code provides universal defaults (tool presets, quality tuning) with optional JSON overrides.

**Required in JSON:** `machine.fCpu`, `machine.x`, `machine.y`, `heads[]` (non-empty), each axis's `node.nodeId` + `stepsPerUnit`, each head's `tool`.

**Optional in JSON:** `jogFeed` (80), `zFeed` (20), `laser`, axis `maxRate`/`accel` (0), `invert` (false), `maxTravel`, head `xOffset`/`yOffset` (0), `defaultHead` (0), `peripherals`, `tools`, `quality`.

`defaultMachine()` in `config.ts` (160/1200/51.667) is a **test fixture only**. The production path uses `configLoader`.

---

## Type progression

```
Sample              { x, y, theta, kappa, ds, flags }
  └→ flatten
ConstrainedSample   Sample + { vCeiling }
  └→ constrain
PlannedSample       ConstrainedSample + { v }
  └→ plan
```

Distinct types prevent skipped stages from compiling silently — `plan()` takes `ConstrainedSample[]` and rejects a bare `Sample[]`.

---

## Deviations from the Python source

| What | Python | TypeScript | Why |
|---|---|---|---|
| Production bake | `svg_to_packets.py` (flat) | `bakePlan` → `Plan` → `.plan` file | Slot-aware multi-tool model; plan/run split |
| `.plan` format | Opaque `.bin` (no slots) | `AB CD 50 02` magic, tool manifest, slot field per op | Revolver slot metadata needed by orchestrator |
| Tool offset | Not modeled | `ToolProfile.toolOffset`, applied as bake-time `-toolOffset` shift in `compileBlock` | Revolver pen tip is offset from head center |
| Revolver pen | Not modeled | `ToolType.REVOLVER_PEN`, `slotOffsets`, slot sub-layers | 7-slot rotating pen module |
| Mount model | `ToolHead.profile` required | `ToolHead.profile?` (seed only); runtime mount table | Operators swap tools without re-baking |
| Feasibility gate | Mount check | Bus node-presence check (`BusNode.present`) | Mount state is runtime, not bake-time |
| Orchestrator | `orchestrate.py` (stateful) | `schedule.ts` (pure batcher) + `walk.ts` (event emitter) | Clean bake/run split; walk is side-effect free |
| `active_head` | Runtime mutable | `defaultHead` — static declaration | Config shouldn't track runtime state |
| `Sample` mutability | Mutable dataclass | Readonly → type progression | Compiler catches skipped stages |
| `**` operator | `speed ** 3` (C `pow()`) | `speed * speed * speed` (IEEE 754) | 3-packet divergence in fish.svg; Python fixed to match |
| Choreograph | Closures inside `discretize.py` | Top-level `choreograph/` module, stateless | Reusable for tool-changing and manual jogging |
| Z axis accel | — | No ramp (constant velocity, matching Python) + TODO comment | `z.accel` not yet characterized |

---

## Parity testing

`production/tests/parity.test.ts` compares TS-baked `.bin` output byte-for-byte against Python reference files. A passing test means the entire TS pipeline (stages 1-8 + packet packer) is identical to Python.

The frozen parity harness (`production/tests/svgToPackets.ts`) preserves the original `subpathsToPackets` verbatim. `bakePlan.test.ts` asserts that `compileBlock` (with zero toolOffset) reproduces the harness exactly — creating a transitive parity chain: Python → harness → compileBlock.

### Reference fixtures (`production/tests/data/`)

| File | Description |
|---|---|
| `test_circle.svg` | Single circle, 641 packets |
| `fish.svg` | Multi-path fish, 8437 packets |
| `test_circle_knife_ref.bin` | Python-baked reference |
| `fish_knife_ref.bin` | Python-baked reference |

### Regenerating references

```sh
python -m host.production.svg_to_packets test_circle.svg --out test_circle_knife_ref.bin
python -m host.production.svg_to_packets fish.svg --out fish_knife_ref.bin
```

Run from `web/production/tests/data/`. Uses default config (KNIFE, default machine — see `config.txt`).

---

## Browser demo

```sh
pnpm demo   # starts Vite on port 5173
```

`demo/index.html` — two-panel dark UI. Left: load `config.json` + SVG file + optional default tool name, click **Bake plan**. Right: SVG preview. Output: block summary + **Download .plan** button.

`demo/config.json` — real machine calibration. `demo/demo.svg` — layered knife + crease SVG.

---

## Tooling

| Tool | Version | Purpose |
|---|---|---|
| TypeScript | 5.9 | Type checking (strict, `noUncheckedIndexedAccess`) |
| Vitest | 2.1 | Test runner (Node, DOMParser polyfilled) |
| Vite | 8 | Dev server for browser demo |
| ESLint | 9 | Linting (flat config, typescript-eslint) |
| pnpm | 11 | Package manager |

**327 tests across 20 files**, all passing.
