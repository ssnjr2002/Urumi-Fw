# Runtime config edits

The operator changes config values the Pico uses while it runs (positions
first), sees them take effect at once, and saves them to `/config.bin` when
satisfied. The next boot loads the saved values.

## Decisions

* **Only fields the Pico uses are editable**, and only those listed in a
  field table. A field without a row cannot be set: node ids and types, head
  count, tool ids and peripherals stay host-only (`CFG_SET`).
* **One decoded config in RAM, plus a dirty flag.** Every consumer reads it;
  `set` edits it in place. Dirty is set by an accepted `set` and cleared by
  `save`, an adopted `CFG_SET` and the reload below; `cfg` and the `cfgdirty`
  key of `get` show it. The binary `STATUS_RSP` does not carry it.
* **There is no `revert`; `reset` restores the saved config.** Unsaved edits
  are lost at power-off and at a soft reset. If dirty, the soft reset decodes
  `/config.bin` again, republishes the buffer times and reapplies `meshOn`
  before the boot sequence. A clean reset touches neither flash nor a mesh
  state set by `mesh on/off`. A failed reload leaves no valid config, as a
  failed boot does. One bad value is undone by another `set`, with no rehome.
* **The field table is host data, the Pico's copy is generated.**
  `web/src/machine/runtime.ts` holds `RUNTIME_FIELDS`: path, C++ member, `has`
  member, type, min/max, homing kind, effect names. A row is a template over
  its scope (machine, axis, head or tool), not one concrete field. A path is
  the blob's own path, `machine.` included: `machine.x.maxFeed`,
  `machine.heads.0.z.maxFeed`, `toolProfiles.knife.cutFeed`. Paths are typed
  against `ControllerConfig` (`PathOf<ControllerConfig>`), so `pnpm typecheck`
  catches a wrong path; the generated header names the C++ member, and a
  compile-time check over its rows makes `pio run -e pico` catch a wrong
  member. The web UI and `validate.ts` read the same ranges.
* **Row details.**
  * A homing row is marked linear, rotary or both. An axis is in one of three
    states (no homing, linear, rotary), read from `homing.present` and
    `rotary`; one without homing rejects every homing row.
  * A point (`machine.positions.park`, `.load`, `.mount`, a head's
    `probeSwitch`) is one row of type `point`, set as
    `set config.machine.positions.park 100 200`. `probeSwitch.z` is its own
    optional row; `toolOffset` is two rows, being always present.
  * `min` and `max` are optional and inclusive; `minExclusive` covers fields
    that must be above a bound. Checks across fields stay rules.
* **The generated header is committed.** `pio run` never needs pnpm. A vitest
  test regenerates it in memory and fails if it differs from the committed
  file, ignoring line endings (`core.autocrlf` checks it out as CRLF), so
  `pnpm test` (Agent Scope for `web/`) is the up-to-date check.
* **Tools are addressed by name**, as `toolProfiles` keys them:
  `set config.toolProfiles.knife.cutFeed 25`. `CfgTool` keeps its blob key in
  `char name[16]`, and parse rejects a longer one. The id stays for the wire
  (`configTool`).
* **Validation is organised by rule, not by field.** Parse and validate are
  split: `configParse` builds the struct, `configValidate` runs rules on it.
  A rule is a plain function over `MachineCfg`. Every rule runs at load and
  on each `set`: the config was already valid, so only the new value can fail
  one. Per-field min/max come from the generated table and run first.
* **`MachineCfg` follows the blob.** Every decoded field has the blob's name
  and nesting, except that `toolProfiles` is an array found by id, the
  `machine` wrapper and `v` are not mirrored, and `homing.kind` is not stored.
