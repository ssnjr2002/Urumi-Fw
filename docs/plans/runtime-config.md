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
  `save` and `revert`; `cfg` and status show it.
* **`revert` restores everything; there is no per-field or undo form.** It
  decodes `/config.bin` again and compares each table field with RAM. The
  gates of every differing field's effects must allow the current state, or
  the whole revert is refused. Then all fields are restored at once and each
  triggered effect runs once. Rules do not run: the stored config already
  passed them, and restoring one field at a time could fail on a mix of old
  and new values.
* **Unsaved edits are lost at power-off.** Whether they survive a soft reset
  is decided in branch 3's Read (see Open questions).
* **The field table is host data, the Pico's copy is generated.**
  `web/src/machine/runtime.ts` holds `RUNTIME_FIELDS`: path, C++ member, type,
  min/max, effect names. Paths are typed against `MachineConfig`
  (`PathOf<MachineConfig>`), so `pnpm typecheck` catches a wrong path; the
  generated header names the C++ member, so `pio run -e pico` catches a wrong
  member. The web UI and `validate.ts` read the same ranges.
* **The generated header is committed.** `pio run` never needs pnpm. A vitest
  test regenerates it in memory and fails if it differs from the committed
  file, so `pnpm test` (Agent Scope for `web/`) is the up-to-date check.
* **Tools are addressed by id.** `toolProfiles` is keyed by name, but each
  entry carries its `id`; the save path finds the entry by scanning for it.
  Rows use a tools-by-id form, e.g. `set tool.3.cutFeed 25`.
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
* **Effects are organised the same way.** An effect names the fields that
  trigger it, a gate (`allowed(state)`) and an `apply`. The table's effect
  names become each effect's trigger set. A field with no effects is live.
* **`set` pipeline:** range → gate (every triggered effect must allow the
  current state) → write into `live` → all rules (on failure restore the old
  value and reject) → effects (each once). A rejected change
  leaves no trace; effects follow only an accepted one.
* **No copy of the struct on `set`: write, check, roll back.** Only the one
  field's old value is kept, on the stack, and restored if a rule fails.
  Rules read the struct as at load, with no proposed-value plumbing. Safe
  because only Core 0 reads the config (`machineCfg()` has no caller under
  `src/rp2350/core1`) and `set` runs to completion in Core 0's loop.
* **Save patches the stored blob, every table field, no diff.** Load
  `/config.bin` into an unfiltered `JsonDocument`; write every
  `RUNTIME_FIELDS` field at its path; `serializeMsgPack` into the staging
  buffer; decode it again and require it to equal the RAM config;
  `configStoreCommit`; clear dirty. No soft reset, no effects (nothing in use
  changes). Write-back rules:
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
  * Values the host computes are wrapped in `f32()` where they are computed
    (e.g. the revolver's `chamberOffsets`, `web/src/machine/tools.ts:53`, if
    that field becomes a row). A test runs the fit check over every table
    field in `DEFAULTS` and the tool presets.
* **Save is allowed in IDLE/ALARM only**, like `CFG_SET` (flash writes stall
  XIP). `set` states are decided per effect gate.
* **`CFG_SET` keeps its full soft reset** and discards `live` edits. The host
  should warn when the status shows dirty before pushing. After a save `seq`
  moves; the host pulls again.

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
  * `web/src/machine/f32.ts`; computed defaults on table fields wrapped in
    it; a test of the fit over `DEFAULTS` and the tool presets.
* Starting rows: positions (park, load, mount), feeds and accels, jog feeds,
  `zClearance`, buffer times, `meshOn`, tool feeds and offsets, homing
  parameters, `stepsPerUnit`, `invertDir`, `maxTravel`. The list is settled
  with the user in Read.
* Depends on: branch 1 (the header names the reshaped members).
* Checks: `pnpm typecheck`, `pnpm test` in `web/`; `pio run -e pico` with the
  header included.

**Status:** not started.

## Branch 3: `feature/runtime-config`

**Plan**

* Type: feature.
* Purpose: the Pico side.
  * Dirty flag in `machine_cfg.{h,cpp}`.
  * Effect table: `voidDatumX/Y/Z/A`, `applyMesh`, tool and frame refreshes,
    with gates. Which fields trigger what is in the field table.
  * The `set` pipeline with rollback; the float-fit check on typed values.
  * Text commands `set config.<path> <value>`, `get config.<path>`,
    `revert` (decode from flash), `save`; `cfg` shows dirty.
    `<path>` is the table's exact path, camelCase included; command words
    stay lowercase, so `docs/wire_protocol.md` changes to say so.
  * Save path: unfiltered load, write every table field (shortest
    round-trip floats, integers as integers, absent removed), encode,
    re-decode check, commit without the soft reset (a sibling of the
    `CFG_SET` commit in `data_plane.cpp`).
  * Docs: `docs/config_storage.md` (save path), `docs/wire_protocol.md`
    (commands, dirty in status).
* Depends on: branches 1 and 2.
* Checks: `pio run -e pico`, `pio test -e native` (pipeline and patch
  round-trip under native where they are free of Arduino I/O; a save of an
  unedited config leaves the blob byte-identical). Human: set a
  position and use it, save, power-cycle, confirm it loaded; set a
  `rehome` field and confirm the datum is void; host `CFG_GET` after a save
  loads in the web host.

**Status:** not started.

## Branch 4 (optional): `feature/runtime-config-web`

**Plan**

* Type: feature.
* Purpose: a web panel built from `RUNTIME_FIELDS`: editable fields, ranges,
  "voids homing" badges, dirty indicator, save/revert, warn before `CFG_SET`
  while dirty, pull after a save.
* Files: `web/src/` (settled in Read).
* Depends on: branch 3.
* Checks: `pnpm typecheck`, `pnpm test`.

**Status:** not started.

## Open questions

* Consumers that copy config values would not see a `set` until they copy
  again. Branch 3's Read audits every `machineCfg()` caller and lists which
  need a refresh effect. Core 1 never reads the struct; Core 0 publishes
  copies to `shared_state`, known so far:
  * `bufferQueueS` / `bufferIdleMs` (buffer times), published at config
    load or adopt (`src/rp2350/core0/config/machine_cfg.cpp:18`), read by
    `core1/emit/follower.cpp:83`. Needs a republish effect; each is one
    32-bit write.
  * `plannerSpm[4]` / `plannerTurnQ32` (`stepsPerUnit`, `invertDir`),
    `plannerMesh` (`meshOn`), `plannerBlade` (the mounted tool's blade
    offset): latched at each planner ring reset
    (`src/rp2350/ipc/shared_state.h:203`). Picked up at the next reset,
    provided their gates allow `set` only while `!plannerActive`.
  * Feeds and accels reach Core 1 already applied by Core 0's planner;
    audit `planner/queue.cpp` for copies of them.
* Do unsaved edits survive a soft reset, or does it revert `live`?
* Which effects allow `set` while RUNNING (feed-override style), if any.
  Default: IDLE/ALARM.
* Effects that need the old value (move a datum by the delta instead of
  voiding it): leave `apply(before, live)` room, void for now.
* Peak RAM of the unfiltered `JsonDocument` for the largest real config,
  measured in branch 3.
