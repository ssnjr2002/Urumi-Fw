# Using this package

A library that turns an **SVG drawing + a machine config** into **wire packets** an
RP2350 CNC/plotter controller can stream. Everything you need is exported from the
package root (`src/index.ts` → `dist/index.js`). TypeScript, ESM, zero runtime
dependencies.

If you want to understand the internals, read [README.md](README.md). This file is
for *using* the package from the outside.

---

## Install

```sh
npm install urumi-toolpath
```

Node 18+ or any modern browser. ESM only (`import`, not `require`).

---

## The mental model

```
config.json ─┐
             ├─► bakePlan ─► Plan ──► savePlan ─► .plan bytes   (bake once, offline)
SVG text ────┘                 │
                               ▼
                       scheduleMounts ─► Schedule
                               │
                               ▼
                        walkSchedule ─► WalkEvent[]             (drive at run time)
                               │
                        motion events carry MicroSegment[]
                               ▼
                        packMicrosegment / writeStream ─► bytes on the wire
```

Two things happen at different times:

- **Bake (offline):** `bakePlan` compiles an SVG into a `Plan` — one `Block` per SVG
  layer, each a list of `MicroSegment` step events. Serialize it to a `.plan` file
  with `savePlan`, or keep the `Plan` in memory.
- **Run (per execution):** `scheduleMounts` batches the plan into tool-swap phases;
  `walkSchedule` emits an ordered list of `WalkEvent`s (motion + operator-pause
  events) with all inter-block travel filled in. You stream the motion segments and
  prompt the operator at the pauses.

Most callers only need the bake half. The run half matters when you're actually
driving the machine and doing tool swaps.

**Don't want a `.plan` file at all?** You can skip baking and compile straight to wire
events with `compileBlock` (optionally assembling an in-memory `Plan` for the
orchestrator) — see [Compile without baking](#compile-without-baking-compile-at-execution).

---

## Quick start — bake a plan

### Browser

```ts
import { parseConfig, bakePlan } from "urumi-toolpath";

const cfg = parseConfig(await (await fetch("/config.json")).text());
if (!cfg.ok) throw new Error(cfg.errors.join("\n"));

const svgText = await (await fetch("/drawing.svg")).text();
const { plan, bytes } = bakePlan(cfg.config, svgText, { defaultTool: "knife" });

// `plan` is the in-memory job model; `bytes` is the .plan file (Uint8Array).
console.log(`${plan.blocks.length} blocks, ${bytes.length} bytes`);
for (const b of plan.blocks) {
    console.log(b.profile.name, b.segments.length, "segments");
}
```

### Node

SVG parsing uses the global `DOMParser`, which exists in browsers but not in Node.
Inject one **once at startup** before calling `bakePlan`:

```ts
import { readFileSync } from "node:fs";
import { DOMParser } from "@xmldom/xmldom";           // your dependency, not ours
import { parseConfig, bakePlan, setDOMParser } from "urumi-toolpath";

setDOMParser(() => new DOMParser());                  // do this before bakePlan

const cfg = parseConfig(readFileSync("config.json", "utf-8"));
if (!cfg.ok) throw new Error(cfg.errors.join("\n"));

const { bytes } = bakePlan(cfg.config, readFileSync("drawing.svg", "utf-8"));
writeFileSync("drawing.plan", bytes);
```

Any object with `parseFromString(source, mimeType)` satisfies `setDOMParser`
(the `DOMParserLike` type). Pass `null` to restore the global-`DOMParser` default.

---

## Read a plan back

`.plan` bytes round-trip losslessly:

```ts
import { loadPlan, savePlan } from "urumi-toolpath";

const plan  = loadPlan(bytes);          // Uint8Array → Plan
const bytes2 = savePlan(plan);          // Plan → Uint8Array  (=== bytes)
```

---

## Compile without baking (compile-at-execution)

`bakePlan` is the batteries-included path: SVG → `Plan` → `.plan` bytes. If your
workflow never wants a `.plan` artifact — you compile in memory right before
streaming, or you want per-layer control — use **`compileBlock`** directly. It's the
per-layer compile that `bakePlan` runs internally:

```ts
compileBlock(subpathsMm, machine, quality, profile) => { segments, startSteps }
```

- `subpathsMm` — one layer's geometry as `CubicBezier[][]` in mm (from the SVG
  ingest helpers below).