* **Optional fields carry a `has` flag.** The member still holds the resolved
  value (0, or `cycle`'s default), so consumers ignore the flag. Save reads it;
  an accepted `set` sets it, and rollback restores it. A point is set whole:
  `set` on an absent point must supply both coordinates at once.
* **`set` and `save` share one state check:** `machineAtRest()`,
  `machineContext == CTX_IDLE` and no estop latched. That is IDLE, or ALARM
  with nothing running. A paused job and JOB.TOOL_WAIT are refused, and no
  field is editable while running.
* **Effects are organised the same way.** An effect names the fields that
  trigger it and an `apply`. The table's effect names become each effect's
  trigger set. A field with no effects is live: consumers read the struct, and
  the planner's latches are taken again at the next ring reset. There are
  three:
  * `voidDatum`: `originInvalidate` on the axis's node, and its work offset
    goes. Z unsets that head's work Z; X or Y returns both to the config's
    `work`, since they are zeroed together; A has no work offset.
  * `republishBuffer`: `bufferQueueS` and `bufferIdleMs`.
  * `applyMesh`: `meshEnable(meshOn)`.
* **`set` pipeline:** state check → range → write into `live` → all rules (on
  failure restore the old value and reject) → effects (each once). A rejected
  change leaves no trace; effects follow only an accepted one.
* **No copy of the struct on `set`: write, check, roll back.** Only the one
  field's old value is kept, on the stack, and restored if a rule fails.
  Rules read the struct as at load, with no proposed-value plumbing. Safe
  because only Core 0 reads the config (`machineCfg()` has no caller under
  `src/rp2350/core1`) and `set` runs to completion in Core 0's loop.
* **Save re-encodes the stored blob with ArduinoJson, every table field, no
  diff.** Load `/config.bin` into an unfiltered `JsonDocument`; write every
  `RUNTIME_FIELDS` field at its path; `serializeMsgPack` into the staging
  buffer; decode it again and require it to equal the RAM config;
  `configStoreCommit`; clear dirty. No soft reset, no effects (nothing in use
  changes). The result is value-identical, not byte-identical: ArduinoJson
  writes a double that fits a float32 as a float32. Write-back rules:
  * Only table fields are written; host-only and non-editable fields are
    untouched.
  * Absent stays absent: an optional field is written only when its `has`
    flag is set and removed otherwise.
  * A whole number is written as an integer, as the host wrote it.
  * A float is written as its shortest round-trip decimal, as a double:
    `snprintf("%.*g")` at increasing precision until `strtof` gives the same
    float, then `strtod`. `0.10000000149f` saves as `0.1`. No library.
* **Every table field fits a float32 exactly, so a save is lossless.** The
  host encodes non-integers as float64 (`@msgpack/msgpack` default,
  `web/src/machine/json/blob.ts:18`); the rule makes the shortest round-trip
  of the Pico's float equal the host's double.
  * `f32(v)` (`web/src/machine/f32.ts`): the shortest decimal with the same
    float32. A value fits iff `f32(v) === v`.
  * Values from the config file that do not fit are rejected by `validate.ts`
    ("more precision than the controller keeps"); `set` rejects typed text
    the same way.
  * Values the host computes are wrapped in `f32()` where they are computed,
    if they are table fields. None is today: `chamberOffsets` is not a row. A
    test runs the fit check over every table field in `DEFAULTS` and the tool
    presets.
* **`CFG_SET` keeps its full soft reset** and discards `live` edits. The host
  should warn when `cfgdirty` is set before pushing. After a save `seq` moves;
  the host pulls again.

## Branch 1: `refactor/config-validate`

**Plan**

* Type: refactor. Same accepted and rejected blobs, same `CfgDecodeError`
  for each existing fixture.
* Purpose: split `configDecode` (`src/rp2350/core0/config/config_decode.cpp`)
  into `configParse` and `configValidate(const MachineCfg&)`, with the
  cross-field checks as an ordered rule table. Shape the struct to follow the
  blob, e.g. `positions.{park,
  load,mount}` nested as in the blob instead of flat in `MachineCfg`
  (`config_decode.h:136`). Optional fields keep their absence: `cycle`,
  `unlockRange` and the tool's optional scalars get `has` flags.
* Files:
  * `src/rp2350/core0/config/config_decode.{h,cpp}`
  * `src/rp2350/core0/config/machine_cfg.{h,cpp}`
  * consumers of renamed members (`machineCfg()` callers under
    `src/rp2350/core0/`: `controller/`, `ops/`, `planner/queue.cpp`)
  * `test/test_config/test_config_decode.cpp`
* Depends on: nothing.
* Checks: `pio run -e pico`, `pio test -e native`.

**Status:** done, merged to `main`. Unblocks branch 2.

**Outcome**

* Deviations: `set` runs every rule, so rules have no reads set. `MachineCfg`
  follows the blob in full, not only where the table needs it. `cycle` keeps
  its resolved value beside `hasCycle`, so its default stays in the parser.
* `unlockRange` moved from parse to a rule. Checks guarding narrowing or
  reading raw blob data (`homing.kind`, laser node, counts) stay in parse.
* For later branches: `configParse`, `configValidate`; has flags `hasCycle`,
  `hasUnlockRange`, `hasAMaxDeg`, `hasAOffsetDeg`, `hasOffsetMm`, `hasCutFeed`,
  `hasPlungeFeed`, `hasDutyLimits`, `probeSwitch.hasZ`. Branch 3 needs a form
  of `set` that supplies a whole point when it is absent.

## Branch 2: `feature/runtime-fields`

**Plan**

* Type: feature.
* Purpose: the field table and its generator.
  * `web/src/machine/runtime.ts`: `RUNTIME_FIELDS` and the `PathOf` type.
  * `web/scripts/runtime-fields.ts`: writes the X-macro header (path, member,
    type, min, max, effect names), run as `pnpm gen:runtime`.
  * The generated header under `src/rp2350/core0/config/`, committed.
  * A vitest test that the committed header is current.
  * `validate.ts` uses the table's ranges for those fields, and rejects a
    table field that does not fit a float32.
  * `web/src/machine/f32.ts`; a test of the fit over `DEFAULTS` and the tool
    presets.
  * `CfgTool.name`, decoded from the `toolProfiles` key.
* Starting rows:
  * Machine: `positions.*`, `feed.*`, `buffer.*`, `zClearance`, `maxOvercut`,
    `meshOn`.
  * Axis: `stepsPerUnit`, `invertDir`, `maxTravel`, `maxFeed`, `maxAccel`,
    `jogFeed`, `jogFeedUnhomed`.
  * Linear homing: `startFeed`, `seekFeed`, `latchFeed`, `rampSteps`,
    `seekScaler`, `backoffDist`, `pullOffDist`, `parkPos`.
  * Rotary homing: `startFeed`, `sweepFeed`, `rampSteps`, `budgetRevs`,
    `toleranceDeg`, `indexPos`.
  * Head: `xOffset`, `yOffset`, `probeSwitch`, `unlockRange`.
  * Tool: `cutFeed`, `plungeFeed`, `toolOffset`, `offsetMm`, `aOffsetDeg`,
    `aMaxDeg`, `cornerAngleDeg`.
  * Not rows: `softLimits`, `jogUnhomed`, `seekPositive`, `homing.cycle`,
    `work`, `chamberOffsets`, `power.*`.
* Files also touched: `src/rp2350/core0/config/config_decode.{h,cpp}` (name,
  compile-time check), `web/package.json`.
* Depends on: branch 1 (the header names the reshaped members).
* Checks: `pnpm typecheck`, `pnpm test` in `web/`; `pio run -e pico` with the
  header included.

**Status:** done, merged to `main`. Unblocks branch 3.

**Outcome**

* Deviations: paths are typed per scope (`PathOf<MachineConfig>`,
  `PathOf<AxisConfig>`, `PathOf<ToolHead>`, `PathOf<ToolProfile>`), not by one
  `PathOf<ControllerConfig>`, whose `toolProfiles` record would accept any tool
  field. `runtimeValues` builds the full blob path from the scope. The branch
  also renames `PipelineConfig` to `ControllerConfig`.
* Added files: `web/src/machine/runtimeHeader.ts` holds the header renderer, so
  the test can call it without running the script; the header is
  `src/rp2350/core0/config/runtime_fields.h`.
* Behaviour: `validate.ts` now requires table fields to be finite and fit a
  float32, and checks fields it used to pass through (`startFeed`,
  `seekScaler`, the distances). Range errors always end in `(got x)`. Error
  messages keep the config file's spelling (`tools.knife`, `heads[0]`).
* For branch 3: `RUNTIME_FIELDS(REQUIRED, OPTIONAL)` is the X-macro, and
  `CfgTool.name` the tool's address. Effect names (`voidDatum`, `applyMesh`,
  `republishBuffer`, `refreshFrames`, `refreshTool`) and which rows trigger them
  are provisional: `voidDatum` on `pullOffDist` and `parkPos` especially. The
  audit may change them by regenerating. `voidDatum` is axis-scoped, replacing
  the four per-axis names. Table limits apply to `set` only; at load the Pico's
  rules still allow values the table refuses (`budgetRevs` below 3.2).
