# Binding the head at the bake

**Status:** stages 1–5 landed; 6 outstanding. Nothing reconstructs a head from a tool any more — the walk reads `block.head` and the runner acts on the walk's `rebind` event. What remains is deleting `plan/` and its consumers, and patching `demo/`, which is knowingly broken until the end.

## The bug, stated once

`compileBlock` resolves its axes with `resolvedAxes(machine)`, which returns
`heads[machine.defaultHead]`'s Z/A regardless of which tool the block is for
(`resolve.ts:38`, `compileBlock.ts:73`, and again in `discretize.ts:91`). On the
bench machine head 0's Z is 1200 steps/mm and head 1's is 600. So a pen block
destined for head 1 gets its plunge discretised at head 0's resolution — wrong
by 2x, no exception, before `orchestrate/` ever sees it.

The compiled block then has no `head` field, so the choice is discarded.
`walk.ts`, `runWalk`, and the Controller each reconstruct "which head"
afterwards from a caller-passed `headAssignment` map defaulted with `?? 0`. Four
layers deriving a fact layer one knew and threw away.

The `rebind` event added earlier (walk.ts / runWalk.ts) binds the right *motors*
to segments carrying the wrong *step counts*. Necessary, not sufficient.

## The decision

Decide the head before compiling, write it into the compiled block, and let every
downstream layer *read* it instead of *re-deriving* it.

mm→steps cannot happen until the head is known, because Z/A `stepsPerUnit`,
`invert`, `maxFeed` and `maxAccel` are per-head and the trajectory (segment
count, intervals) is shaped by all four — it is not a scale factor applied at the
wire. X/Y are one gantry, fixed at slots 0/1, and `startSteps` is tool-frame, so
**XY geometry never needed the head**; only Z and A did. That is why choreograph
(`headOffsetJog`) is untouched below.

## Vocabulary

Four nouns. Use these and no synonyms.

| noun | means |
|---|---|
| `Block` | one layer's work before compiling: `{profile, slot?, subpaths}` |
| `CompiledBlock` | that, compiled: adds `{head, segments, startSteps}` |
| `SwapPhase` | a run of blocks executable without operator intervention |
| `Mounts` | head-indexed tool table: `readonly (ToolType \| null)[]`, null = empty socket |
| `Setup` | the live rig: `{engaged, mounts}` |

Retire `LayerBlock`, `CompileBlockResult`, `Plan`, `Schedule`, `MountSet`, and
`Phase`. Containers become bare arrays — `readonly CompiledBlock[]`, `readonly
SwapPhase[]` — because a named wrapper around a single array field earns nothing.

Keep `Block` and `CompiledBlock` distinct rather than one type with optional
fields. The compiler then refuses to hand an uncompiled block to the walk, which
is precisely the boundary this redesign exists to enforce.

`Setup` survives as a type because it means more than its parts: it pairs what is
fitted with which head is wired to slots 2/3. `Mounts` is only the first half.

## The pipeline

`scheduleMounts` never reads `segments` or `startSteps` — every line of it
touches only `profile.toolType` (`schedule.ts:66`, `:103`). It is a pure function
of the tool sequence, and takes a `Plan` today only because that is what was
lying around. Run it *before* the bake, where it can hand each block its head.

```
svg
 └─ assembleBlocks  → Block[]                      tool sequence exists here
     └─ orderBlocks → Block[], permuted            PLACEHOLDER: identity
         └─ scheduleMounts(machine, tools, mounts) → SwapPhase[]
             └─ compileBlock ×N, axes from the phase's head
                 └─ CompiledBlock[]  head frozen, segments frozen
                     └─ walkSchedule → WalkEvent[]  pure; reads block.head
                         └─ runWalk  → wire         verifies, never derives
```

`orderBlocks` is a placeholder returning document order unchanged; it exists so
execution-order policy, when it arrives, has a home upstream of scheduling. See
"Ordering".

Move `schedule.ts` from `orchestrate/` to `production/` with this change. It was
always pure scheduling with no physical state (its own header says so), sitting
in `orchestrate/` only because the walk used to be its one caller. The move also
prevents a cycle: otherwise `production/bakePlan.ts` imports
`orchestrate/schedule.ts` while `orchestrate/walk.ts` imports `production`'s
`CompiledBlock`. After it, dependencies run one way — `production/` makes the
blocks, `orchestrate/` turns them into events, `controller/` executes them.

