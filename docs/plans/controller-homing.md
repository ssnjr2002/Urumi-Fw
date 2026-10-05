# Controller homing

How the Pico homes a dual-head machine: parallel legs, cycles, the `home`
controller command, node-addressed `setorigin`, and the homing config. Axis
directions, frames and the work offset are in docs/plans/coordinate-system.md.
Builds on the homing session in
docs/plans/state-handling.md (entry, exits, what a session allows), which
stays there.

The machine: a shared X/Y gantry and per head a Z and an A axis (`heads[]` in
the config, up to `CFG_MAX_HEADS`). Six axes on the dual-head machine, four
stream slots; `select <head>` (docs/plans/motion-sessions.md) binds one head's
Z and A at a time.

## Decisions

### Legs and failures

* **Legs run in parallel**, one per node: a leg may start while legs on other
  nodes run; a second leg on a busy node is refused. The supervisor
  (`core0/ops/homing.cpp`, today one `hNode`) keeps per-node state (deadline,
  poll misses, in-session latch, failure cause) and polls the active nodes in
  turn.
* **A latch is not a failure.** A seek ending on its switch is a finished
  leg; inside the session the latch is recorded, not published as
  `ALARM_LIMIT_LATCHED`. Only `HOMEFAIL_*` causes and estop are failures.
* **Any failure fails the session:** the other running legs are stopped, the
  machine goes to `ALARM_HOMING_FAIL`, and the failing node is named.
* **A park leg:** a homed node moves to its park position (`parkPos`) using
  its datum. It is a leg, not a plain move, so the session still allows no
  motion outside legs.

### Cycles

* Each axis has a `cycle` number in the config (as FluidNC); lower runs
  first, equal numbers run in parallel. Default: every Z 1, everything else 2.
* **Earlier cycles clear first.** Before any axis of cycle k homes, every
  axis of every earlier cycle is made clear, named or not: homed goes to its
  park position (park leg, datum kept), unhomed is homed. A run naming only
  first-cycle axes moves nothing else.
* **Clear is the park position**, where every axis ends a home. Z homes up
  (`seekPositive: true`), so its park position is the top of its soft range:
  the safe height.
* **Uncertain** (mute, excluded, or failed its leg) in an earlier cycle fails
  the session before any later leg runs, naming the axis.
* The rule lives in the controller recipe (only the config knows the cycles);
  the leg primitive stays unguarded, the raw path for the bench.

### `home`

* A controller command: `home [unhomed] [only] [<axis> …]`. Lowercase only
  (the control/data plane mux relies on it).
* **Axes:** `x`, `y`, `z<n>`, `a<n>` with `n` the index into the config's
  `heads[]` (the number `select` takes); `z` and `a` alone mean every head's.
  No axes means all. An unknown name, a head index out of range, or an axis
  the head lacks is `err usage` before anything moves.
* **Named axes** are re-homed (the default); with `unhomed`, only the unhomed
  ones are.
