# Coordinate Frames and Soft Limits

**Branch:** `cpp-port`
**Date:** 2026-08-17
**Status:** Frames built (§1–3). **Soft limits (§4–6) are PENDING — not built.**
Nothing enforces a travel limit today; `maxTravel` is still read by nothing.

Dual-head jogging needs the operator to pick which tip is "centre". That needs
a frame model, and the frame model is also what soft limits are built on.

Cross-links:
[../web/src/machine/schema.ts](../web/src/machine/schema.ts) (`ReferencePoint`,
`ToolHead`, `maxTravel`),
[../web/src/production/compileBlock.ts](../web/src/production/compileBlock.ts)
(bake-time tool-offset shift — see §3.2),
[../web/src/choreograph/choreograph.ts](../web/src/choreograph/choreograph.ts)
(`headOffsetJog`).

---

## 0. Tasklist

- [x] **Stage 1** — `machine/frames.ts` (§1–3). Anchor, head/tool offsets,
      home↔tool, and wire↔home. Pure; no `Link`, no clock.
- [x] **Stage 4** — demo: tip-frame selector + home/tip readout (§2). Verified
      against the sim: head 1 at +60 reads home 10.000 / tip 70.000, and a
      3.5mm tool offset moves the tip reading and nothing else.
- [ ] **Stage 2** — `machine/limits.ts` (§4–5). **PENDING.** Inert until
      `enforce` is set, so landing it changes no behaviour.
- [ ] **Stage 3** — config-load report: per-head reach + shared work area
      (§5.3). **PENDING.** Offline; useful with no machine attached.
- [ ] **Later** — per-blend jog enforcement (§7); planner workpiece→machine
      transform (§3.3).

Stage 4 ran ahead of 2–3 deliberately: the frame model is what soft limits are
built on, so proving it against a live machine first means the envelope work
starts from transforms that are known to be right.

**Soft limits do not exist yet.** §4–6 describe what to build, not what runs.
Until Stage 2 lands, nothing refuses a move for being out of travel — on any
axis, in any frame, homed or not.

## 1. Three frames

```
wire   per-slot int32 step counters. Invert applied. Firmware-owned.

home   mm. The firmware's position, ÷ stepsPerUnit. Datum from homing;
       `setorigin` overrides it. THE machine frame — the only one the
       firmware knows, and the only one the envelope lives in.

tool   home + head offset + tool offset. Host-side only. Per selected
       head/tool. This is the "tip" position.
```

`setorigin` acts on **home only**. There is no work frame: the operator's datum
*is* the home datum.

Signs: the anchor in home frame is `[0, maxTravel]`, non-negative. Tip
coordinates may be negative — a head offset can be negative.

## 2. X and Y are one DOF; the anchor

X and Y have one degree of freedom each. Gantry, anchor and every tip are one
rigid body differing by fixed offsets — one position per axis, plus offsets
that reinterpret it. There is no gantry entity.

The **anchor** is whichever entity config places at `(0,0)`: the laser if
fitted, otherwise a head. Its offset is zero by construction, so its position
*is* the machine position. Call it the anchor — with a laser fitted, no head
holds the origin.

The fold is X/Y only:

| | DOFs | model |
|---|---|---|
| X, Y | 1 each, shared | one position + fixed per-head offsets |
| Z, A | 1 per head | per-head; selecting the head selects the axis |

Z and A are separate motors with their own `AxisConfig` — no offset arithmetic.

**UI reports home frame and tip frame side by side.** Two frames are live;
showing one number is how operators get confused.

## 3. Offsets

### 3.1 The two terms

- **Tool offset** — `ToolProfile.toolOffset`, tip vs head centre. Fixed by the
  *tool*. A block names its tool, so this is known at bake time.
- **Head offset** — `ToolHead.xOffset/yOffset`, head centre vs anchor. Fixed
  geometry, but *which* head applies depends on where the tool is mounted —
  the orchestrator's mount table, known only at run time.