## `accepts` replaces the seed

`heads[].profile` is documented in `schema.ts:260` as "a SEED mount only", and
`setup.ts`'s header calls it "a type apologising for carrying something it cannot
vouch for". Delete it rather than generalising it into a second field. Have each
head declare what its fixture can hold, preference-ordered:

```json
"heads": [
  { "z": {…}, "a": {…}, "xOffset": -50, "accepts": ["knife", "crease"] },
  { "z": {…}, "a": {…}, "xOffset":  50, "accepts": ["pen", "crease"] }
]
```

- **membership** = compatibility. The crease wheel fits either fixture.
- **position** = preference. Head 0 would rather hold the knife.

Store tool names in JSON — they are what an operator reads and edits — and
resolve each to its `ToolType` on load, as `heads[].tool` does today. Key
everything internally on `ToolType`.

Put this on the head, not the tool profile: tool profiles are machine-portable
(`KNIFE` in `tools.ts` is a preset describing a blade, shipped as a constant with
no machine attached), while head definitions are already the machine-specific
half — bus nodes, offsets, `stepsPerUnit`, `invert`. "This socket's fixture
accepts a knife" is the same kind of fact in the same place. Validation stays
local too: for each head, do the tools it claims need axes it has? One pass, no
join.

`setupFor(machine)` seeds `mounts[i] = accepts[i][0] ?? null`. `defaultHead`
survives only as the initial `engaged`.

### Why `ToolType` is a safe key

`machine/json/load.ts:302` restricts `json.tools` to patching one of four fixed
preset names (`pen`, `knife`, `crease`, `revolver_pen`), and `patchToolProfile`'s
override key list (`load.ts:391-392`) never includes `toolType`. Every profile
the loader can produce keeps the `toolType` of the preset it patched, so `name`,
`toolType`, and `ToolProfile` identity stay in lockstep on the real load path.
Prefer the enum: small, stable, usable directly as a `Map` or JSON key with no
name→profile indirection.

Re-validate that bijection where `accepts` is parsed, so a future loosening of
the loader — a genuinely user-defined profile name — trips a named check instead
of quietly colliding two profiles. The revolver pen shows why the bijection must
hold: its blocks share one `ToolType` across slots by design, so a second profile
sharing that type would be indistinguishable from a revolver slot to every
consumer keyed on `toolType`.

## Scheduling

```ts
scheduleMounts(
    machine: MachineConfig,
    tools: readonly ToolType[],   // per block, document order
    mounts: Mounts,               // what is in the sockets NOW
): readonly SwapPhase[]

interface SwapPhase {
    readonly mounts: Mounts;                   // in force for this phase
    readonly blockIndices: readonly number[];  // execution order
    readonly swapIn: readonly ToolType[];      // vs the previous phase
    readonly swapOut: readonly ToolType[];
}
```

Head assignment is an **output**, not an input. There is no `assignHeads`: the
scheduler is the only layer that must reason about which socket holds what, so it
is the layer that decides. A block's head is the index where its phase's `mounts`
holds that block's tool.

### The `mounts` argument

Take the current mount table as a parameter and do not care where it came from.
A bare machine is `[null, null]`; a declared baseline is `[KNIFE, null]`; the live
rig is whatever `Setup.mounts` says. The scheduler is deterministic in its inputs
either way, which turns what looked like an algorithm choice into a caller
choice:

```ts
scheduleMounts(machine, tools, [null, null])   // reproducible — same bake every time
scheduleMounts(machine, tools, ctl.setup.mounts) // swap-minimal — fewest tool changes
```

Reproducibility matters because the head decides step counts: a plan baked
against a live rig is not byte-identical to one baked against a bare machine.
Callers who need a diffable bake pass a fixed table; callers who need the fewest
operator swaps pass the live one. Neither is hidden state.

Validate the incoming table before scheduling anything:

- length equals `machine.heads.length`
- every non-null entry appears in that head's `accepts`

`[PEN, KNIFE]` against `head0.accepts = [knife, crease]` is a caller error worth
naming, not something to silently work around.

### The fill rule

Two separable questions, and conflating them is where greedy placement goes
wrong: **which** tools share a phase, and **where** each one sits.