* Out of scope: `feature/park-speed` overlaps `schema.ts`, `validate.ts`,
  `load.ts` and `config_decode.{h,cpp}`; if it merges, `parkAccelScale` needs
  a row. Not run on the machine: nothing on the Pico reads the table yet.

## Branch 3: `feature/runtime-config`

**Plan**

* Type: feature.
* Purpose: the Pico side.
  * Dirty flag in `machine_cfg.{h,cpp}`, and the reload at a dirty soft reset.
  * Effects: `voidDatum`, `republishBuffer`, `applyMesh`. `refreshFrames` and
    `refreshTool` leave `RUNTIME_FIELDS` and the regenerated header: at rest
    they have nothing to do.
  * The `set` pipeline with rollback; the float-fit check on typed values.
    New files: `config/runtime_edit.{h,cpp}` (pipeline and write-back, free of
    Arduino I/O), `cmd/config.cpp` (handlers and effects).
  * Rules the host has and the Pico lacks, so `set` cannot save a config the
    host refuses: `latchFeed < seekFeed`, `seekFeed <= maxFeed`,
    `sweepFeed <= maxFeed`. A `maxFeed` of 0 is uncapped.
  * Text commands `set config.<path> <value>`, `get config.<path>`, `save`;
    `cfg` shows dirty, as does `get cfgdirty`. `set` and `save` are controller
    commands, refused without a config. `<path>` is the table's exact blob
    path, camelCase included; command words stay lowercase, so
    `docs/wire_protocol.md` changes to say so.
  * Save path: unfiltered load, write every table field (shortest
    round-trip floats, integers as integers, absent removed), encode,
    re-decode check, commit without the soft reset.
  * Docs: `docs/config_storage.md` (save path, reload), `docs/wire_protocol.md`
    (commands, `cfgdirty`, `reset`).