That asymmetry is the whole reason the two are handled differently: one can be
baked, the other cannot.

Selecting a centre for jogging = choosing which head offset to view through.
A view transform over immutable config; no state is written.

### 3.2 Where each offset is applied

`compileBlock.ts` shifts path geometry by `-toolOffset` at bake time — it can,
because the block names its tool. Baked coordinates are therefore in
**head-centre** space, and head offset is applied later at emit, once the mount
table says which head carries that tool.

| consumer | tool offset | head offset |
|---|---|---|
| planner / plan pre-flight | already in the geometry — work in head-centre | at emit, from the mount table |
| jog / UI tip readout | look up the mounted tool, apply | selected head, apply |

Jog applies both live: with no block to name the tool, the mount table answers
"what is on this head" as well as "which head".

Pass the frame explicitly — `frame: "head-centre" | "tip"`, no default — so
every call site states which one it is working in. The two differ by exactly one
tool offset, a few millimetres, and agree everywhere else.

### 3.3 Planner note

`headOffsetJog` shifts the gantry so the new head's centre lands where the old
head's centre was — i.e. plan geometry is implicitly in the *active tool frame*.
Invisible today because the planner is relative-only.

When the planner needs absolute position, make it explicit: plan geometry in the
workpiece frame, orchestrator maps workpiece→machine at emit time applying head
offset there. `headOffsetJog` then falls out of the transform. Recorded in that
function's doc comment.

## 4. Soft limits — PENDING

Not built. This section is the specification for Stage 2.

The envelope lives in **home frame** — machine-fixed, from the switches.

`enforce` is a per-axis toggle (§6). Build the check, ship it off, turn it on
per axis when that axis is homed and trusted.

`checkTarget` reports three outcomes: `outside` (past the envelope), `unhomed`
(no valid datum — `axes_homed` bit clear), `disarmed` (`enforce: false`).
Rotary axes are exempt; `AxisConfig.rotary` already marks them.

## 5. Reach and work area

### 5.1 Per head

The anchor travels `[0, maxTravel]`. Tip *h* sweeps `[o_h, maxTravel + o_h]` —
same size, shifted.

### 5.2 Shared

The region every head in a job can reach:

```
common = [ max(o_h), maxTravel + min(o_h) ]
width  = maxTravel − head separation
```

**A two-head job loses the head separation from usable width.** Head 1 at +60
with 900mm travel gives 840mm. If separation exceeds `maxTravel` there is no
common area and a two-head job cannot run — validation says so.

So the API takes a head *set*. One head degenerates to §5.1 for free; a plan
passes the heads its blocks use.

### 5.3 All of §5 is offline

Pure config arithmetic, available at load with no machine attached. Only the
comparison against live position needs a homed machine. Ship the load report
ahead of enforcement:

```
anchor         head 0 "knife" (no laser fitted)
travel         X 900mm   Y 600mm

reach          X            Y          Z
  head 0       [  0, 900]   [0, 600]   [0, 25]
  head 1       [ 60, 960]   [0, 600]   [0, 15]

shared (0,1)   X [60, 900] — 840 of 900   ⚠ 60mm lost to head separation
               Y [ 0, 600] — 600 of 600

enforce        X off   Y off   Z off   A n/a (rotary)
```

## 6. Config

Per axis, replacing the bare `maxTravel`:

```jsonc
"limits": {
  "maxTravel": 900,          // mm. The ANCHOR's travel — X/Y are one shared DOF
                             // (§2), so the anchor's position IS the machine
                             // position. Head 1 (+60) reaches [60, 960].
                             // On Z this is per-head, no offset math.
  "home": { "end": "min" },  // which end the switch is at
  "enforce": false           // per-axis toggle
}
```

`enforce` is per-axis because one axis may be homed and trusted before another.