**Which** grows in first-use order, stopping at the first tool that will not fit
alongside the ones already chosen. First-use order is right here even though
placement must not depend on it — execution stops at the first block whose tool
is missing, so a tool first needed after that point cannot run this phase however
well it would have fitted.

**Where** is a bipartite matching over the resulting set (Kuhn's algorithm), not
a greedy placement. It has to be: a tool that fits several heads can take the
only socket another tool has, and greedy cannot give it back. Matching can — when
a head is taken it asks the occupant to move, recursively.

```
head0.accepts = [knife, crease]      head1.accepts = [pen, crease]

tools [knife, crease] → knife takes head 0 (its only option), crease head 1.
tools [crease, knife] → greedy: crease grabs head 0, knife has nowhere to go,
                        and the job gains a swap it never needed.
                        matching: crease moves aside. Same arrangement.
```

Ordering the placements most-constrained-first fixes this *example* and is still
wrong in general; matching is exact, order-invariant by construction, and about
the same amount of code. The sets hold at most one tool per head, so the cost is
irrelevant. Test order-invariance directly rather than trusting it emerges.

Carry `mounts` forward by trying each tool's **current socket first** during the
matching. This steers without constraining: an arrangement needing no operator
work is the one found, so a tool stays put across phases — and across jobs, when
the caller passes the live table — unless something genuinely has to move.

Filling is by socket **occupancy**, which the matching gives for free: a tool
whose heads are all taken does not stop the fill, it simply fails to join this
phase. Counting distinct tools instead returns the physically impossible mount
`[A, C]` when both want head 0.

### Known limits

- **Phasing stays document-order-dependent.** With more tools than heads, *which*
  tools share a phase follows block order, because blocks never reorder. Binding
  is order-invariant; phasing is not, and cannot be without reordering work.
- **`swapOut` over-reports.** `mountDiff` names a still-fitted tool as removed
  when its head is merely idle next phase, so the operator is asked to take out
  something that could stay. Cosmetic, prompt-only.
- **Frequency is ignored.** `[A,B,A,C,A,B]` would benefit from knowing A recurs
  throughout. Greedy lookahead is a deliberate starting point, not an optimum.

## Ordering

`orderBlocks` returns document order unchanged. The placeholder's value is the
seam position, not the code; assert it is identity so whoever implements it gets
a failure saying "you changed job semantics" rather than silently reordering
everyone's cuts.

Its contract, which belongs in its doc comment: **head-independent constraints
only.** Constraint-shaped ordering ("crease before the cut that frees the part",
"keep a tool's blocks contiguous") is a physical fact about the job and sits
correctly upstream of scheduling. Cost-shaped ordering ("reorder to minimise
swaps") cannot evaluate its objective without the assignment that depends on the
order — that is one joint problem, and it wants a new stage, not a bigger
`orderBlocks`.

## `plan/` dissolves

`src/plan/` holds two files and both are ending. Delete the directory.

`Block` and `CompiledBlock` move to `production/compileBlock.ts`, next to the
function that makes them:

```ts
/** One layer's work, before compiling. */
export interface Block {
    readonly profile: ToolProfile;
    readonly slot?: number;
    readonly subpaths: readonly (readonly CubicBezier[])[];
}

/** A block compiled against a specific head. */
export interface CompiledBlock {
    readonly profile: ToolProfile;
    readonly slot?: number;
    readonly head: number;
    readonly segments: readonly MicroSegment[];
    readonly startSteps: { readonly x: number; readonly y: number };
}
```

Pass `head` into `compileBlock` — it picks the axes. Leave `slot` outside;
the compiler never reads it, so assembling it in `bakePlan` beats threading it
through to be echoed back.

Drop the `Plan` wrapper: `{ blocks: readonly CompiledBlock[] }` carries nothing a
bare array does not, and every caller immediately unwraps it. Same for
`Schedule` over `SwapPhase[]`.

`feasibleOn` and `planRequiredAxes` have no caller anywhere in `src/`. Delete
`feasibleOn` — `accepts` validation at load time subsumes it. Before porting
`planRequiredAxes`, find its live equivalent: it computes a real firmware
requirement (MCFG `required_axes`), nothing calls it, so the duplication its own
doc comment complains about may already be resolved elsewhere. Replace
`planToolTypes` with `toolsUsed(blocks: readonly Block[]): ToolType[]` in
`production/` — the pipeline needs the tool sequence before anything is compiled.

## Files

### Stage 1 — machine layer

| file | change |
|---|---|
| `machine/schema.ts` | `ToolHead.profile?: ToolProfile` → `accepts: readonly ToolType[]`; `toolHead()` follows. Narrow `defaultHead`'s doc to "initial engaged head" |
| `machine/resolve.ts` | rename `resolvedAxes` → `resolvedAxesDefault` (name `defaultHead` out loud); promote `axesForHead(machine, head)` here from `walk.ts:107` as the shared primitive; add `headsAccepting(machine, tool): number[]` |
| `machine/setup.ts` | rename `Setup.mounted` → `Setup.mounts`; seed from `accepts[0]`; rewrite the header (the "seed apologising" paragraph is now history) |
| `machine/slots.ts` | delete `headAssignment()` |
| `machine/json/load.ts` | parse `accepts`, resolving names to `ToolType`; error on the old `tool`/`profile` key, naming what replaced it |
| `machine/json/validate.ts` | resolve every `accepts` name to a known `ToolType`; require an A node on any head accepting a tangential tool; warn when a tool is accepted by no head |
| `index.ts` | rename the `resolvedAxes` export; drop `headAssignment` |

Move call sites to `resolvedAxesDefault` with identical behaviour, so this stage
stays green apart from fixtures adopting `accepts`.

### Stage 2 — schedule ahead of the bake

| file | change |
|---|---|
| `production/order.ts` | **new.** `orderBlocks(blocks, machine)` → identity. Short doc comment stating the head-independent-constraints-only contract |
| `production/schedule.ts` | **moved from `orchestrate/`.** `scheduleMounts(machine, tools, mounts)` → `SwapPhase[]`. Validate `mounts`; carry it forward across phases; fill by occupancy, most-constrained-first. `Phase` → `SwapPhase`, `mount: MountSet` → `mounts: Mounts` |
| `production/bakePlan.ts` | wire `orderBlocks` and `scheduleMounts` between `assembleBlocks` and the compile loop; take `mounts` as an option defaulting to `setupFor(machine).mounts`; return `{blocks, phases}` and **drop `bytes`** |

### Stage 3 — per-block resolve

| file | change |
|---|---|
| `production/compileBlock.ts` | define `Block` and `CompiledBlock` here (replacing `plan/plan.ts`); take `head` as a parameter; resolve via `axesForHead(machine, head)`; thread axes into `discretize` |
| `toolpath/discretize.ts` | `:91` takes `ResolvedAxes` as a param instead of calling `resolvedAxes(machine)` |

This is the stage that turns `headResolve.test.ts` green.

### Stage 4 — walk reads, stops deriving

| file | change |
|---|---|
| `orchestrate/walk.ts` | delete the `headAssignment` option, `state.headIndex`, the private `axesForHead`, and `initialState.headIndex`. A rebind is `blocks[i].head !== blocks[i-1].head` over execution order, with an unconditional leading rebind. Import `CompiledBlock` from `../production/compileBlock.js` |
| `orchestrate/estimate.ts` | none — head-blind |

Keep `walkSchedule` a pure `(SwapPhase[], readonly CompiledBlock[],
MachineConfig) → WalkEvent[]`. It must not read live `Setup`: everything it
derives from the head is geometry `block.head` already answers — the offset jog
from `heads[].xOffset`, A-home against that head's axes, whether to emit a
rebind. Comparing plan against reality belongs to the controller.

### Stage 5 — controller executes and verifies

| file | change |
|---|---|
| `controller/runWalk.ts` | delete `rebindForSwap` — the last tool→head reconstruction; the `rebind` branch already reads `ev.head`. Add the pre-flight check and re-run it after each `confirmSwap` returns |
| `controller/controller.ts` | `verifyMounts(blocks, setup)` helper; shrink `walkState()` to `posX/posY/aPhys` |
| `index.ts` | export surface follow-through |

Compare, do not derive:

```ts
for (const b of blocks) {
    const fitted = setup.mounts[b.head];
    if (fitted?.toolType !== b.profile.toolType) {
        throw new Error(
            `block needs ${b.profile.name} on head ${b.head}, but head ${b.head} ` +
            `holds ${fitted?.name ?? "nothing"}`,
        );
    }
}
```

Run it twice: once pre-flight over every block, before any material moves, and
again after each `confirmSwap` returns, over the upcoming phase only — the
operator has just changed the machine, and `confirmSwap` returning `true` is a
human claim, not evidence. That turns a mis-mount into a refusal instead of a
wrong cut.

Keep `engaged` and `mounts` distinct throughout: a rebind changes `engaged`
(which socket's motors own wire slots 2/3), a swap changes `mounts` (what is
screwed in).

### Stage 6 — remove `plan/`, deprecate its consumers

| file | change |
|---|---|
| `plan/plan.ts` | **deleted.** Types relocated in stage 3; `planToolTypes`/`planRequiredAxes`/`feasibleOn` moved or dropped |
| `plan/planFile.ts` | **deleted.** Both remaining callers are deprecated in this same stage, so a throw-on-call tombstone would fail for nobody. The codec stays in git history |
| `demo/main.js` | whole purpose is bake→download `.plan` (`:92–123`) — mark deprecated |
| `demo/orchestrate.js` | built entirely on `loadPlan` (`:178`) — mark deprecated |
| `demo/comms.js` | drop `liveInitialState` and the `headAssignment()` call; pass `mounts` to `scheduleMounts` |
| `test/plan/*.test.ts` | **deleted** |

`.plan` cannot survive this honestly: a serialised plan encodes step counts
resolved against one head arrangement, so a file baked under one `accepts` config
is silently wrong under another.

### Tests

| file | change |
|---|---|
| `test/production/headResolve.test.ts` | unskip the spec block; tighten the second test from "no head declares it" to "no head accepts it" |
| `test/machines.ts` | `twoHeadMachine()` already carries distinct 1200/600 Z calibration; swap its `profile:` seeds for `accepts:` |
| `test/production/order.test.ts` | **new.** Assert `orderBlocks` is identity |
| `test/production/schedule.test.ts` | **moved from `test/orchestrate/`.** New signature; `mounts` validation; the `A→0, B→1, C→0` occupancy case; most-constrained-first order-invariance |
| `test/orchestrate/walk.test.ts` | drop `headAssignment` wiring; derive rebinds from `block.head` |
| `test/controller/runWalk.test.ts` | `rebindForSwap` gone; add the mis-mount refusal |
| 8 files naming `resolvedAxes` | `choreograph`, `controller`, `setup`, `cppRefDiscretize`, `dutyBreaks`, `microsegment`, `discretize`, `dutyInsert` — mechanical rename |

## Scope

```
~18 source files (2 deleted, 2 moved, rest edited), ~13 test files.
```

Net line count should fall. Out: `plan/plan.ts`, `plan/planFile.ts`, a
`WalkOptions` field, a mutable state field, `rebindForSwap`, `headAssignment()`,
`heads[].profile`, the `Plan` and `Schedule` wrappers, and a whole `?? 0`
reconstruction path. In: `accepts`, a `head` field, a `mounts` parameter, and a
one-line `orderBlocks`.

## What does NOT change, and why

- **choreograph** — `headOffsetJog` emits the head-to-head XY delta and was
  always correct; XY is tool-frame and head-independent.
- **`frames.ts` offsets** — those feed UI readout and go-to targeting
  (`headXY`/`tipXY`), a different consumer from the motion path.
- **the offline/online split** — sharpened, not broken. The bake becomes
  head-aware (it must — the head decides segment content), and the Controller
  drops from *deriving* the head to *verifying* it. Per block → toolpath; inter
  block → scheduler; end-to-end execution + verification → controller.

## Decisions settled

- **Key on `ToolType`, not `ToolProfile`.** The loader guarantees the bijection;
  the enum is the smaller, stabler key. Re-validate at the `accepts` boundary.
- **`accepts` stores names in JSON, `ToolType` internally.** Names stay legible
  to whoever edits config.
- **No per-job head override.** The operator does not pick heads. Route a broken
  head, or any exceptional placement, through `machine.heads[].accepts` — the
  same source of truth every job already bakes against.
- **No `assignHeads`.** The scheduler is the only layer that must reason about
  socket contention, so it decides, and head assignment falls out as an output.
- **`mounts` is a parameter, not a policy.** Reproducible and swap-minimal bakes
  are the same function with different arguments.
