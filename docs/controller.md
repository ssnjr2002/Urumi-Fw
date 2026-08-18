# The Controller

**Status:** design agreed, not implemented.
**Prerequisite:** done — `web/src/config/` is now `web/src/machine/` + `machine/json/` (commit `ffd6546`).

## Why this exists

The library today is two towers that touch at only two type-level edges:

```
offline (bake)   machine → svg → toolpath → choreograph → production → plan → orchestrate
online (move)    machine → wire/format → wire/link → operatorJog
```

That separation is real and worth keeping: **no module in `src/` is both
config-aware and live-state-aware.** The one thing that holds a `MachineConfig`
and a `Link` at the same time is `web/demo/comms.js` — a 1856-line demo file.

Which means every browser UI built on this library has to re-implement the join.
The evidence that this is not hypothetical is that we already did it three times
inside one demo folder:

| Duplicated thing | Copies | Note |
|---|---|---|
| `liveInitialState` | `comms.js:1543`, `bench.js:146` | byte-identical |
| `estimateSeconds` | `comms.js:1425`, `bench.js:166` | near-identical |
| idle/state waiting | `waitIdle` `comms.js:1106`, `waitForState` `:1599`, + 2 more | four shapes of one idea |
| state names | `orchestrate.js:124` | positional array, drifts from the enum |
| required axes mask | `orchestrate.js:114` | hardcoded `0x04`/`0x08` — a live question answered statically |

The Controller is the missing object: the one place allowed to know both the
machine description and the live link.

## Charter — three invariants, each with exactly one possible owner

**A. Exclusivity.** The ack sink is a single shared resource. Two concurrent
senders (a jog and a plan run, or two plan runs) corrupt the sequence window.
Nothing below the Controller can enforce "one session at a time" because
`Link` deliberately does not know what a session is *for*.

**B. Setup reconciliation.** The axis map never appears in `STATUS_RSP`. The
firmware knows which node drives which axis; the host knows which head carries
which tool. Neither can answer "is the machine currently set up to run this
plan?" alone. Reconciling the two is the Controller's job, and the answer
changes at runtime — which is why `resolvedAxes()` resolving Z/A against
`machine.defaultHead` is a static answer to a live question.

**C. Frame-correct live position.** Converting a wire position into tool-frame
mm requires knowing which head is active *right now*. `mmOf` in the demo read Z
through the wrong head's calibration: a silent 2× error, no exception, wrong
cut. The frames model (`machine/frames.ts`) has the maths; it has no way to know
the active head. The Controller does.

Everything else the Controller might do is convenience. These three are
correctness.

## Scope boundary

The Controller **owns**: the active `Link`, the current `Setup` (which head is
mounted, which tool is in it), session exclusivity, the live status poll, and
frame conversion of live positions.

The Controller **does not own**: baking (stays pure and offline), packet
framing (`wire/format`), transport (`wire/link`), or any UI concern. It emits
state; it does not render it. No DOM, no timers the caller cannot inject.

## Staged plan

Each stage stands alone and leaves the suite green.

1. **Fixture consolidation + package boundary.**
   Package boundary is **done** — `exports` now carries the two real-port
   backend subpaths that `src/index.ts` told consumers to import, plus
   `test/packageExports.test.ts` to keep the map from drifting.
   Remaining: move `choreograph.test.ts`, `orchestrate/walk.test.ts`,
   `plan/plan.test.ts` onto `test/machines.ts`. They are the only three files
   outside the machine tests that hand-roll a machine, so this makes the blast
   radius of every later stage exactly one file.

2. **`settled` into `wire/link`.** Deletes the four copies above. Smallest real
   change; no new concepts.

3. **Offline description helpers.** `machine/slots.ts` (`buildAxes`,
   `desiredMap`, `headAssignment`) and `machine/names.ts` (state/alarm/reason
   names, `maskStr`) — these are pure functions of the machine description that
   only ever lived in the demo because that is where they were first needed.
   `estimateSeconds` goes to the plan/work side. Fixes `orchestrate.js:114`.

4. **`Setup`** in `machine/setup.ts` — the runtime mount table. Seeded from
   `defaultHead` so nothing existing has to move, which also retires the
   "seed vs. truth" apology currently written into `schema.ts`.

5. **`Controller`** over the top, then rewrite `comms.js` as a thin consumer.
   **The proof of the whole exercise:** `comms.js` shrinks substantially and
   stops importing `../src/` internals.

## Known debts this touches

- `machine/defaults.ts:68` writes `0x01 as const` to dodge a cycle, because
  `ToolType` lives in `schema.ts` and `schema.ts` imports `DEFAULTS`. A real
  cycle, independent of directory layout; the fix is moving the enums to a leaf.
- The three demos import `'../src/index.js'` (raw source), so `dist/`, the
  `exports` map and the `.d.ts` output have no consumer exercising them.
  Stage 1's boundary test is a floor, not a substitute.
- Soft limits (`machine/limits.ts`) remain PENDING — see
  `docs/coordinate_frames_and_limits.md`, Stage 2. The Controller is where a
  limit violation would be caught, so these are related but separable.
- Deferred by choice: one point type (`ReferencePoint`/`XY`/`Pt`/`ToolOffset`),
  and tagging geometry with its frame + head in the type. The latter is the
  expensive one (choreograph 64 tests, walk 13, plus production/plan) and is
  what would eventually lift `scheduleMounts(plan, 1)`.
