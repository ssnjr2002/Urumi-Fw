# urumi-toolpath

A geometry and motion planning engine that transforms **SVG drawings + a machine config** into **binary wire packets** for real-time CNC/plotter control over RS485.

Built for the [ATtiny3224 × RP2350 RS485 CNC motion controller](https://github.com/ssnjr2002/ATtiny3224xRP2350_RS485_Custom_for_ai). Multi-tool, multi-layer, tangential knife and revolver pen aware. Works in any modern browser and in Node 18+. TypeScript, ESM, zero runtime dependencies.

---

## Table of Contents

- [Installation](#installation)
- [Quick Start](#quick-start)
- [Usage](#usage)
  - [Browser](#browser)
  - [Node.js](#nodejs)
  - [Working with the output](#working-with-the-output)
- [Configuration Reference](#configuration-reference)
- [SVG Authoring Guide](#svg-authoring-guide)
- [Output Format: The MicroSegment Packet](#output-format-the-microsegment-packet)
- [Architecture Overview](#architecture-overview)
- [API Reference](#api-reference)

---

## Installation

```sh
npm install urumi-toolpath
```

Node 18+ or any modern browser. ESM only (`import`, not `require`).

---

## Quick Start

```ts
import { parseConfig, bakePlan } from "urumi-toolpath";

const cfg = parseConfig(await (await fetch("/config.json")).text());
if (!cfg.ok) throw new Error(cfg.errors.join("\n"));

const { plan, bytes } = bakePlan(
    cfg.config,
    await (await fetch("/drawing.svg")).text(),
    { defaultTool: "knife" },
);

console.log(`${plan.blocks.length} blocks, ${bytes.length} bytes`);
// `bytes` is a .plan file — save it or stream it
```

---

## Usage

### Browser

```ts
import {
    parseConfig, bakePlan,
    loadPlan, scheduleMounts, walkSchedule,
} from "urumi-toolpath";

// ── Bake (once, offline) ──────────────────────────────────────────────────────

const cfg = parseConfig(await (await fetch("/config.json")).text());
if (!cfg.ok) throw new Error(cfg.errors.join("\n"));

const { plan, bytes } = bakePlan(
    cfg.config,
    await (await fetch("/drawing.svg")).text(),
    { defaultTool: "knife" },
);

// Save bytes as a .plan file, or keep `plan` in memory for immediate execution.
const a    = document.createElement("a");
a.href     = URL.createObjectURL(new Blob([bytes], { type: "application/octet-stream" }));
a.download = "drawing.plan";
a.click();

// ── Run (per execution) ───────────────────────────────────────────────────────

const job      = loadPlan(bytes);                                // or use `plan` directly
const schedule = scheduleMounts(job, cfg.config.machine.heads.length);

for (const ev of walkSchedule(schedule, job, cfg.config.machine)) {
    if (ev.kind === "motion") {
        await streamToMachine(ev.segments);        // your WebSerial code here
    } else {
        await promptOperator(ev.swapIn, ev.swapOut);   // show a tool-change dialog
    }
}
```

### Node.js

SVG parsing relies on `DOMParser`, which does not exist in Node. Inject one **once at startup** before calling `bakePlan`:

```ts
import { readFileSync, writeFileSync } from "node:fs";
import { DOMParser } from "@xmldom/xmldom";         // your dependency, not ours
import { parseConfig, bakePlan, setDOMParser } from "urumi-toolpath";

setDOMParser(() => new DOMParser());                // call before bakePlan

const cfg = parseConfig(readFileSync("config.json", "utf-8"));
if (!cfg.ok) throw new Error(cfg.errors.join("\n"));

const { bytes } = bakePlan(cfg.config, readFileSync("drawing.svg", "utf-8"));
writeFileSync("drawing.plan", bytes);
```

Any object with `parseFromString(source, mimeType)` works — it satisfies the `DOMParserLike` structural type. Pass `null` to restore the global `DOMParser` default.

### Working with the output

`bakePlan` returns two things:

| Field | Type | Description |
|---|---|---|
| `plan` | `Plan` | In-memory job model: `blocks[]`, each a `ToolProfile` + `MicroSegment[]` |
| `bytes` | `Uint8Array` | `.plan` binary — round-trips losslessly through `savePlan` / `loadPlan` |

**Inspecting blocks:**

```ts
for (const block of plan.blocks) {
    console.log(block.profile.name, "→", block.segments.length, "segments");
}
```

**Round-tripping the file:**

```ts
import { savePlan, loadPlan } from "urumi-toolpath";

const bytes = savePlan(plan);     // Plan → Uint8Array
const plan2 = loadPlan(bytes);    // Uint8Array → Plan  (structurally identical)
```

**Turning segments into wire bytes:**

```ts
import { packMicrosegment, writeStream } from "urumi-toolpath";

const packets = segments.map((seg, i) => packMicrosegment(seg, i & 0xFF));
const framed  = writeStream(packets);  // [u16 LE len][26-byte packet] × n
```

> **Transport is not included.** This package produces bytes; it does not open a serial port. The reference WebSerial driver is in `demo/transport.js` in the repository — copy it or write your own around `packMicrosegment` and `writeStream`.

---

## Configuration Reference

All machine calibration comes from a JSON string passed to `parseConfig`. There are no silent defaults for machine values — if a required field is missing or invalid, `parseConfig` returns `{ ok: false, errors }` listing every problem at once.

**Minimal working config:**

```json
{
  "machine": {
    "fCpu": 150000000,
    "x": { "stepsPerUnit": 26.5, "node": { "nodeId": 1 } },
    "y": { "stepsPerUnit": 26.5, "node": { "nodeId": 2 } },
    "heads": [{ "tool": "knife" }]
  }
}
```

### Axes

`x` and `y` are required. `z` and `a` are optional but needed for Z-lift and tangential / revolver tools.

| Field | Type | Default | Description |
|---|---|---|---|
| `stepsPerUnit` | `number` | **required** | Steps per mm (X/Y/Z) or per degree (A). E.g. `26.5` for a GT2 belt, 200-step motor, 1/8 microstepping. |
| `node.nodeId` | `number` | **required** | RS485 node address (1–4) wired to this axis's stepper driver. |
| `node.present` | `boolean` | `true` | Whether this node is physically wired. Feasibility checks use this — `false` for axes your build omits. |
| `maxRate` | `number` | `0` | Maximum step rate (steps/sec). `0` = unconstrained. |
| `accel` | `number` | `0` | Acceleration (steps/sec²). `0` = constant velocity, no ramp. |
| `invert` | `boolean` | `false` | Invert step direction signal. |
| `maxTravel` | `number` | — | Soft travel limit (mm or degrees). |

### Global machine fields

| Field | Type | Default | Description |
|---|---|---|---|
| `fCpu` | `number` | **required** | Controller clock frequency in Hz. The RP2350 runs at `150000000`. Converts velocities to step intervals. |
| `jogFeed` | `number` | `80` | Travel jog feed rate (mm/s) for inter-block moves between cuts. |
| `zFeed` | `number` | `20` | Z-axis feed rate (mm/s) for lift and plunge moves. |

### Tool heads

Each entry in `heads[]` is a physical socket on the machine (one Z+A axis set, at a given XY position).

```json
"heads": [
  { "tool": "knife",  "xOffset": 0  },
  { "tool": "crease", "xOffset": 50 }
]
```

| Field | Type | Default | Description |
|---|---|---|---|
| `tool` | `string` | **required** | Seed tool in this socket at boot: `"knife"`, `"pen"`, `"crease"`, or `"revolver_pen"`. The orchestrator tracks actual mounts at runtime and updates them on every swap. |
| `xOffset` | `number` | `0` | X distance (mm) from machine reference to this head center. |
| `yOffset` | `number` | `0` | Y distance (mm) from machine reference to this head center. |

### Quality tuning

Optional `quality` block. Defaults work for most knife-cutting applications.

| Field | Type | Default | Description |
|---|---|---|---|
| `segmentLength` | `number` | `0.5` | Maximum chord length (mm) when flattening Bézier curves to line segments. Lower = smoother arcs, slower bake. |
| `cornerAngle` | `number` | `15` | If the required tool heading changes by more than this many degrees, the planner inserts a **lift → orient → plunge** sequence. Lower = tighter corner fidelity; higher = fewer interruptions on gentle curves. |

### Custom tools

Four presets ship with the package: `PEN`, `KNIFE`, `CREASE`, `REVOLVER_PEN`. Override any field per-tool in the `tools` block:

```json
"tools": {
  "knife": {
    "feedRate":    200,
    "liftHeight":  3,
    "plungeDepth": -1.5
  }
}
```

| Field | Type | Description |
|---|---|---|
| `feedRate` | `number` | Cutting feed rate (mm/s). |
| `liftHeight` | `number` | Z height (mm) for safe travel between paths. `0` = no lift. |
| `plungeDepth` | `number` | Z depth (mm) at cut start. |
| `tangential` | `boolean` | Steer the A axis to follow path tangent. `true` for knife, `false` for pen. |
| `toolOffset` | `{ x, y }` | Fixed XY offset (mm) of the tool tip from head center. Applied at bake time as a coordinate shift. |
| `slotOffsets` | `number[]` | A-axis angles (degrees) for each revolver slot. Present on `REVOLVER_PEN` only. |

---

## SVG Authoring Guide

### Multi-tool: layer mapping

The ingestor maps each `<g>` group to a layer name, then `bakePlan` maps that name to a tool. A `<g>` is identified by its **`inkscape:label` attribute first, then its `id` attribute** as a fallback — so a plain `<g id="knife">` works without any Inkscape namespace:

```svg
<!-- plain SVG — id is the layer name -->
<svg>
  <g id="knife">
    <path d="M10,10 L90,10 …" />
  </g>
  <g id="crease">
    <path d="M10,50 L90,50 …" />
  </g>
</svg>
```

```svg
<!-- Inkscape SVG — inkscape:label takes priority over id when both are present -->
<svg xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape">
  <g inkscape:label="knife"  inkscape:groupmode="layer">
    <path d="M10,10 L90,10 …" />
  </g>
  <g inkscape:label="crease" inkscape:groupmode="layer">
    <path d="M10,50 L90,50 …" />
  </g>
</svg>
```

Layer names to tool mapping:

```
Layer name / id       → Tool
──────────────────────────────────
"knife"               KNIFE
"crease"              CREASE
"pen"                 PEN
"revolver_pen"        REVOLVER_PEN
```

For a single-tool SVG with no named groups at all, pass `{ defaultTool: "knife" }` to `bakePlan` and all geometry goes to that one tool.

Unnamed `<g>` elements (no `inkscape:label` and no `id`) are transparent — geometry inside them is attributed to the nearest named ancestor.

#### Revolver pen: nested layer structure

The revolver pen is multi-slot. Each slot is a nested `<g>` inside the `revolver_pen` group. The ingestor builds `/`-separated layer keys from the nesting:

```svg
<svg>
  <g id="revolver_pen">
    <g id="slot0"> <path d="…" /> </g>
    <g id="slot1"> <path d="…" /> </g>
    <g id="slot3"> <path d="…" /> </g>
  </g>
</svg>
```

This produces the layer keys `"revolver_pen/slot0"`, `"revolver_pen/slot1"`, `"revolver_pen/slot3"` — one key per slot group. `bakePlan` resolves each to the `REVOLVER_PEN` tool and records the slot index for the orchestrator to select the correct A-axis position at run time.

The same nesting works with `inkscape:label` on both levels.

#### Pre-normalised input

The input SVG does not have to be authored from scratch. A common workflow is to pre-process the SVG upstream — simplify paths, convert all curves to cubics, remove transforms, output coordinates already in mm with the Y-axis already correct — so that by the time it reaches this library the only remaining work is the layer mapping and the stage 3–8 motion planning chain.

For that workflow use `loadSvgLayers` instead of `loadSvgMmLayers`. `loadSvgLayers` runs stage 1 only (parse to `CubicBezier[][]` in the SVG's own coordinate space) and skips stage 2 entirely. If the upstream pipeline already outputs mm coordinates, the result goes straight into `compileBlock`:

```ts
import { loadSvgLayers, toolForLayer, compileBlock } from "urumi-toolpath";

// SVG coordinates are already in mm, Y already flipped upstream
const layers = loadSvgLayers(svgText);   // Map<layerName, CubicBezier[][]>

for (const [name, subpaths] of layers) {
    const profile = toolForLayer(name, cfg.config.toolProfiles);
    const { segments } = compileBlock(
        subpaths, cfg.config.machine, cfg.config.quality, profile,
    );
}
```

A `<path>` whose `d` attribute contains only absolute cubic `C` commands is read directly with no conversion — the format most SVG processing pipelines produce.

### Supported elements

| Element | How it is processed |
|---|---|
| `<path>` | Parsed directly — supports `M L H V C S Q T A Z` (absolute and relative) |
| `<rect>` | Converted to 4 linear segments (or Bézier arcs if `rx`/`ry` are set) |
| `<circle>` | Converted to 4 cubic Bézier arcs (κ ≈ 0.5523, error < 0.03%) |
| `<ellipse>` | Converted to 4 cubic Bézier arcs |
| `<line>` | Treated as `M` + `L` |
| `<polyline>` | Sequential `L` segments |
| `<polygon>` | Same as polyline with a closing `Z` |

Elements inside `<defs>`, `<clipPath>`, or `<mask>`, or marked `display:none` / `visibility:hidden`, are skipped. Non-paintable elements (fill and stroke both `none`) are also skipped.

### Coordinate system

The SVG Y-down coordinate origin is automatically flipped to machine Y-up during ingest. All geometry is converted to millimetres using the SVG's own `width` / `height` / `viewBox` attributes — no manual scale factor needed. Units (`mm`, `cm`, `in`, `pt`, `pc`, `px`) in the `width`/`height` attributes are all handled; unitless values are treated as `px` at 96 DPI.

---

## Output Format: The MicroSegment Packet

`packMicrosegment(seg, seq)` serializes one motion event to a **26-byte `Uint8Array`**:

```
Offset  Size  Type     Field
──────  ────  ───────  ───────────────────────────────────────────────────
  0       1   uint8    Magic: 0xAB
  1       4   int32    dX — relative X steps (signed LE)
  5       4   int32    dY — relative Y steps (signed LE)
  9       4   int32    dZ — relative Z steps (signed LE)
 13       4   int32    dA — relative A steps (signed LE)
 17       4   uint32   interval — timer ticks between steps (fCpu Hz, LE)
 21       1   uint8    flags
 22       1   uint8    sequence number (wraps at 255)
 23       2   —        reserved, zeroed
 25       1   uint8    CRC-8 (polynomial 0x8C, over bytes 0–24)
```

**Decoding a packet:**

```ts
const view = new DataView(packet.buffer);

const dX       = view.getInt32(1,  true);
const dY       = view.getInt32(5,  true);
const dZ       = view.getInt32(9,  true);
const dA       = view.getInt32(13, true);
const interval = view.getUint32(17, true);
const flags    = view.getUint8(21);
```

**Interval → physical speed:**

```
speed (steps/sec) = fCpu / interval
speed (mm/sec)    = speed (steps/sec) / stepsPerUnit
```

**Step delta → physical distance:**

```
distance (mm) = dX / stepsPerUnit_X
```

**Flag constants** (import from `urumi-toolpath`):

| Constant | Value | Meaning |
|---|---|---|
| `MICRO_PATH_END` | `0x01` | Last segment of a path stroke |
| `MICRO_LIFT` | `0x02` | Z-lift / non-cutting move |
| `MICRO_JOG` | `0x04` | Travel jog between paths |
| `MICRO_PAUSE` | `0x08` | Pause point — operator action required |

---

## Architecture Overview

The pipeline is split into two phases so the expensive geometry work can happen offline and be stored, while the lightweight scheduling runs fresh at execution time against the actual machine state.

```
SVG text ─┐
          ├─► bakePlan ─────────► Plan ──► savePlan ──► .plan bytes
config ───┘      │
                 │  (or keep Plan in memory — same object)
                 ▼
         scheduleMounts ──────────► Schedule   (tool-swap phases)
                 │
                 ▼
          walkSchedule ───────────► WalkEvent[]
                 │                   │
                 │                   ├─ kind: "motion"  → segments → packMicrosegment → wire
                 │                   └─ kind: "pause"   → swapIn/swapOut → operator prompt
                 ▼
         bytes on the RS485 bus
```

### Bake phase (offline, per block)

`compileBlock` (called internally by `bakePlan`) runs the full stage chain for one SVG layer:

1. Apply `−toolOffset` — re-express the path in head-center coordinates
2. Repair C1 continuity at curve joins
3. Flatten Béziers → equally-spaced `Sample[]` (arc-length parameterized, each ≤ `segmentLength` mm)
4. Constrain velocity — per-sample speed ceilings from axis limits
5. Look-ahead feedrate planner — smooth velocity profile across the full block
6. Discretize → `MicroSegment[]` — integer step deltas + clock intervals

Intra-block non-cutting motion is also baked: Z lift/lower, travel jogs between subpaths, A-axis pre-orientation at path starts, and corner pivot sequences (lift → orient → plunge when heading change exceeds `cornerAngle`).

**Compiled blocks assume `aPhys = 0` at entry.** The walk enforces this by A-homing before every block.

### Run phase (per execution)

`scheduleMounts` + `walkSchedule` are a **reference implementation** of the run phase. They handle the common case — greedy tool-swap batching, A-homing, revolver slot selection, inter-block travel, and operator pause events — but you are free to skip them entirely and implement your own scheduling logic directly against the `Plan` and `Block` types. The baked `MicroSegment[]` in each block are self-contained regardless of how you sequence them.

The reference orchestrator generates inter-block motion that depends on real machine position:

1. A-home to 0°
2. Revolver slot rotation to the correct slot angle
3. Travel jog from current `(posX, posY)` to `block.startSteps`
4. Head-offset jog when switching between physical heads

At phase boundaries it emits a **pause event** before generating the inter-block motion, so the caller can prompt the operator to swap tools.

---

## Pipeline Stages

The full pipeline runs stages 1–8. Stages 1–2 are in the SVG ingestor; stages 3–8 run inside `compileBlock` (once per layer). `bakePlan` calls both halves in sequence. If you call `compileBlock` directly you are entering at stage 3 with geometry already in mm.

```
Stage 1  SVG parse          raw SVG text → CubicBezier[][] in SVG pixel coordinates
Stage 2  Normalise          pixel coordinates → mm, Y-axis flip
                            ── compileBlock entry point ──
Stage 3  Repair             C1 continuity enforced at curve joins
Stage 4  Flatten            Bézier subpaths → Sample[]  (arc-length parameterized)
Stage 5  Constrain          per-sample velocity ceilings from axis limits
Stage 6  Plan               look-ahead feedrate planner → smooth velocity profile
Stage 7  Choreograph        non-cutting motion helpers (called during stage 8)
Stage 8  Discretize         PlannedSample[] → MicroSegment[]
```

### Stage 1 — SVG parse

Walks the SVG document and converts every drawable element to a list of cubic Bézier subpaths in SVG pixel coordinates. All shape types are normalised to cubics at this point:

- `<path>` — `M L H V C S Q T Z` commands parsed and chained; relative commands converted to absolute; implicit command repeats handled; each `M` after the first starts a new subpath.
- `<circle>` / `<ellipse>` — approximated with 4 cubic arcs using κ ≈ 0.5523 (error < 0.03%).
- `<rect>` — 4 linear segments, or Bézier arcs at rounded corners.
- `<line>`, `<polyline>`, `<polygon>` — converted to sequential linear segments (each internally a degenerate cubic).

Non-paintable elements (fill and stroke both `none`) and elements inside `<defs>` / `<clipPath>` / `<mask>` are skipped. Layer grouping (`<g>` labels) is recorded here but does not affect the geometry.

### Stage 2 — Normalise

Converts the pixel-space cubics from stage 1 into millimetres and flips the Y-axis so the machine origin is at the bottom-left (SVG has Y increasing downward; the machine has Y increasing upward).

The transform is built from the SVG's own `width`, `height`, and `viewBox` attributes. Units (`mm`, `cm`, `in`, `pt`, `pc`, `px`) in the `width`/`height` values are handled automatically. If physical dimensions are absent, `viewBox` units are treated as mm 1:1.

**Output:** `CubicBezier[][]` in mm, machine coordinate frame. This is what `loadSvgMmLayers` and `loadSvgMmSubpaths` return, and what you pass into `compileBlock`.

### Stage 3 — Repair

Enforces C1 continuity (matching tangent direction) at the join between consecutive curves within a subpath. Sharp kinks at joins can cause the planner to produce very short high-acceleration segments that the machine cannot follow smoothly. The repair stage adjusts the affected control points to eliminate kinks below a configurable threshold before any sampling occurs.

### Stage 4 — Flatten

Converts the cubic Bézier subpaths into a flat sequence of `Sample` points, equally spaced in arc-length. Each sample carries:

- `x`, `y` — position in mm
- `theta` — tangent direction (radians), used by the knife-orient logic
- `kappa` — signed curvature, used for velocity ceilings at curves
- `ds` — arc-length distance from the previous sample
- `flags` — `PATH_START` / `PATH_END` markers that trigger intra-block non-cutting motion in stage 8

The target spacing between samples is `quality.segmentLength` (default 0.5 mm). Curves are sampled by arc-length parameterization (LUT-based binary search) so the spacing is uniform regardless of curvature — tight curves and gentle arcs get the same physical density.

**Output:** `Sample[]`

### Stage 5 — Constrain

Computes a per-sample velocity ceiling based on axis capabilities and path geometry. At high curvature the centripetal acceleration requirement limits how fast the tool can move; at low curvature the axis `maxRate` is the binding constraint. The result is a `vCeiling` attached to each sample — a hard upper bound that the planner in stage 6 must respect.

**Output:** `ConstrainedSample[]` (each `Sample` plus `vCeiling`)

### Stage 6 — Plan (look-ahead feedrate)

Runs a bidirectional pass over the constrained samples to produce a smooth velocity profile for the whole block:

- **Forward pass:** starting from `v_entry`, accelerate up to `vCeiling` without exceeding axis `accel`.
- **Backward pass:** starting from `v_exit`, decelerate so the tool can stop in time for every future constraint.
- The final velocity at each sample is the minimum of the two passes.

The result is a physically achievable velocity profile — the tool is always fast enough to meet throughput but never faster than the machine can decelerate from.

**Output:** `PlannedSample[]` (each `ConstrainedSample` plus `v`, the resolved velocity)

### Stage 7 — Choreograph

Not a sequential step — a library of stateless helpers called *during* stage 8 at path transitions and intra-block events. Each helper returns a `MicroSegment[]` splice that is inserted into the output stream:

- **`zMove`** — lift or plunge the Z axis
- **`aMove`** / **`aMoveTo`** — rotate A axis by a delta or to an absolute angle
- **`preOrient`** — rotate A to the entry tangent before a `PATH_START`
- **`pivot`** — lift → orient → plunge sequence at sharp corners (when heading change > `cornerAngle`)
- **`travelJog`** — XY jog between subpaths within the block
- **`headOffsetJog`** — XY correction when switching between heads with different offsets

These helpers are also used at the inter-block level by `walkSchedule` — the same code that handles intra-block travel handles inter-block travel.

### Stage 8 — Discretize

Converts the `PlannedSample[]` from stage 6 into integer `MicroSegment` step events, interleaved with non-cutting motion from stage 7:

1. At each `PATH_START`: emit `preOrient` (A → entry tangent) then `zMove` (plunge).
2. For each sample: compute `dX`, `dY` (integer step deltas from `ds × stepsPerUnit`), `dA` (knife heading delta), and `interval` (timer ticks = `fCpu / v`). If the heading change exceeds `cornerAngle`, insert a `pivot` sequence before continuing.
3. At each `PATH_END`: emit `zMove` (lift) then `travelJog` to the next subpath's start (if one follows).

**Output:** `MicroSegment[]` — the final baked block, ready to pack with `packMicrosegment`.

---

## API Reference

Everything below is imported from `urumi-toolpath`. Anything not listed here is internal.

### Config

| Export | Description |
|---|---|
| `parseConfig(json)` | `string → { ok, config } \| { ok: false, errors }` — never throws, never silently fills machine values |
| `PipelineConfig` | Top-level config type: `{ machine, quality, toolProfiles }` |
| `MachineConfig`, `QualityConfig` | Axis + head specs; quality tuning |
| `ToolProfile`, `ToolType` | Tool model |
| `PEN`, `KNIFE`, `CREASE`, `REVOLVER_PEN` | Built-in tool presets |
| `TOOL_PROFILES`, `TOOL_PROFILES_BY_TYPE` | Preset lookup tables |
| `toolForLayer(name, profiles)` | Resolve an SVG layer name to a `ToolProfile` |
| `requiredAxes(profile)` | Which bus nodes a given tool needs |
| `canRunTool(profile, machine)` | Whether the machine's wired nodes can run a tool |

### Bake

| Export | Description |
|---|---|
| `bakePlan(config, svgText, opts?)` | **Main entry.** → `{ plan: Plan, bytes: Uint8Array }` |
| `BakePlanOptions` | `{ defaultTool?: string }` |
| `compileBlock(subpathsMm, machine, quality, profile)` | One layer's `CubicBezier[][]` → `{ segments, startSteps }`. Use directly for compile-at-execution workflows. |
| `assembleBlocks(layers, config, opts?)` | Resolve SVG layers → tool-tagged blocks (pre-compile; used inside `bakePlan`) |

### Plan

| Export | Description |
|---|---|
| `Plan`, `Block` | Job model types |
| `savePlan(plan)` | `Plan → Uint8Array` |
| `loadPlan(bytes)` | `Uint8Array → Plan` — throws on bad magic or truncation |
| `PLAN_MAGIC`, `PLAN_VERSION`, `SLOT_NONE` | Binary format constants |
| `planToolTypes(plan)` | Distinct tool types used by a plan |
| `feasibleOn(plan, machine)` | Whether all required bus nodes are present |

### Orchestrate

> **Reference implementation.** `scheduleMounts` and `walkSchedule` cover the common case but are optional — you can drive the `Plan` and `Block` types directly with your own scheduling logic.

| Export | Description |
|---|---|
| `scheduleMounts(plan, headCount, seedMounted?)` | → `Schedule` (greedy tool-swap phase batching) |
| `walkSchedule(schedule, plan, machine, opts?)` | → `WalkEvent[]` (ordered motion + pause events, with inter-block travel filled in) |
| `Schedule`, `Phase`, `WalkEvent` | Orchestrator types |

### Wire

| Export | Description |
|---|---|
| `MicroSegment` | `{ dx, dy, dz, da, interval, flags }` |
| `packMicrosegment(seg, seq?)` | → 26-byte `Uint8Array` |
| `writeStream(packets)` | → length-framed byte stream: `[u16 LE len][26-byte packet]` × n |
| `decodePacket(bytes)` | 26-byte packet → `MicroSegment` |
| `crc8(buf, start, end)` | CRC-8, polynomial 0x8C |
| `MICRO_PATH_END`, `MICRO_LIFT`, `MICRO_JOG`, `MICRO_PAUSE` | Flag constants |
| `PACKET_SIZE` | `26` |

### SVG ingest

| Export | Description |
|---|---|
| `loadSvgMmLayers(svgText)` | → `{ layers: Map<string, CubicBezier[][]> }` in mm — multi-tool input |
| `loadSvgMmSubpaths(svgText)` | → `{ subpaths: CubicBezier[][], viewport }` in mm — single-tool input |
| `loadSvgMm(svgText)` | Combined: both layers and flat subpaths |
| `setDOMParser(factory \| null)` | Inject a `DOMParser` factory for Node; `null` restores default |
| `DOMParserLike` | Structural type: `{ parseFromString(source, mimeType) }` |
| `Pt`, `CubicBezier` | Geometry primitives: `{ x, y }` and `[Pt, Pt, Pt, Pt]` |

---

## See also

- **`demo/main.js`** — bake demo: load config + SVG, click Bake, download `.plan`
- **`demo/orchestrate.js`** — run demo: load `.plan`, schedule, walk, stream over WebSerial
- **`demo/transport.js`** — WebSerial transport (Go-Back-N, window=16) — copy this for your own transport layer
- **`USAGE.md`** — additional notes on the `compileBlock` compile-at-execution path
- **`README.md`** — internal architecture, pipeline stages, parity testing