`maxTravel` defaults to `0` = uncapped, so existing configs stay valid and
inert with no edits.

**Also proposed** — hoist runtime seeds out of the geometry:

```jsonc
"seed": { "head": 0, "tools": { "0": "knife", "1": "pen" } }
```

`defaultHead` and `heads[].tool` are selection, not geometry, and `schema.ts`
already documents them as non-authoritative seeds. Moving them leaves `ToolHead`
as pure geometry for `frames.ts`. A rename and a config migration; no semantic
change. Sequence independently.

## 7. Modules

`machine/frames.ts` and `machine/limits.ts` — both leaves, both pure (no `Link`,
no clock), both testable without a machine.

They belong in `config/` because that is where their *dependencies* are, even
though their consumers are the jogger, planner and orchestrator. Precedent:
`machine/resolve.ts` holds `canRunTool` / `resolveTargets`. Putting them under
`orchestrate` would invert the graph — `operatorJog` depends on `wire` alone
today and would acquire the whole planner subtree.

Two rules:

- **`operatorJog` stays config-free.** Pass a resolved `{xOffset, yOffset}`,
  never a `MachineConfig` — matching how it takes `AxisCalibration` today.
  `jogTo` keeps speaking wire steps.
- **`limits.ts` does not import `plan`.** For the planner's offline check, it
  takes a precomputed travel bounding box; the box walk is the planner's job.

```ts
// machine/frames.ts
export interface XY { readonly x: number; readonly y: number }

export function machineAnchor(m: MachineConfig): { kind: "laser" | "head"; index?: number };
export function toolFrameOffset(m: MachineConfig, headIndex: number, profile?: ToolProfile): XY;
export function homeToTool(pos: XY, offset: XY): XY;
export function toolToHome(target: XY, offset: XY): XY;   // jogTo's caller uses this
```

```ts
// machine/limits.ts
export type Envelope = { readonly min: number; readonly max: number } | null;  // null = uncapped

export type Violation =
    | { kind: "outside";  axis: AxisLetter; target: number; envelope: {min: number; max: number} }
    | { kind: "unhomed";  axis: AxisLetter }
    | { kind: "disarmed"; axis: AxisLetter };

export function headReach(m: MachineConfig, axis: AxisLetter, headIndex: number): Envelope;
export function workArea(m: MachineConfig, heads: readonly number[]): Record<AxisLetter, Envelope>;
export function checkTarget(
    m: MachineConfig,
    headIndex: number,
    frame: "head-centre" | "tip",        // §3.2 — no default
    target: Partial<Record<AxisLetter, number>>,
    axesHomed: number,
): readonly Violation[];
```

**Offline** (config alone): anchor, per-head reach, Z/A envelopes, work area.
**Online**: which head is engaged, which tool is mounted, `axes_homed`. Online
is selection plus one additive term — no geometry computed at runtime.

## 8. Consumers

| consumer | position from | check |
|---|---|---|
| planner | offline, from an assumed start | travel box → **required-start envelope** |
| orchestrator | `STATUS_RSP`, once at job start | actual start vs that envelope |
| jog | `STATUS_RSP`, live | per-move, and per-blend |

The planner is relative-only, so its offline answer is conditional: "safe iff it
starts within this box." Emit that box as a bake artifact and the orchestrator
does one check at job start rather than a per-move check on the hot path.

Jog is the only genuine per-move checker — unbounded and operator-driven.
`ClickJogSource.add()` blends into a *live* session, so the check is per-blend,
not per-session-start.

## 9. Open

1. **Per-blend behaviour** — refuse the blend, or truncate the session? Decide
   when arming jog enforcement.
2. **`seed` block** (§6) — ship the migration or leave it?
3. **Per-head Z reference.** `ReferencePoint` is XY-only, so nothing records
   tool length. Z never composes across heads, so nothing is blocked — needed
   when "same Z command, both heads" should mean the same physical height.
