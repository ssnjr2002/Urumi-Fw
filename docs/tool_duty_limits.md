# Tool Duty Limits

**Status:** PROPOSED — nothing in this document is implemented.

A design for tools that cannot run continuously: the planner schedules the
pauses, bakes the deceleration, and marks where the tool's enable line must be
released and re-asserted.

Written for the ultrasonic knife, but nothing here is knife-specific. Any tool
with a duty limit gets the same treatment by declaring one config block.

---

## 1. The problem

The oscillating ultrasonic knife is driven by a proprietary controller we
cannot modify. Our knife node asserts one digital pin; the controller does the
rest — including shutting itself off after roughly 40 seconds of continuous
power, to protect the transducer from overheating.

Recovery is manual by design: release the enable line, wait 1–2 s for the
controller's fault indicator to clear, re-assert. There is no reset command and
no way to read the controller's state back.

So a cut longer than the budget has to be broken into bursts, and something has
to schedule those breaks.

**Out of scope.** The controller has a second, harsher protection: repeated
near-limit bursts eventually trip a lockout needing several minutes of cooling.
We do not model it, detect it, or plan around it. Jobs long enough to provoke it
are out of scope for this design.

---

## 2. Why this belongs in the planner

Each constraint forces the next, and together they rule out every simpler place
to put the fix:

1. Releasing the enable line means relaying a command to the knife node.
2. Node relays run over RS485, which during a job is saturated by the motion
   stream. The firmware therefore gates every peripheral relay on
   `IDLE`/`PAUSED`/`ALARM` (`control_plane.cpp`) — mid-stream, a relay blocks
   Core 0 on a Core 1 round trip, stretching a step interval and marking the
   material.
3. So the machine must be `PAUSED` at the break.
4. Reaching `PAUSED` mid-job means a baked `MSEG_FLAG_PAUSE`. That flag does
   **not** decelerate. From `core1.cpp`: *"`MSEG_FLAG_PAUSE` is a PLANNED
   boundary the host has already decelerated into."* An operator `pause` ramps
   inside the emitter; a baked one stops at the segment edge and assumes
   velocity is already zero there.
5. So velocity must be zero at the break, which means marking it at
   `constrain` (as `vCeiling = 0`) so `plan`'s sweeps bake the deceleration in
   and the acceleration out.

Consequences worth stating explicitly, because each was considered and is dead:

- **A node-side auto-retrigger cannot work.** The reset needs the line held low
  for 1–2 s. No pulse the node could generate on its own hides that from the cut.
- **A host-side timer cannot work.** It would need the bus mid-stream.
- **The operator cannot do it manually.** Same reason. This is why the buttons
  in the comms demo do nothing during a job.

---

## 3. Switching order

**Release after the lift. Assert before the plunge. Both with the tool clear of
the material.**

Releasing while buried risks the dead blade snagging or tearing on withdrawal.
More importantly, plunging a dead blade is actively harmful: an ultrasonic
cutter does its work through vibration, so an unpowered blade entering material
acts as a wedge — it deflects, pushes rather than parts, and loads the Z axis
and the transducer tip.

The transducer also needs a moment to reach full amplitude. The assert must
therefore lead material contact by at least `settleS`. **The pipeline places
the assert marker to guarantee this geometrically** — the runner acts on markers
and never times blade physics.

**Corollary: the dwell cannot be hidden in a slow lift or plunge.** The window
that must be ≥ `dwellS` lies strictly between release and assert; the lift
precedes the release and the plunge follows the assert. Slowing either only
makes it slow. This rules out per-event Z feed or lift-height overrides, which
is convenient — `discretize` resolves `zFeed`, `liftHeight` and `zSteps` once
per call, and per-event overrides would mean threading parameters through.

---

## 4. What the planner is allowed to know

The planner reasons about **off-windows**, not about heat.

> An **off-window** is the elapsed time between a lift reaching height and the
> following plunge beginning. A lift **qualifies** as a reset opportunity when
> its off-window is at least `dwellS`.

No cooldowns, no transducers, no fault indicators. A tool declares a budget and
a required off duration; the planner finds places to spend them.

The budget accumulates **wall-clock between resets**, including travels, lifts
and pivots — not just cutting time. The controller's own timer runs from
assertion regardless of whether the blade is in material, so anything else would
be optimistic in the unsafe direction.