* **`only`** homes the named axes without clearing any earlier cycle: the
  operator asserts the way is clear (as grbl's `$HX`). The recovery for an
  uncertain Z.
* Each cycle runs its legs in parallel; seeks, then retracts; then the next
  cycle; `setorigin` names every node that was homed.
* Replies name axes, not bus ids (e.g. `z1`).
* After a tool swap only Z is probed again; no axis is homed.
* Prior art: grbl/FluidNC `$H`, `$HX`; Marlin `G28 X Y`, `G28 O` (skip
  trusted axes); Klipper `G28 X Y`.

### `setorigin`

* **Node-addressed:** `setorigin <node>:<steps> [<node>:<steps> …]`, one or
  more pairs in any order. No pair, a repeated node, or a token without `:` is
  `err usage`, so every old call answers `err usage`.
* Why nodes, not slots: the unselected head's nodes have no slot while they
  home. The datum is already node-frame and legs are node-addressed.
* A node that is not a stepper is an error before any bus I/O. A node that
  does not answer `DATUM_SET` gives `err node <id> …`; the other nodes still
  go through.
* Its state rules (no alarm clearing, estop-wins check, valid outside a
  session) are in docs/plans/state-handling.md.

### Homing config

Per linear axis (frames and directions: docs/plans/coordinate-system.md):

```jsonc
"x": {
  "maxTravel": 480,          // usable length from the park position
  "softLimits": true,        // enforce [park, park ± maxTravel]
  "invertDir": false,        // wiring fix so + is physical; checked by jogging
  "homing": {
    "cycle": 2,              // lower homes first; equal homes in parallel
    "seekPositive": true,    // home toward + (switch at the + end)
    "seekScaler": 1.2,       // seek budget = (maxTravel + pullOffDist) × this
    "startFeed": 2.5, "seekFeed": 12.5, "latchFeed": 0.78, "rampSteps": 400,
    "backoffDist": 2,        // retract between the fast and slow approaches
    "pullOffDist": 5,        // final retract (FluidNC's pulloff_mm)
    "parkPos": 0             // optional: coordinate after the pull-off
  }
}
```

* **`parkPos` places 0.** Absent: the trip is at the axis's 0 end (homing −)
  or at `maxTravel + pullOffDist` (homing +), so the range is `[0, maxTravel]`
  plus the pull-off. Given: the axis parks at that coordinate, as FluidNC's
  `mpos_mm` (the position after the pull-off, not the trip). With an explicit
  `parkPos`, retuning `pullOffDist` moves every point stored in machine
  coordinates; leave it absent to keep the frame on the trip.
* **Commissioning order:** jog + and fix `invertDir` first; then home and fix
  `seekPositive`. Fixing a wrong + with `seekPositive` homes correctly and
  mirrors every job.
* Replaces `hardTravel`, `atOrigin` and `invert`; renames `backoffMm`,
  `parkMm`, `pullInFeed` and `datumDeg`. The web keeps the old fields until its homing
  path is removed; the Pico reads only the new ones.
* **Z is ISO:** + up, `seekPositive: true`, typically `parkPos: 0`: range
  `[-maxTravel, 0]`, 0 at the top, the park position is the safe height.
* Rotary A keeps its own block; shares `invertDir` and `cycle`:

  ```jsonc
  "homing": {
    "kind": "rotary", "cycle": 2,
    "budgetRevs": 4,         // runaway ceiling, revolutions
    "startFeed": 10, "sweepFeed": 60, "rampSteps": 400,
    "toleranceDeg": 2,       // max disagreement of the two sweeps
    "indexPos": 0            // coordinate of the index itself
  }
  ```

  `indexPos` (was `datumDeg`) numbers the index, not a rest point: a rotary
  home has no park. It is the angle the knife's edge points at while the axis
  sits at the index (0° = X+, counter-clockwise +). From CAD: sensor angle
  from X+ minus magnet angle from the edge. Check by homing, jogging the edge
  to face X+ and subtracting the reading.

### Dropped

* Slot-framed `setorigin`: assumed every slot always has a node, which dual
  heads break.
* A plain move inside the homing session to park Z: the park leg instead.
* Cycles as ordered groups (`[[z], [x, y, a]]`): a per-axis number instead.
* `ignore` (skip only uncertain axes, clear the rest): `only` skips all
  clearing.
* Raw `switchDir`/`originDir` bits: `originDir` tied + to the chosen origin,
  so an origin choice could mirror the frame. Replaced by a physical +
  (`invertDir`), `seekPositive` and `parkPos`.
* `hardTravel`: the range comes from `maxTravel` and `parkPos`, the seek
  budget from `seekScaler`.
* Positive-down Z: with signed ranges, ISO Z costs nothing.

## Wire changes

* Commands: `home`; `setorigin` node-addressed syntax.
* Config: the homing config above, read by the Pico.

## Branches

Proposed order. Each gets a full Plan section when its turn comes.

1. `feature/homing-config`: the homing config above, decoded by the Pico
   (`core0/config/config_decode`); added to the web schema and loader beside
   the old fields; fixtures and `docs/homing.md`. Depends on nothing.
2. `feature/homing-legs`: parallel legs with fail-all, the park leg,
   node-addressed `setorigin`. Depends on state-handling branch 3.
3. `feature/home-command`: the `home` controller command. Depends on 1 and 2.

## Branch 1: `feature/homing-config`

### Plan

* **Type:** `feature`. **Depends on:** nothing.
* **Purpose:** the Pico decodes and validates the homing config above. Nothing
  acts on it yet (branches 2 and 3 do). The web carries the new fields through
  to the blob beside the old ones, which it alone keeps using.
* **Pico** (`src/rp2350/core0/config/`):
  * `config_decode.h:30-38` `CfgAxis`: `invert` → `invertDir`; add
    `softLimits` and a `CfgHoming` (`present`, `cycle`, `seekPositive`,
    `seekScaler`, the four feeds/ramp, `backoffDist`, `pullOffDist`,
    `hasParkPos`, `parkPos`), and for a rotary block `budgetRevs`,
    `startFeed`, `sweepFeed`, `rampSteps`, `toleranceDeg`, `indexPos`.
  * `config_decode.h:18` `CFG_SCHEMA_VERSION` 1 → 2, so an old blob is
    `version`, not `missing`.
  * `config_decode.h:59-69` and `.cpp:152-166`: add `CFG_DEC_HOMING`
    (`"homing"`) for an out-of-range homing field.
  * `config_decode.cpp:15-23` `filterAxis`, `:44-55` `readAxis`, `:59-66`
    `checkAxis`: read and check the new fields. The homing block is optional
    (absent: not homeable); present, every field is required except `parkPos`
    and `cycle` (default Z 1, else 2, applied by the decoder from the axis's
    place).
* **Web** (pass-through only, no behaviour change). The blob is built from
  the resolved config, which the loader builds field by field, so a field it
  does not copy never reaches the Pico. The Pico's decoder is the only check;
  the web adds no validation for code that is about to be removed.
  * `web/src/machine/schema.ts:122-171` `LinearHoming`, `:191-244`
    `RotaryHoming`, `:375-` `AxisConfig`: the new fields, optional, beside
    the old ones.
  * `web/src/machine/json/load.ts:80-91` `JsonAxis`, `:108-125` `JsonHoming`,
    `:437-448` axis build, `:517-585` `buildHoming`: copy the new fields
    through when present. The rotary name check (`:545-551`) must not reject
    them.
  * `web/src/machine/json/blob.ts:15` `CONFIG_BLOB_VERSION` 1 → 2.
* **Fixtures and tests:**
  * `web/test/fixtures/test-machine.json`: homing blocks on x, y, z and a
    rotary block on a (new and old fields), so `good.msgpack` carries them.
  * `web/test/machine/json/blob.test.ts:55-63` `BAD`: add `bad_homing`;
    regenerate `web/test/fixtures/config/*.msgpack`.
  * `test/test_config/test_config_decode.cpp`: decode checks for the new
    fields, `cycle` defaults, absent `parkPos`; `homing` in the reasons list.
  * `web/demo/comms.json`: the new fields beside the old.
* **Docs:** `docs/homing.md` §3.1 (schema) and §3.2 (`invert`): the new
  fields and that the Pico reads only them.
* **Checks:** `pio run -e pico`, `pio test -e native`, `pnpm typecheck` and
  `pnpm test` in `web/`.
* **Not in this branch:** the probe block keeps `pullInFeed`, `backoffMm`,
  `parkMm`; renaming it is the Z/probe open question in
  docs/plans/coordinate-system.md.

### Status

Ready to merge.

### Outcome

* `web/demo/comms.json` not updated: its new fields describe the real machine
  (directions, `maxTravel`), set when motion commissioning starts. Until then
  the Pico rejects its blob as `missing` (no `invertDir`).
* Decided in Read: `invertDir` and `softLimits` are required on every axis.
  The web adds no validation; `load.ts` passes the fields through.
* Errors: a missing or wrong-typed field is `missing`; a value out of range,
  or a homing kind that does not match the axis, is the new `homing`.
  Rejected: `cycle` 0, non-positive feeds and distances, `seekScaler` < 1,
  linear homing with `maxTravel` 0. `rampSteps` is 16 bits; `budgetRevs`
  is only checked positive (the 3.2 floor stays a config judgement).
* `docs/homing.md` §3.2a lists the Pico's fields and what each replaces.

## Open questions

* Guarding earlier-cycle axes while later legs run (still enabled and unmoved
  since their leg ended, plus still latched where there is a switch), and a
  minimum seek travel to catch a stuck switch. None of grbl, Marlin or Klipper
  does either.
* Stopping a running leg without a full `CMD_MAKE_SAFE` (needed by fail-all):
  check the node firmware.
* FluidNC's `cycle: 0` (homed only by name, never by a plain `home`).
* The homing recipe on the Pico: the controller sequence that holds the
  session, and the handler bodies (`setorigin`'s datum loop, the enables) that
  move into ops for it. The probe session may later follow the same shape.
