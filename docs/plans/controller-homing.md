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
* **A failure fails its cycle:** the other running legs of that cycle are
  stopped with a leg abort (per node, never a broadcast), the machine goes to
  `ALARM_HOMING_FAIL`, the failing node is named, and no later cycle runs.
* **A cycle commits whole or not at all.** `setorigin` runs at the end of each
  cycle, for every node of it; nothing from a failed cycle is committed, not
  even a sibling that finished. Earlier cycles keep their datums: a failure
  invalidates only its own leg's node, and `home_end` touches no origin. A
  retry (`home unhomed`) parks the earlier cycles and homes the failed one.
* **A leg abort** stops a node's pulser and leaves it energised, so its
  counter and `NODE_FLAG_DATUM` stay good. `CMD_MAKE_SAFE` is not used: it
  de-energises and drops the witness.
* **A park leg:** a homed node moves to its park position (`parkPos`) using
  its datum. It is a leg, not a plain move, so the session still allows no
  motion outside legs. The node drives it: it runs to an absolute counter
  value, ramping up and down. A switch asserting on the way stops it, and
  ending anywhere but the target is a failure.

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

* Commands: `home`; `setorigin` node-addressed syntax; `leg <node>
  seek|retract|sweep|park …` and `leg_abort` (raw, bench) replace `lin_leg`
  and `rot_leg`.
* Node: `CMD_PARK_LEG`, `CMD_LEG_ABORT`.
* Config: the homing config above, read by the Pico.

## Branches

Proposed order. Each gets a full Plan section when its turn comes.

1. `feature/homing-config`: the homing config above, decoded by the Pico
   (`core0/config/config_decode`); added to the web schema and loader beside
   the old fields; fixtures and `docs/homing.md`. Depends on nothing.
1a. `refactor/node-legs`: a base leg on the node and the Pico, with seek,
   retract and sweep built on it; leg names instead of homing names. Same
   behaviour. Depends on nothing.
1b. `feature/node-park-leg`: the park leg and leg abort, and the `leg` verbs
   replacing `lin_leg`/`rot_leg`. Depends on 1a.
2. `feature/homing-legs`: parallel legs with per-cycle failure, the park leg,
   node-addressed `setorigin`. Depends on state-handling branch 3 and 1b.
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

Done, merged in ddeacb8. Unblocks branch 3 (`feature/home-command`, with
branch 2) and coordinate-system.md branch 1 (`feature/pico-frames`).

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

## Branch 1a: `refactor/node-legs`

### Plan

* **Type:** `refactor`. **Depends on:** nothing. Existing tests pass
  unchanged; nothing changes on the wire.
* **Purpose:** legs stop being a homing-only thing. A base leg owns the
  mechanics; seek, retract and sweep are modes on it, so branch 1b adds park
  as one more mode.
* **Node** (`src/node/types/stepper/`):
  * New `leg.{h,cpp}`: the pulser ISR, ramp, budget, arm, halt, finish, span
    and busy flag, from `stepper.cpp:106-181`, `:300-518`. Each mode supplies
    its terminator (checked before the step) and finish hook.
  * Seek and retract (switch debounce, latch write) and sweep (Hall sample
    and resolve) stay homing code, built on the base.
  * Renames: `HomingState` → `LegState`, `homingActive` → `legActive`,
    `homingLegArm` → `legArm`, `homingHalt`/`homingFinish` → `legHalt`/
    `legFinish`, `homingSpan*` → `legSpan*`.
* **Protocol** (`include/common.h`): `NODE_FLAG_HOMING` → `NODE_FLAG_LEG`,
  same bit; `CMD_HOME_LEG` keeps its name and opcode.
* **Pico:**
  * New `core0/ops/leg.{h,cpp}`: claim, poll, settle, deadline and the
    outcome, from `core0/ops/homing.cpp`. `homing.cpp` keeps the session
    (`STATE_HOMING`, `homingReason`, `ALARM_HOMING_FAIL`) and maps leg
    outcomes to it.
  * `HOMEFAIL_*` → `LEGFAIL_*` (`homing.h`, `query.cpp:34`). The `homefail=`
    field in `?` and `ALARM_HOMING_FAIL` keep their names: they belong to the
    session.
  * Callers: `core0/cmd/axis.cpp` (`cmdLinLeg`, `cmdRotLeg`),
    `core0/ops/probe.cpp`, `core1/rpc_server.cpp:47`.
* **Web:** none (no wire change).
* **Checks:** `pio run` for every stepper env, `pio run -e pico`,
  `pio test -e native`.
* **Human scope:** one seek/retract on X and one sweep on A, unchanged.

### Status

Done, ready to merge (d73fe25). Unblocks 1b.

### Outcome

* Modes are a `LegMode` enum switched on inside the one TCA0 ISR, not
  per-mode hooks: a call the compiler cannot see would make the ISR save
  every call-clobbered register. ISR prologues are unchanged (18 pushes
  linear, 28 rotary; RX ISR 16); `db_node1` flash is byte-identical.
