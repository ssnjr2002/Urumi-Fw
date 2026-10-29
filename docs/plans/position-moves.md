# Position moves

How the operator moves the machine once it is homed: homing scopes, relative
and absolute jogs, named positions, and the speeds of each. Builds on
docs/plans/coordinate-system.md (frames, work offset, derived selection) and
docs/plans/motion-sessions.md (jog, `jogto`, continuous jog, which this
supersedes in part).

## Decisions

### Homing scopes

* Existing: `home [only] [<node> …]`, `home_unhomed`, `home_cycle <k>`,
  `home_head <n>` (docs/homing.md §2.8).
* **`home_xy`** is a convenience: `home <X node> <Y node>`, nodes from the
  config's `x` and `y`. Without `only`, so earlier cycles (the Zs) park or home
  first, as `home` does (see Checked).

### Command shape

```
jog   <x|y|z|a> <d> [scale]               relative, one axis
jog   xy <dx> <dy> [scale]                relative, XY line
goto  <m|w> <x y | z | a> <pos> [scale]   absolute, machine or work
goto  park | load | mount | up | unlock [scale]
```

* `goto` replaces `jogto`. `jogto` is removed, not aliased; the web host has no
  caller.
* Relative moves have no frame: work = machine − offset, so a distance is the
  same in both.
* `cjog` (`CJOG_MAGIC`) is unchanged.

### What each move does

| Move | Axes | Z first | Runs as |
|---|---|---|---|
| `jog x\|y\|xy` | anchor | no | planner line |
| `jog z\|a` | selected head's Z or A | no | planner (`plannerQueueAxis`) |
| `goto <m\|w> x y` | anchor | every Z parks | legs, then planner line |
| `goto <m\|w> z\|a` | selected head's Z or A | no | planner |
| `goto up` | every present Z, to machine park | — | park legs |
| `goto unlock` | every present A, to machine 0 | no | park legs |
| `goto park\|load` | anchor, stored position | every Z parks | legs, then planner line |
| `goto mount` | anchor, stored position; the selected A to unlock along the line | every Z parks | legs, then planner line |
| `probe_park <tool>` | as `mount`, the target the tool's switch | every Z parks | legs, then planner line |

* **Absolute XY is the travel move** and always parks Z. Relative XY is the
  nudge near the work and never does.
* **Machine A is not folded**, so `unlock` and a machine-A `goto` unwind the
  way A turned. A work-A target (the edge heading) picks the turn that stays
  within `aMaxDeg` of machine 0, not the shortest.
* **Gates:** every `goto` needs its axes homed, `jogUnhomed` or not. A work
  target needs that axis's work offset (`err no_work`, e.g. after a tool
  change before `wzero z`). Relative `jog` keeps today's `jogUnhomed` rule.
  Soft range as today (machine range, XY shrunk by the blade offset).

### Speeds

* **One rule for every axis:** `jogFeed` × scale, capped at `maxFeed`, planner
  or leg alike. Homing's seek, back-off and latch keep the homing block's
  speeds and `rampSteps`.
* **Every park leg** (`up`, `unlock`, the Z park in `goto`, the park ending a
  homing cycle) runs at `jogFeed` × scale (homing passes 1), ramping from the
  homing `startFeed`, with its ramp worked out from acceleration:
  steps = (v² − v₀²) / (2 · maxAccel · parkAccelScale) × stepsPerUnit.
* **`parkAccelScale`:** new optional per-axis config field, (0, 1], default 1.
  Legs have a linear ramp, not the planner's plan; this lets a motor that
  stalls on it park gentler without lowering its planner `maxAccel`.

### Admission and replies

* **`goto` is a sequence** (`K_SEQUENCE`, IDLE only) and replies `ok` once it
  starts, as `jogto` today; it never joins queued clicks, so there is no
  depth. `jog` keeps `K_STEP` and `ok <depth>`.
* **JOB.TOOL_WAIT** admits the named forms only (`goto up|unlock|mount`, `unmount`, and
  `probe_park` as today) beside relative `jog`. The coordinate forms are not
  needed there: a tool change clears the work Z, so `goto w z` would refuse
  anyway.