### Reading the timeline

Baked segments already carry the structure needed, via flags set by
`choreograph`:

- `MICRO_LIFT` (`0x08`) — every `zMove`, up or down.
- `MICRO_JOG` (`0x10`) — travel jogs, head-offset jogs, and the A-rotation
  chunks inside `pivot` and `preOrient`.

So a post-pass can identify every lift and measure the gap to the next descent
without new metadata. Two shapes matter:

- **Subpath end** — lift, travel jog, pre-orient, descend. The gap is the travel
  plus the re-orientation, and is often substantial.
- **Corner** — lift, pivot, descend (`choreograph.pivot`). The gap is one A
  rotation: short, but real, and releasing at lift-top before the pivot still
  beats inserting a fresh stop.

---

## 5. Scheduling: banded greedy

Within `[minOnS, maxOnS]` after the last reset, in priority order:

1. **Reset at an existing lift.** No added motion, no witness mark. Prefer the
   latest qualifying lift in the band; fall back to the latest lift of any kind,
   paying the shortfall as an explicit dwell.
2. **Insert a lift.** Only when the band contains none. Place it at the most
   corner-like sample available — largest tangent change, equivalently lowest
   planned `v`, both already computed by `constrain`. That co-opts a corner
   that did not quite clear `cornerAngleDeg`, rather than stopping mid-sweep.

Insertion is last because it is the only option that leaves a witness mark: the
blade decelerates to rest while buried, dwells, lifts and re-plunges at the same
XY.

`minOnS` prevents churn — without it, a drawing with many short subpaths would
reset at every one of them for no benefit.

### Considered and rejected

- **Greedy-latest for fewest resets.** This is the gas-station problem, and
  greedy is provably optimal for *count*. It is blind to cost, though: it will
  take an expensive insertion at the end of the band over a free lift earlier
  in it.
- **Cost-optimal scheduling.** A DAG shortest path — candidates as nodes, edges
  where the gap fits the budget, edge weight the reset's cost — solvable in
  O(n log n) with a sliding window. Correct, and not worth it: the number of
  resets is forced by the budget, so optimisation only shuffles which ones are
  free. On a ten-minute job the whole prize is a few percent.

The banded greedy approximates the DP and captures most of that. If a
pathological toolpath ever justifies more, the shortest-path formulation drops
in behind the same interface.

---

## 6. Config: `dutyLimits`

An optional block on `ToolProfile`. **Absent means no limit** — pens and crease
tools are unaffected and their presets need no changes.

```
dutyLimits?: {
    maxOnS:   number   // hard budget between resets
    minOnS:   number   // don't reset before this — churn guard
    dwellS:   number   // required release duration
    settleS:  number   // assert-to-contact lead, for amplitude ramp-up
}
```

Grouped rather than flat because the fields are interdependent — a budget with
no dwell is a config error, and `validate` can only say so if it sees them
together. Flat keys would also put four dead fields on every tool that has no
duty limit.

Suggested starting values for the knife: `maxOnS` 30, `minOnS` 20, against a
device limit near 40. The headroom absorbs the approximation in §11.

---

## 7. Wire flags

Two spare bits in the existing flags byte. The firmware honours only
`MSEG_FLAG_WIRE_MASK = 0x06`, so these are host-only hints and **no firmware
change is needed**. The byte round-trips whole through `packet.ts` and
`planFile.ts`, so there is no codec change and no `.plan` version bump.

```
0x01  PATH_END       shared     honoured by nobody (excluded from WIRE_MASK)
0x02  ESTOP          wire       firmware — flush and halt
0x04  PAUSE          wire       firmware — drain, then PAUSED
0x08  LIFT           host hint
0x10  JOG            host hint
0x20  DUTY_RELEASE   host hint  ← new
0x40  DUTY_ASSERT    host hint  ← new
0x80  —              free
```

The markers carry **timing, not identity** — they say "release the active tool's
enable line here", and the runner resolves which peripheral from the profile's
`dutyLimits` and the peripheral registry.