- `machine`, `quality` — from your parsed config (`cfg.config.machine`,
  `cfg.config.quality`).
- `profile` — the `ToolProfile` that cuts this layer (a preset like `KNIFE`, or
  `toolForLayer(name, cfg.config.toolProfiles)`).
- returns `segments: MicroSegment[]` (ready to pack) and `startSteps` (the block's
  XY start in true machine steps).

### Level A — single tool, straight to wire (no Plan, no file)

```ts
import {
    loadSvgMmSubpaths, compileBlock, KNIFE,
    packMicrosegment, writeStream,
} from "urumi-toolpath";

const { subpaths } = loadSvgMmSubpaths(svgText);   // SVG → CubicBezier[][] in mm
const { segments } = compileBlock(
    subpaths, cfg.config.machine, cfg.config.quality, KNIFE,
);
const bytes = writeStream(segments.map((s, i) => packMicrosegment(s, i & 0xff)));
// stream `bytes` immediately — nothing was baked or serialized
```

### Level B — multi-layer, in-memory Plan, still no `.plan` file

Build a `Plan` in memory from `compileBlock` results and feed it to the orchestrator.
You skip `savePlan`/`loadPlan` entirely but still get inter-block travel and tool-swap
scheduling for free:

```ts
import {
    loadSvgMmLayers, toolForLayer, compileBlock,
    scheduleMounts, walkSchedule, type Plan, type Block,
} from "urumi-toolpath";

const { layers } = loadSvgMmLayers(svgText);       // Map<layerName, CubicBezier[][]>
const blocks: Block[] = [];
for (const [name, subpaths] of layers) {
    const profile = toolForLayer(name, cfg.config.toolProfiles);
    if (!profile) throw new Error(`layer '${name}' names no tool`);
    const { segments, startSteps } = compileBlock(
        subpaths, cfg.config.machine, cfg.config.quality, profile,
    );
    blocks.push({ profile, segments, startSteps });
}
const plan: Plan = { blocks };                     // in memory only — never saved

const schedule = scheduleMounts(plan, cfg.config.machine.heads.length);
for (const ev of walkSchedule(schedule, plan, cfg.config.machine)) {
    if (ev.kind === "motion") stream(ev.segments);
    else await promptOperator(ev.swapIn, ev.swapOut);
}
```

> **Inter-block motion is your responsibility at Level A.** A `compileBlock` result is
> self-contained for *one* block, but it assumes A is homed (`aPhys = 0`) at entry and
> contains **no** inter-block travel or A-home moves. Streaming several blocks' segments
> back-to-back yourself (Level A repeated) means you must insert that motion. Routing the
> blocks through `walkSchedule` (Level B) generates A-home, revolver rotation, and travel
> jogs for you — which is exactly what `bakePlan` + the orchestrator give you, minus the
> file.
>
> **Revolver slots:** the manual loop above handles flat single-tool layers. Revolver
> sub-layer keys (`"revolver_pen/slot3"`) need slot parsing that lives in `bakePlan`'s
> block assembler — use `bakePlan` if your SVG drives a revolver pen.

---

## Schedule + walk (driving the machine)

```ts
import { scheduleMounts, walkSchedule } from "urumi-toolpath";

const headCount = cfg.config.machine.heads.length;
const schedule  = scheduleMounts(plan, headCount);
const events    = walkSchedule(schedule, plan, cfg.config.machine);

for (const ev of events) {
    if (ev.kind === "motion") {
        stream(ev.segments);            // MicroSegment[] — send to the controller
    } else {
        await promptOperator(ev.swapIn, ev.swapOut);   // tool-swap pause
    }
}
```

`walkSchedule` handles A-homing, revolver slot rotation, and inter-block travel
jogs for you — the `segments` you get are ready to serialize.

### Turning segments into wire bytes

```ts
import { packMicrosegment, writeStream } from "urumi-toolpath";

const packets = segments.map((seg, i) => packMicrosegment(seg, i & 0xff));
const framed  = writeStream(packets);   // [u16 LE len][26-byte packet] per segment
```

> **Transport is not included.** This package produces bytes; it does not open a
> serial port. The reference WebSerial driver lives in the repo's `demo/transport.js`
> and is intentionally out of the package (it's browser-only and depends on
> `navigator.serial`). Copy it or write your own around `packMicrosegment`.