### Holder lock

The tool holder on a head's A releases only near machine A 0.

* **Unlocked:** |machine A| ≤ `unlockRange`; the tool can come out.
  **Locked:** outside it. Going to machine 0 unlocks.
* **`unlockRange`:** optional head-block config field, degrees. Absent: the
  head has no interlock, no lock status, and `mount` does not turn A.
* **`unlocked=`** status mask, bit h = head h. Derived on each read from the
  head's A node position and origin (as `head=` is derived from the map), so
  it needs no mapping: set when that A is homed and inside the range; clear
  when unhomed (its homed bit says the state is unknown).
* **`unmount <head>`:** each step skipped if already true: tool → 0 (clears
  the head's work Z, as `mount`), that head's Z parks, that head's A unlocks.
  A sequence of legs.
* **`mount <head> <tool>`** turns that head's A out of the unlock range,
  locking it, when the head has `unlockRange`.
* **Tool change:** the stop unmounts the head about to cut only. Other heads
  stay as they are, so a holder not in use stays locked and can't be emptied
  by mistake; the operator `unmount`s a head to load it.

### Checked

* `home_xy` lifts Z first: with no `cycle` in the config, Z defaults to cycle
  1 and everything else to 2 (`config_decode.h:39`). A shares X and Y's cycle,
  so it is not swept.
* Park legs on a rotary node: the node's `CMD_PARK_LEG` needs only the datum
  (the switch check is `HAS_LIMIT_SWITCH` only, `stepper.cpp:402`), and the
  Pico's leg end accepts a rotary park without the LIMIT test
  (`ops/leg.cpp:356`). The tool-change stop already runs them (`lockAs`,
  `controller/seq/steps.cpp:85`, every head's A to machine 0 at `jogFeed`).

## Branches

Order: G1 and G2 are independent; G3 lands before G4; G5 after G1 and G4.

## Branch G1: `fix/jogto-mount-alock`

### Plan

* Type: fix.
* Purpose: `jogto mount` turns the selected head's A to unlock (machine 0)
  along the line, as `probe_park` does; `err a_not_homed` when that A is mapped and
  unhomed.
* Files: `src/rp2350/core0/controller/cmd/jogto.cpp` (`cmdJogTo` passes
  `aLock` for `mount`, :42; header comment), `controller/seq/jogto.h` (comment),
  `docs/wire_protocol.md` (`jogto` row :504, `a_not_homed` in its replies).
  New wording ("to unlock") in what G1 touches; the other rows wait for G5.
* Depends on: nothing.
* Test: bench (no native test covers command handling).

### Status

Done, merged 2026-10-29 (`ec7ce88`). Unblocks G5 (with G4).

### Outcome

* `jogto mount` unlocks only the selected A along the line; the tool-change
  stop still unlocks every A by legs (`lockAs`) until G5.

## Branch G2: `feature/home-xy`

### Plan

* Type: feature.
* Purpose: `home_xy`, as in Homing scopes. `err usage` when neither X nor Y
  is homeable; otherwise replies as `home`.
* Files: `src/rp2350/core0/controller/cmd/home.cpp` (`cmdHomeXY` beside
  `cmdHomeHead`, :105), `controller/cmd/table.h`, `control_plane.cpp` (row
  after `home_head`, :80), `docs/wire_protocol.md` (command row :491,
  admission table :654), `docs/homing.md` (§2.8, :863),
  `web/src/wire/link/commands.ts` (`homeXY` beside `home_head`, :518), the
  sim backend if it models `home_head`.
* Depends on: nothing.

### Status

Done, merged 2026-10-29 (`4a377c5`..`e284104`). Nothing waits on G2.

### Outcome

* Also fixed here: the web validator refused a tool's `aMaxDeg: 0` (the Pico
  reads 0 as no cable), so `config/controller.jsonc`'s `crease` tool failed
  the web config tests; the shared `controller.msgpack` fixture is re-encoded
  and the Pico decode test expects the third tool.
* Out of scope: a tool's `unwind` field is read by the web loader but used
  nowhere, and the Pico never decodes it; `aMaxDeg > 0` alone makes a cable.
  Follow-up: remove it from the schema, loader, defaults, `tools.ts` and
  `config/controller.jsonc`.

## Branch G3: `feature/park-speed`

### Plan

* Type: feature.
* Purpose: park legs at jog speed with an acceleration-derived ramp, as in
  Speeds; the `parkAccelScale` config field. `lockAs` already parks at
  `jogFeed`; it gains only the derived ramp.
* Files:
  * Pico: `controller/seq/steps.cpp` (`parkZs` :105, `lockAs` :85; a shared
    park-leg helper), `controller/seq/home.cpp` (cycle-end park :94, :379),
    `config/config_decode.{h,cpp}` (field, range check), config version bump
    if the blob changes.
  * Web: `web/src/machine/schema.ts`, `machine/json/load.ts`, the validator,
    the config blob encoder.
  * Docs: `docs/homing.md` (leg numbers, §2.8), the config schema doc.
* Depends on: nothing, but lands before G4, whose `up` and `unlock` use the
  helper.
* Test: native config tests for the field; motion on the bench.

### Status

Planned.

### Outcome

## Branch G4: `feature/goto`

### Plan

* Type: feature (protocol change).
* Purpose: `goto` replacing `jogto`, `jog xy`, `up` and `unlock`, scale on every
  form, as in Command shape and What each move does.
* Files:
  * Pico: `controller/cmd/jogto.cpp` → `goto.cpp` (parse, frames,
    named), `controller/seq/jogto.{h,cpp}` (Z park optional, scale, the
    `up`/`unlock` sequences), `cmd/axis.cpp` (`jog xy`, `queueJog` :685,
    `queueAxisJog`), `ops/frames.cpp` (machine A target for a work heading;
    work target needs a valid offset), `control_plane.cpp` and
    `controller/cmd/table.h` (rows), `controller/seq/toolchange.cpp` if it
    shares the recipe.
  * Web: `web/src/wire/link/commands.ts` (`goto`, `jog xy`),
    `wire/format/status.ts` comment (:45), `wire/link/backends/sim.ts` (:113).
  * Docs: `docs/wire_protocol.md` (`jog`, `jogto` → `goto`, admission
    table), `docs/plans/motion-sessions.md` (point Jog command shape here).
* Depends on: G1, G3.

### Status

Planned.

### Outcome

## Branch G5: `feature/unmount`

### Plan

* Type: feature (protocol change).
* Purpose: the holder lock, as in Holder lock: `unlockRange`, the derived
  `unlocked=` status, `unmount`, `mount` locking A, the tool-change stop
  unmounting only the head about to cut. Renames the remaining "lock" wording
  to "unlock" (`lockAs` → `unlockAs`, `aLockReady`, `jogtoStart`'s `aLock`,
  the `probe_park` and tool-change rows).
* Files:
  * Pico: `controller/cmd/mount.cpp` (`unmount`, `mount` locks A),
    `controller/seq/toolchange.cpp` (:49, one head), `controller/seq/steps.cpp`
    (`unlockAs`, a one-head form), a sequence for `unmount`,
    `control_plane.cpp` and `controller/cmd/table.h` (row),
    `cmd/get.cpp` (`unlocked` key, :189), the status reply,
    `config/config_decode.{h,cpp}` (head `unlockRange`).
  * Web: `web/src/machine/schema.ts`, `machine/json/load.ts`, the validator,
    the config blob encoder; `wire/format/status.ts` (`unlocked`),
    `wire/link/commands.ts` (`unmount`), `wire/link/backends/sim.ts`.
  * Docs: `docs/wire_protocol.md` (`unmount`, `mount`, `probe_park`, the
    tool-change stop :263, status keys), the config schema doc.
* Depends on: G1, G4 (`goto unlock`, the shared recipe).
* Open: which way, and how far, `mount` turns A out of the range (away from
  the cable's wound side keeps the full `aMaxDeg`); a config value or a fixed
  margin past `unlockRange`.

### Status

Planned.

### Outcome