* `stepper_state.h` (new) shares the counter, DIR and limit gate between
  `stepper.cpp` and `leg.cpp`. `HAS_HOMING` keeps its name: park and abort
  stay under it in 1b (a node without a terminator never has a datum).
* Pico: `legArm` / `legPoll` / `legDrop` return a `LegEnd` (node, dummy,
  rotary, retract, `LEGFAIL_*`); `homing.cpp` turns it into the session,
  the latch and the origin drop. The leg's state is one `Leg` struct, for
  branch 2 to make per node. The dummy leg is a leg (`legArmDummy`).
* `rpcHome` → `rpcHomeLeg`. `probe.cpp` was not a caller: probe legs are
  stream-driven.
* Bench: X seek (span 25241, latched) and retract (span −320, clear), A
  sweep (`idxcause ok`, cross 3, steprev 16605), `kind_mismatch` and the
  dummy failure, all as before.

## Branch 1b: `feature/node-park-leg`

### Plan

* **Type:** `feature`. **Depends on:** 1a. Protocol change: lands before
  branch 2, which uses both commands.
* **Purpose:** the park leg and leg abort on the node, and the `leg` verbs on
  the Pico. Nothing sequences them yet.
* **Protocol** (`include/common.h`, beside `CMD_HOME_LEG` at `:135-190`):
  * `CMD_PARK_LEG` 0x25, payload 10 bytes big-endian: `[0..3]` target
    (signed node counter), `[4..5]` start interval µs, `[6..7]` floor
    interval µs, `[8..9]` ramp steps. Ack: the status payload, as
    `CMD_HOME_LEG`. Direction and step count come from target − counter; no
    budget field, the distance is the budget.
  * `CMD_LEG_ABORT` 0x26, no payload. Ack: the status payload. Aborting with
    no leg running is an ack, not a NAK (idempotent, so fail-all needs no
    state check).
  * NAKs: `NAK_BUSY` (a leg runs), `NAK_BAD_ARG` (as `homingLegArm`), and a
    park leg without `NODE_FLAG_DATUM` is refused (new `NAK_NO_DATUM`): a
    counter with a broken witness is not a position.
* **Node** (`src/node/types/stepper/`, on 1a's `leg.{h,cpp}`):
  * Park mode: the base leg gains a ramp-down over the last `rampSteps`
    (mirror of the ramp up, so the stop is at the start rate). Its terminator
    is a debounced switch, as a seek; a stop on the switch sets
    `limitLatched`, so the gate still holds. No Hall sampling, no latch clear.
    Target == counter arms nothing and acks.
  * Abort: `legHalt()`, then the loop's `legFinish` closes the span. The
    motor stays enabled; `NODE_FLAG_DATUM` is untouched.
  * Handlers in `node_handle_command` beside `CMD_HOME_LEG`, under
    `HAS_HOMING`.
* **Pico:**
  * `ipc/core1_rpc.{h,cpp}`: `rpcParkLeg`, `rpcLegAbort`;
    `core1/rpc_server.cpp:47` answers both with status.
  * `core0/ops/leg.{h,cpp}`: a park leg uses the same claim, poll and
    deadline; it succeeds when the node stops at the target, and fails with a
    new `LEGFAIL_PARK` otherwise. One node at a time, as today; branch 2 makes
    it per node.
  * `core0/cmd/axis.cpp`: `leg <node> seek|retract|sweep|park …` and
    `leg_abort <node>` replace `cmdLinLeg`/`cmdRotLeg`; `control_plane.cpp:60`
    table. Seek and retract set the intent bit, so a mismatch NAKs as today.
    `park` needs the node homed on the Pico (`originValid`), so the target is
    in the node's frame as `setorigin` left it.
* **Web:** `web/src/wire/link/commands.ts` (`:320-349`): `linLeg`/`rotLeg`
  move to the `leg` verbs; `parkLeg` and `legAbort` added;
  `web/src/homing/sequence.ts` and `web/src/wire/link/backends/sim.ts`
  follow. No homing behaviour change.
* **Docs:** `docs/homing.md`: §1.4 the park leg and the abort, and the bench
  commands renamed.
* **Checks:** `pio run` for every stepper env with `HAS_HOMING`,
  `pio run -e pico`, `pnpm typecheck` and `pnpm test` in `web/`.
* **Human scope:** park to a target and back on X and A, a park run into the
  switch, abort mid-seek (motor stays energised, `datum` flag kept, counter
  matches a `getpos`).

### Status

Planned.

## Open questions

* Guarding earlier-cycle axes while later legs run (still enabled and unmoved
  since their leg ended, plus still latched where there is a switch), and a
  minimum seek travel to catch a stuck switch. None of grbl, Marlin or Klipper
  does either.
* FluidNC's `cycle: 0` (homed only by name, never by a plain `home`).
* The homing recipe on the Pico: the controller sequence that holds the
  session, and the handler bodies (`setorigin`'s datum loop, the enables) that
  move into ops for it. The probe session may later follow the same shape.