---

## API surface

Everything below is imported from `urumi-toolpath`. This is the whole
supported surface — anything not listed here is internal.

### Config

| Export | Purpose |
|---|---|
| `parseConfig(json)` | `config.json` string → `{ ok, config } \| { ok: false, errors }` |
| `PipelineConfig`, `MachineConfig`, `QualityConfig` | config types |
| `ToolProfile`, `ToolType`, `toolProfile()` | tool model |
| `PEN`, `KNIFE`, `CREASE`, `REVOLVER_PEN` | built-in tool presets |
| `TOOL_PROFILES`, `TOOL_PROFILES_BY_TYPE` | preset lookup tables |
| `toolForLayer`, `requiredAxes`, `canRunTool` | policy helpers |

### Bake

| Export | Purpose |
|---|---|
| `bakePlan(config, svgText, opts?)` | **main entry** → `{ plan, bytes }` |
| `BakePlanOptions` | `{ defaultTool?: string }` — tool for an unlayered SVG |
| `compileBlock(subpathsMm, machine, quality, profile)` | one layer's mm subpaths → `{ segments, startSteps }`. The bake-free entry — see [Compile without baking](#compile-without-baking-compile-at-execution) |
| `assembleBlocks(layers, config, opts?)` | resolve SVG layers → tool-tagged blocks (pre-compile; used inside `bakePlan`) |

### Plan

| Export | Purpose |
|---|---|
| `Plan`, `Block` | job model types |
| `savePlan(plan)` / `loadPlan(bytes)` | `.plan` codec (round-trips) |
| `planToolTypes`, `feasibleOn` | plan inspection |

### Orchestrate

| Export | Purpose |
|---|---|
| `scheduleMounts(plan, headCount, seedMounted?)` | → `Schedule` (swap phases) |
| `walkSchedule(schedule, plan, machine, opts?)` | → `WalkEvent[]` |
| `WalkEvent`, `Schedule`, `Phase` | types |

### Wire

| Export | Purpose |
|---|---|
| `MicroSegment` | one step event: `{ dx, dy, dz, da, interval, flags }` |
| `packMicrosegment(seg, seq?)` | segment → 26-byte packet |
| `writeStream(packets)` | packets → length-framed byte stream |
| `decodePacket`, `crc8` | decode / checksum |
| `MICRO_PATH_END`, `MICRO_LIFT`, `MICRO_JOG`, `MICRO_PAUSE` | flag constants |

### SVG (optional)

| Export | Purpose |
|---|---|
| `loadSvgMmLayers` | SVG → `Map<layerName, CubicBezier[][]>` in mm (multi-tool compile input) |
| `loadSvgMmSubpaths` | SVG → `{ subpaths, viewport }` in mm (single-tool compile input) |
| `loadSvgMm`, `loadSvgLayers`, … | other ingest variants (see `src/svg/ingest.ts`) |
| `setDOMParser(factory)` | inject a `DOMParser` in Node; `DOMParserLike` type |

---

## The config object

`bakePlan` and `walkSchedule` take a `PipelineConfig` (or its `.machine`), which you
almost always get from `parseConfig(configJson)`. The JSON must supply machine
calibration — at minimum `machine.fCpu`, `machine.x`, `machine.y`, and a non-empty
`heads[]`, each axis carrying its `node.nodeId` and `stepsPerUnit`, each head naming
its `tool`. See the authoritative schema in `src/config/configLoader.ts` and a real
working example in `demo/config.json`.

`parseConfig` never throws and never silently fills in machine values — on a bad
config it returns `{ ok: false, errors }` listing **every** problem, so you can show
the operator all of them at once.

---

## Errors

- `parseConfig` → `{ ok: false, errors }` (no throw).
- `bakePlan` **throws** on unparseable SVG or a layer naming a tool the bus can't
  run (missing axis node). Wrap it in `try/catch` and surface `e.message`.
- `loadPlan` throws on a truncated or wrong-magic `.plan` file.

---

## A complete browser example

See `demo/main.js` (bake + download) and `demo/orchestrate.js` (load `.plan`,
schedule, walk, stream over WebSerial) in the repository — both import this package
through its public barrel and are the canonical worked examples.
