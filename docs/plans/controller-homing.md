# Controller homing

How the Pico homes a dual-head machine: parallel legs, cycles, the `home`
controller command, node-addressed `setorigin`, and the axis direction config
homing derives from. Builds on the homing session in
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
* **A switch-end leg:** the node moves to its switch end using its datum. It
  is a leg, not a plain move, so the session still allows no motion outside
  legs.

### Cycles

* Each axis has a `cycle` number in the config (as FluidNC); lower runs
  first, equal numbers run in parallel. Default: every Z 1, everything else 2.
* **Earlier cycles clear first.** Before any axis of cycle k homes, every
  axis of every earlier cycle is made clear, named or not: homed goes to its
  switch end (switch-end leg, datum kept), unhomed is homed. A run naming only
  first-cycle axes moves nothing else.
* **Clear is the switch end**, the one place every axis reaches by homing.
  For Z it is 0 (see Axis direction config).
* **Uncertain** (mute, excluded, or failed its leg) in an earlier cycle fails
  the session before any later leg runs, naming the axis.
* The rule lives in the controller recipe (only the config knows the cycles);
  the leg primitive stays unguarded, the raw path for the bench.

### `home`

* A controller command: `home [unhomed] [solo] [<axis> …]`. Lowercase only
  (the control/data plane mux relies on it).
* **Axes:** `x`, `y`, `z<n>`, `a<n>` with `n` the index into the config's
  `heads[]` (the number `select` takes); `z` and `a` alone mean every head's.
  No axes means all. An unknown name, a head index out of range, or an axis
  the head lacks is `err usage` before anything moves.
* **Named axes** are re-homed (the default); with `unhomed`, only the unhomed
  ones are.
* **`solo`** homes the named axes without clearing any earlier cycle: the
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

### Axis direction config

* Two raw direction bits per axis, each found by jogging on the machine:
  * `switchDir`: the raw direction that drives the axis into its switch (or
    toward the index, for a rotary axis).
  * `originDir`: the raw direction toward the end chosen as 0.
* Derived: the switch is at 0 when `switchDir == originDir`, else at
  `hardTravel`; positive coordinates run in raw `!originDir`; the approach
  direction is `switchDir`. An axis without homing has `originDir` only.
* **Z has no `originDir`:** it is `switchDir`, so Z's switch end is 0. Z can
  only seek up (with current feedback too, the bed is the other end), so 0 is
  the top and the safe height by definition. Positive Z runs down, away from
  the switch. The config marks which axes are Z.
* Replaces `atOrigin` and `invert`, whose XOR gave the approach direction and
  whose meaning depended on a chosen frame.
* "Raw" is the direction bit the Pico sends, in the stream and in
  `CMD_HOME_LEG`. Inversion in a node or driver is wiring, captured by the
  jog.
* Choosing `originDir` per axis can mirror the frame (a left-handed X/Y).
  Firmware cannot see that; commissioning checks it (cut an asymmetric shape).

### Dropped

* Slot-framed `setorigin`: assumed every slot always has a node, which dual
  heads break.
* A plain move inside the homing session to park Z: the switch-end leg
  instead.
* Cycles as ordered groups (`[[z], [x, y, a]]`): a per-axis number instead.
* `ignore` (skip only uncertain axes, clear the rest): `solo` skips all
  clearing.
* ISO 841 Z (+ up, Z in `[-hardTravel, 0]`), natively or as a sign flip in
  the host UI: every Z input and output would need the flip. Z stays
  positive-down; the safe height is 0 either way.

## Wire changes

* Commands: `home`; `setorigin` node-addressed syntax.
* Config: `switchDir` and `originDir` replace `atOrigin` and `invert`;
  per-axis `cycle`.

## Branches

Proposed order. Each gets a full Plan section when its turn comes.

1. `feature/axis-dirs`: `switchDir` and `originDir` replace `atOrigin` and
   `invert` (`web/src/machine/schema.ts`, the homing derivation and its tests,
   `web/demo/comms.json`, the Pico's `config_decode`, `docs/homing.md`).
   Depends on nothing.
2. `feature/homing-legs`: parallel legs with fail-all, the switch-end leg,
   node-addressed `setorigin`. Depends on state-handling branch 3.
3. `feature/home-command`: the `home` controller command and the per-axis
   `cycle` field. Depends on 1 and 2.

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
