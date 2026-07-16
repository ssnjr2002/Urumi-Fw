# Feed & Accel Value Model

**Status:** implemented (library + demo configs) · **Last updated:** 2026-07-16

A single, principled model for every velocity and acceleration value in the
toolpath pipeline. Replaces the ad-hoc mix of `feedMax` / `maxRate` / `jogFeed`
/ `zFeed` scattered across the machine, axis, and tool tiers, where ceiling and
target were conflated, tiers overlapped, and some fields were dead
(`tool.accel`, `z.accel`).

---

## Core concepts

Two orthogonal ideas, each with one natural home:

- **Ceiling** — a *physical* limit of the hardware (motor + driver + mechanics):
  "never exceed this or the axis stalls / loses steps / breaks." Intrinsic to an
  **axis**; independent of tool or operation. Lives at the **axis tier**,
  per-axis, and is never raised by a tool or machine setting.

- **Target** — a *desired operating value* for a specific operation ("cut at
  40 mm/s"). Varies by operation and tool. A scalar in the operation's own
  motion space, not per-axis. Lives at the **tool / machine tiers**.

### Organizing principle: engage vs. reposition

Which tier owns a target follows from whether the tool is touching the work:

- **Engage** (tool touches the work) → **tool-owned** (with a machine default):
  `path` (cut), `z` (tool touch-down / retract).
- **Reposition** (pen-up, nothing touching) → **machine-owned**, tool-agnostic:
  `rapid` (XY travel), `slew` (standalone A).

Naming convention: **`max*` always means a ceiling** (`maxFeed`, `maxAccel`);
plain `*Feed` / `*Accel` always means a target. This breaks the old
`feedMax`-vs-`maxRate` collision where both said "max" but meant opposite things.

---

## Ceilings — axis tier, physical, per-axis

| Axis | `maxFeed` | `maxAccel` | Role |
|---|:---:|:---:|---|
| X | ✅ | ✅ | physical cap |
| Y | ✅ | ✅ | physical cap |
| Z | ✅ | ✅ | physical cap |
| A | ✅ | ✅ | physical cap — bounds **both** tracking and slew |

## Targets — per operation, `{feed, accel}`

| Operation | Space (axes) | Feed | Accel | Owner | Applies when |
|---|---|---|---|---|---|
| `path` (cut) | XY, +A *tracked* | `pathFeed` | `pathAccel` | **tool** (machine default) | pen-down cutting |
| `z` (engage) | Z | `zFeed` | `zAccel` | **tool** (machine default) | tool touch-down / retract |
| `rapid` (travel) | XY | `rapidFeed` | `rapidAccel` | **machine only** | any pen-up XY — boundaries **and** in-block after Z lifts |
| `slew` (A) | A | `slewFeed` | `slewAccel` | **machine only** | standalone A — orient, pivot, slot-select, A-home |

## Tier ownership at a glance

| Tier | Owns | Count |
|---|---|---|
| **axis** (X/Y/Z/A) | ceilings: `{maxFeed, maxAccel}` each | 8 |
| **machine** | full baseline: `path`, `z`, `rapid`, `slew` × `{feed, accel}` | 8 |
| **tool** | overrides: `path`, `z` × `{feed, accel}` (optional, sparse) | ≤4 |

The machine tier is the **complete default baseline** (all 8 target values). The
tool tier is a **sparse override layer** on top of it, and may only override the
two *engage* operations. `rapid` and `slew` have no tool override — repositioning
is the machine's job.

### Machine tier detail

| Operation | Feed | Accel | Tool can override? |
|---|---|---|---|
| `path` | `pathFeed` | `pathAccel` | ✅ yes |
| `z` | `zFeed` | `zAccel` | ✅ yes |
| `rapid` | `rapidFeed` | `rapidAccel` | ❌ no |
| `slew` | `slewFeed` | `slewAccel` | ❌ no |

---

## Resolution rule

One rule, everywhere:

```
commanded = tool value if set, else machine value      (the target)
            then clamp to maxFeed / maxAccel of every participating axis
```

- **A during `path`** is not a target — it is *derived* from the XY path
  (`ω = pathFeed × curvature`) and clamped by A's `maxFeed` / `maxAccel`. This is
  what forces XY to slow on tight curves so A can keep up.
- **XY `path` accel** is a single commanded target (`pathAccel`); it is split
  across X and Y and clamped by each axis's `maxAccel`. There is no stored scalar
  XY accel — the per-direction limit is derived from the per-axis ceilings.
- **`path` never falls back to `rapid`.** They are distinct operations that share
  path-space; a tool with no `pathFeed` inherits the machine `path` *default*,
  not `rapidFeed`.

---

## Notes & deferred work

- **`z` is currently a default-only baseline.** `rapid` and `slew` have genuine
  tool-agnostic uses (boundary XY jogs, A-home between swaps run pen-up with no
  tool engaged), so the machine value is used directly. Nothing moves Z outside a
  tool's block today, so the machine-tier `zFeed`/`zAccel` is only a fallback
  template for now.

- **Future split: `z` → `plunge` (down) + `retract` (up).** The advantage is a
  gentle touch-down (protects the blade tip / pen nib, avoids stab-marks) with a
  fast retract (saves cycle time). This fits the engage/reposition principle
  exactly: `plunge` is engage (tool-owned), `retract` is reposition
  (machine-owned). Deferred until Z acceleration ramping lands — splitting a
  motion the Z path can't yet ramp models a distinction that has no effect.

- **Implement `z.accel` / `zAccel` ramping.** Z moves are currently
  single-segment constant-velocity (see the TODO in `choreograph.ts`). Making Z
  ramp trapezoidally (like A) is the precondition for the `plunge`/`retract`
  split above.

---

## Validation (scoped to feed & accel)

Config validation as a whole — duplicate `nodeId`, `defaultHead` range, node-type
checks, `toolOffset` tolerance — is a **separate effort, out of scope here.** A
`validateConfig()` pass is introduced, but for this work it enforces only the
feed/accel fields:

- **Invalid (error):** any feed or accel < 0 (ceilings and targets alike).
- **Warning:** an axis `maxFeed`/`maxAccel` of 0 on X or Y (the always-driven
  axes) — "uncapped," almost always a mistake; a target that exceeds its
  participating axis ceiling (harmless — it is clamped — but flags a likely
  misunderstanding).
- **Optional (absent → default):** every target. Accel/slew targets absent →
  axis ceiling (the parity lever); feed targets absent → the documented machine
  default.

No new *mandatory* feed/accel field: the mandatory presence checks (`fCpu`, axis
`stepsPerUnit`, a head with tool+z+a) are structural and already in the loader.

## Library impact (`web/src`)

Only 3 of the 7 `toolpath/` files touch feed/accel. `flatten`, `repair`,
`sample`, `geometry` consume only quality/geometry params — **no changes**.

Each change below is tagged **rename** (parity-neutral resource/rename),
**behavioral** (new capability; parity-neutral only via the defaulting lever
below), or **deferred** (kept in schema, inert until implemented).

| Module | Change | Kind |
|---|---|---|
| `toolpath/constrain.ts` | `feedMax` → `pathFeed` | rename |
| | `aMax` re-sourced to `min(x.maxAccel, y.maxAccel)` (lateral-accel ceiling) | rename |
| | `aRateDegS?` ← `a.maxFeed`; `aAccelDegS2?` ← `a.maxAccel` | rename |
| | `cornerStopAngleDeg?` (tool geometry) | unchanged |
| `toolpath/plan.ts` | add `pathAccel` target; `segAccel` returns `min(per-axis maxAccel combo, pathAccel)` | behavioral |
| | `xAccel`/`yAccel` → per-axis `maxAccel` ceilings; `aAccelDegS2` ← `a.maxAccel`; `aMax` fallback → `pathAccel` | rename |
| `toolpath/discretize.ts` | jog resolution → `machine.rapidFeed` only (drop `tool.jogFeed` fallback) | rename |
| | standalone A cruises at `slewFeed`/`slewAccel` target, not the axis ceiling | behavioral |
| | `zFeed` resolution kept (`tool.zFeed ?? machine.zFeed`); `zAccel` ramping | deferred |
| | `liftHeight` (tool geometry) | unchanged |
| `choreograph/choreograph.ts` | `aMove` takes `slewFeed`/`slewAccel` instead of reading `a.maxRate`; callers (`preOrient`, `pivot`, `aMoveTo`) thread them | behavioral |
| | `travelJog`/`headOffsetJog` take `rapidFeed`; `rapidAccel` ramping | deferred |
| | `zMove` keeps `zFeed`; trapezoidal `zAccel` ramp | deferred |
| `wire/microsegment.ts` | `interval()` reads `axes.*.maxFeed` (was `maxRate`) | rename |
| `production/compileBlock.ts` | rewrite the tier-resolution map (below) | rename + behavioral |
| `orchestrate/walk.ts` | `machine.jogFeed` → `machine.rapidFeed`; `aMoveTo` threads the slew target | rename |

`compileBlock.ts` sourcing after the change:

```
pathFeed    = tool.path?.feed  ?? machine.path.feed
pathAccel   = tool.path?.accel ?? machine.path.accel ?? min(x.maxAccel, y.maxAccel)
latAccelCap = min(x.maxAccel, y.maxAccel)                (constrain centripetal cap)
A ceilings  = a.maxFeed, a.maxAccel                      (tangential-gated as today)
rapid       = machine.rapid   (feed/accel)               → discretize
slew        = machine.slew    (feed/accel)               → discretize / choreograph
z           = tool.z?.feed ?? machine.z.feed  (+ z.accel deferred)
```

### Byte changes & the golden snapshot

The Python parity harness is retired (replaced by a self-golden snapshot baked
through the real `compileBlock`; see `test/production/snapshot.test.ts`). So a
byte change is no longer forbidden — an intentional change is accepted by
regenerating the golden (`UPDATE_GOLDEN=1`) and reviewing the diff.

Defaulting the new accel/slew targets to their axis ceiling is therefore **no
longer a parity workaround** — it is just the honest semantic default ("no cap
below the axis limit unless you set one"). It keeps the *initial* migration
byte-neutral (the golden won't move on the rename alone), which is a convenient
sanity check, but it's no longer load-bearing.

---

## Schema

**Decision: flat axis ceilings, nested operation targets.** Axis ceilings are
two scalars that belong on the axis; operation targets are genuine
`{feed, accel}` pairs, and nesting makes "accel omitted → ceiling" read as simply
leaving it out.

### Axis (flat ceilings)

```jsonc
"x": {
  "node": { "id": 1 },        // nodeId → id (rides along, see node_type_architecture §2)
  "stepsPerUnit": 160,
  "maxFeed": 80,              // ceiling  (was maxRate)
  "maxAccel": 1000,           // ceiling  (was accel)
  "invert": true, "maxTravel": 0, "rotary": false
}
```

### Machine (nested targets — the full baseline)

```jsonc
"machine": {
  "fCpu": 150000000,
  "path":  { "feed": 80 },    // accel omitted → ceiling
  "rapid": { "feed": 80 },
  "z":     { "feed": 20 },
  "slew":  { },               // feed & accel omitted → A ceiling
  "x": { … }, "y": { … }
}
```

### Tool (nested overrides — engage ops only)

```jsonc
"tools": {
  "knife": { "path": { "feed": 40 }, "z": { "feed": 20 }, "liftHeight": 2.0 }
}
```
`rapid` and `slew` are **not** accepted under `tools.*` (machine-owned). Presets
carry only tool-intrinsic values (`path.feed`, `z.feed`, `liftHeight`); accels
left unset → ceiling.

### TS shape

```ts
interface AxisConfig { …; maxFeed: number; maxAccel: number; }          // flat
interface OpTarget   { feed?: number; accel?: number; }                 // nested, both optional
interface MachineConfig { …; path: OpTarget; rapid: OpTarget; z: OpTarget; slew: OpTarget; }
interface ToolProfile   { …; path?: OpTarget; z?: OpTarget; liftHeight: number; }
```

### Resolution

```
target = tool.<op>.feed  ?? machine.<op>.feed  ?? <op default>          (rapid/slew skip tool)
accel  = tool.<op>.accel ?? machine.<op>.accel ?? min(participating axis maxAccel)
         then clamp to every participating axis's maxFeed / maxAccel
```
Machine defaults: `path.feed` 80, `rapid.feed` 80, `z.feed` 20; every accel and
all of `slew` default to *unset → ceiling*. A during `path` stays derived
(`ω = feed × curvature`), clamped by A's ceiling.

---

## Migration

Field-by-field:

| Today | New |
|---|---|
| `axis.maxRate` | `axis.maxFeed` |
| `axis.accel` | `axis.maxAccel` |
| `node.nodeId` | `node.id` |
| `tool.feedMax` | `tool.path.feed` |
| `machine.jogFeed` / `tool.jogFeed` | `machine.rapid.feed` (tool jog override dropped) |
| `machine.zFeed` / `tool.zFeed` | `machine.z.feed` / `tool.z.feed` |
| `tool.accel` | **deleted** (was dead) |
| `z.accel` | **deleted** until ramping lands |
| A slew from `a.maxRate` | `machine.slew.feed` (defaults to A ceiling) |

**Lenient migration — no deprecation gate.** The loader does *not* reject old
names; unknown keys are simply ignored (today's behavior). We control the only
two config files (`web/demo/config.json`, `web/demo/bench.json`) and update them
in the same change, so a stale-config-runs-on-defaults hazard isn't worth the
validation cost. (This is deliberately *out* of the scoped `validateConfig` pass
above.)

---

## Implementation tasklist

Checked off as each lands. Order matters: types first, then loader/validation,
then the toolpath consumers, then configs + golden.

- [x] **config.ts** — `AxisConfig`: `maxRate`→`maxFeed`, `accel`→`maxAccel`.
      Add `OpTarget {feed?; accel?}`. `MachineConfig`: drop `jogFeed`/`zFeed`,
      add `path`/`rapid`/`z`/`slew: OpTarget`. `ToolProfile`: drop
      `feedMax`/`accel`/`zFeed`/`jogFeed`, add `path?`/`z?: OpTarget`.
      `nodeId`→`id` on `BusNode`. Update presets + `uniformMachine` +
      `defaultMachine`.
- [x] **configLoader.ts** — nested target parsing (flat axis ceilings, nested
      machine/tool targets), `node.id`, lenient (ignore old keys).
- [x] **validateConfig.ts** — scoped feed/accel pass (negativity error;
      uncapped-XY + over-ceiling warnings). Exported from `index.ts`.
- [x] **toolpath/constrain.ts** — A ceilings from `maxFeed`/`maxAccel`; `aMax`
      ← `min(x.maxAccel, y.maxAccel)`. (Internal option key `feedMax` kept; it
      now receives the resolved `pathFeed` from compileBlock.)
- [x] **toolpath/plan.ts** — per-axis `maxAccel`; add optional `pathAccel`
      cap (0/unset = no clip, byte-neutral).
- [x] **toolpath/discretize.ts** — jog ← `machine.rapid`; slew ←
      `machine.slew`; `z` ← `tool.z ?? machine.z`.
- [x] **choreograph/choreograph.ts** — `aMove`/`pivot`/`preOrient`/`aMoveTo`
      take an optional slew `OpTarget` (unset → A ceiling).
- [x] **wire/microsegment.ts** — `interval()` reads `maxFeed`.
- [x] **production/compileBlock.ts** — tier-resolution rewritten.
- [x] **orchestrate/walk.ts** — `jogFeed`→`machine.rapid.feed`; slew threaded
      into every `aMoveTo`.
- [x] **web/demo/config.json + bench.json** — migrated to new schema.
- [x] **golden** — verified byte-identical (defaults resolve unchanged); no
      regen needed, full suite green (323 tests).

**Note surfaced by validation:** in both demo configs the Z axis ceiling is
`maxFeed: 10` while the `z.feed` engage target is `20` → clamped to 10.
Pre-existing (old `z.maxRate 10` vs `machine.zFeed 20`), now visible. Left as-is
— a calibration decision (raise the Z ceiling or lower the engage feed), not a
mechanical fix to make here.