* Files also touched: `web/src/machine/runtime.ts`,
  `src/rp2350/core0/config/runtime_fields.h`, `config_decode.cpp`,
  `core0.cpp`, `control_plane.cpp`, `cmd/query.cpp`, `cmd/get.cpp`.
* Depends on: branches 1 and 2.
* Checks: `pio run -e pico`, `pio test -e native`, `pnpm typecheck`,
  `pnpm test` (pipeline and save round-trip under native where they are free
  of Arduino I/O; a save of an unedited config decodes to equal values). Human:
  set a position and use it, save, power-cycle, confirm it loaded; set a
  `voidDatum` field and confirm the datum is void; edit, `reset`, confirm the
  saved value is back; host `CFG_GET` after a save loads in the web host.

**Status:** done, merged to `main`. Unblocks branch 4.

**Outcome**

* Deviations and details:
  * The handlers are in `controller/cmd/config.cpp`, not `cmd/config.cpp`.
  * Setting the value a field already holds does nothing: no dirty flag, no
    effects.
  * Added `configStoreCrc` and `framesResetWorkXY`.
  * `get` prints a point as `x,y`; `set` takes `x y`.
  * `probeSwitch.z` on a head with no probe switch answers `err absent`.
  * `maxTravel` keeps its unconditional `voidDatum`.
  * `set meshOn` overrides `mesh on|off`, and a dirty `reset` reapplies the
    stored value.
* For branch 4: `dirty=1` is in `cfg` and `cfgdirty` in `get`, not in
  STATUS_RSP. `CFG_SET` over unsaved edits clears dirty. Saved blobs are
  value-identical to the source, not byte-identical.
* Bench, on the machine with no tools mounted: everything in Checks passed,
  and `set` and `save` answer `err busy` in a jog and `err bad_state` in estop.
  A rebooted Pico stood in for a power-cycle. The saved blob was pulled with
  `CFG_GET` and read with the host decoder, not through a live web connection.
  The paused job refusal is untested; it shares the estop check.
* Peak RAM of `save`, 2.8 KB config: 14.1 KB of JSON documents against 413 KB
  free heap, no leak over 10 saves. About 1.5 KB of stack is left at its
  deepest (2.35 KB used of 3.9 KB free on entry), so stack is the limit to
  watch.
* Out of scope:
  * `save` with nothing changed still rewrites flash and bumps `seq`.
  * The host's `probe.seekFeed` rule compares with the now editable `maxFeed`
    if a config has that block. `config/controller.jsonc` has none, and the
    block looks dead in `web/src`, so it may be removable.

## Branch 4 (optional): `feature/runtime-config-web`

**Plan**

* Type: feature.
* Purpose: a web panel built from `RUNTIME_FIELDS`: editable fields, ranges,
  "voids homing" badges, dirty indicator, save, each field's saved value to
  set back, warn before `CFG_SET` or `reset` while dirty, pull after a save.
* Files: `web/src/` (settled in Read).
* Depends on: branch 3.
* Checks: `pnpm typecheck`, `pnpm test`.

**Status:** not started.

## Open questions

* Effects that need the old value (move a datum by the delta instead of
  voiding it): void for now.