**Invariant:** `DUTY_RELEASE` and `DUTY_ASSERT` are meaningless without `PAUSE`,
since the relay needs the bus and the bus needs `PAUSED`. They are kept as
separate orthogonal bits to preserve the byte's "low bits are wire, high bits
are host" split, so the invariant must be asserted in a test rather than left to
a comment.

Typical values:

```
dwell in place, on a lift segment:
  LIFT | PAUSE | DUTY_RELEASE | DUTY_ASSERT       = 0x6C

masked across a pivot or travel (§10):
  LIFT | PAUSE | DUTY_RELEASE                     = 0x2C   at lift-top
  JOG  | PAUSE | DUTY_ASSERT                      = 0x54   after the gap
```

A pair rather than one "break" bit, because one bit cannot express a release and
an assert at different segments. The runner handles each marker independently,
so the masked variant needs no new runner logic — only a different emission
pattern.

> `PATH_END` is the cautionary precedent: defined, set by the host, never wired
> to behaviour, and misleading readers ever since. These two bits should land in
> the same change as the pipeline that emits them and the runner that acts on
> them — not ahead of either.

---

## 8. Pipeline changes

The stop must be marked at `constrain`, but the qualifying-lift test needs
durations that only exist after `discretize`. That circularity resolves by
iterating:

```
constrain → plan → discretize → measure timeline
    → if a break is needed: mark the sample, re-run
```

Each break sits at a reset boundary and does not consume the following budget
window, so the inter-event durations along the path are invariant and the loop
converges — expect one iteration per budget window.

This makes `compileBlock` a fixed point rather than a straight chain. It also
means regenerating the golden snapshot (`test/production/snapshot.test.ts`,
`UPDATE_GOLDEN=1`) and reviewing the diff.

---

## 9. Runner changes

The runner currently batches consecutive motion events and streams each batch
whole, ORing `MICRO_PAUSE` onto the last segment before a tool swap. It gains
one case:

1. Scan each batch for `DUTY_RELEASE` / `DUTY_ASSERT`. Stream up to and
   including that segment; do not stream past it — the machine will be `PAUSED`
   and `data_plane.cpp` NACKs the remainder with `NACK_PAUSED`.
2. `waitForState(PAUSED)`.
3. On `DUTY_RELEASE`: release the line. On `DUTY_ASSERT`: assert it. Both on one
   segment: release, dwell the shortfall, assert.
4. `resume`, continue from the next segment.

A duty break does not move the machine — it toggles a relay. The firmware
snapshots `resumePos` at the pause and Phase 1 resume expects the host to have
pre-positioned, but position is unchanged here by construction, so no
repositioning is needed. This is the one respect in which a duty break is
simpler than a tool swap.

**Failure handling is safety-critical.** If the assert fails, the runner must
**abort, not resume** — resuming plunges an unpowered blade into material, the
exact failure this design exists to prevent. A failed release is less urgent but
still fatal to the run, since the budget cannot be honoured.

---

## 10. Extension points

Deliberately deferred, listed so the design does not foreclose them:

- **Masked dwell (the main one).** Exploiting a gap means releasing at lift-top,
  resuming to stream the pivot or travel, then pausing again to assert — two
  pause cycles. Without it, *every* reset pays a full `dwellS` in place and the
  "free lift" tier of §5 does not exist; the ladder collapses to "reset at an
  existing lift" versus "insert one". At roughly twenty resets on a ten-minute
  job that is about 40 s of dwell that could be free. The flag pair in §7 is
  designed so this is purely an emission change.
- **Other duty-limited tools.** Nothing above is knife-specific. A tool declares
  `dutyLimits` and gets the same scheduling.
- **Cost-optimal scheduling.** §5.
- **Lockout modelling.** §1.

---

## 11. Known approximations

**The budget is global; baking is per-block.** `compileBlock` sees one block and
knows nothing of the travel that preceded it, so inter-block motion is not
counted against the budget. The `maxOnS` headroom below the device limit absorbs
this. It is a margin, not a guarantee, and a job with unusually long inter-block
travel could still overrun — in which case the controller shuts off, the cut
fails visibly, and nothing is damaged.

**The controller's state is unobservable.** We cannot read the fault indicator,
so a reset that does not take is undetectable: the next pass cuts with a dead
blade and the first sign is the workpiece. `dwellS` should be set with margin
over the observed 1–2 s.
