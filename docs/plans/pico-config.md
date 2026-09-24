# Pico reads the machine config

First step of moving the controller layer from `web/src` onto the Pico: the
Pico decodes the config blob it already stores, and uses it for its first
consumer, the axis map.

## Decisions

* **Storage moves to LittleFS.** The raw A/B store was kept because it let a
  reader walk the blob in place over XIP. The firmware now decodes the blob once
  into a struct, so that advantage is gone, and LittleFS brings wear levelling
  and room for more files. `docs/config_storage_littlefs_migration.md` is the
  starting point; its Cancelled status is lifted by branch 1.
* **Blob format stays msgpack**, decoded with ArduinoJson v7
  `deserializeMsgPack` straight from the `File`, with a filter so only the
  fields the Pico consumes are built. Host encodes with `@msgpack/msgpack`.
* **The host pushes a fully-resolved config** (every default filled in by
  `load.ts`). The Pico has no defaults table; a missing field is a rejection.
* **The blob carries a payload schema version** (`v`). The Pico rejects a
  version it does not know.
* **Validation is split by consumer.** The Pico validates only the fields it
  decodes and uses. The host (`validate.ts`) validates everything. A shared
  fixture set (good and bad configs) runs through both.
* **`ALARM_CONFIG` means "no valid config"**, as `shared_state.h:142`
  originally reserved it. It stops being the unmapped-axis boot gate.
* **A failed map is `ALARM_NODE_FAULT`** (`shared_state.h:145`, reserved
  until now). No new alarm reason. The exit rule checks state, not history:
  leaving `ALARM_NODE_FAULT` (by `unalarm` or a successful `axis_map`) is
  refused while the map is incomplete. Complete means it equals the config's
  map for some head; a slot whose node the config marks absent is correctly
  empty.
* **First consumer: the axis map, driven by a Pico controller layer on top of
  the control plane.** At boot and after each accepted `CFG_SET`, the
  controller builds the `defaultHead` map from config, matching
  `slotMapFor(machine, machine.defaultHead)` in `web/src/machine/slots.ts:82`
  (X, Y, then the head's Z/A; an absent node binds as null), and calls
  `axisMapApply` (`src/rp2350/core0/cmd/axis.cpp:184`), the same path as a
  host `axis_map`, so node engage and slot state adoption are reused. A host
  `axis_map` remains the head-switch command.

## Branch 1: `refactor/config-littlefs`

**Plan**

* Type: refactor. Same wire behaviour (`CFG_SET`/`CFG_GET` framing, NACK
  reasons, Core 1 flash quiesce), different storage.
* Purpose: replace the A/B slot code with `/config.bin` (header + payload),
  written as `/config.tmp` then renamed over the old file so the previous
  config survives a power cut mid-write.
* Files:
  * `src/rp2350/config/config_store.{h,cpp}`: LittleFS mount, write-tmp-rename,
    boot CRC check; `ConfigCache` loses the XIP `addr`.
  * `src/rp2350/core0/data_plane.cpp`: `CFG_GET` streams from the file in
    chunks instead of from the XIP pointer.
  * `src/rp2350/core0/cmd/query.cpp`: `status cfg` fields.
  * `docs/config_storage.md`, `docs/config_storage_littlefs_migration.md`.
* Depends on: nothing.
* Checks: `pio run -e pico`. Human: push/pull a blob on hardware, power-cycle,
  confirm it survives; pull power during a write, confirm the old one survives.

**Status:** merged. Hardware checks move to branch 2, which adds the host
sender. Unblocks branch 2.

**Outcome:**

* `ConfigBlobHeader` keeps `version` (drops `magic`); `ConfigCache` now holds
  `mounted/valid/length/seq/crc32`, and readers use `configStoreRead()`.
* The park handshake stays around the whole write, on top of LittleFS's own
  per-operation Core 1 idle.
* First boot on this firmware formats the span; a config stored by the old A/B
  store is lost and must be pushed again.

## Branch 2: `feature/pico-config-read`

**Plan**

* Type: feature.
* Purpose: decode and validate the config on the Pico, gate the machine on it,
  and build the axis map from it.
* Pico:
  * `platformio.ini` `[env:pico]`: add `bblanchon/ArduinoJson@^7` to
    `lib_deps`.
  * New `src/rp2350/config/config_decode.{h,cpp}`: blob → `MachineCfg`
    (axes: node id, present, stepsPerUnit, invert, rotary, maxFeed, maxAccel,
    maxTravel; heads: z/a axes, defaultHead) plus consumer-scoped validation
    (node ids in range, no duplicate node ids, stepsPerUnit > 0). Keep it free
    of Arduino I/O so it builds under `native`.
  * `src/rp2350/core0/data_plane.cpp`: decode and validate the staged blob
    before commit; new NACK reason `CFG_NACK_SCHEMA` on failure. Accept
    `CFG_SET` only in IDLE or ALARM.
  * New `src/rp2350/controller/`: builds the `defaultHead` map from
    `MachineCfg` and calls `axisMapApply`; a failure raises
    `ALARM_NODE_FAULT`.
  * `src/rp2350/core0/core0.cpp:108`: at boot, missing or invalid config →
    `ALARM_CONFIG`; valid → run the controller's boot map.
  * `src/rp2350/core0/cmd/axis.cpp`: `axis_map` no longer clears
    `ALARM_CONFIG` (`:259`, `:350`) and is refused in it; rejects nodes not in
    the config. The re-alarm at `:240-256` becomes: an incomplete map leaves or
    puts the machine in `ALARM_NODE_FAULT`; a complete one clears it. Rewrite
    the comments at `:44`, `:134`, `:175`, `:582`.
  * `src/rp2350/core0/cmd/lifecycle.cpp:75`: `unalarm` refuses
    `ALARM_CONFIG` (as now) and refuses `ALARM_NODE_FAULT` while the map is
    incomplete. Same check on `setorigin`'s ALARM→IDLE path.
  * `src/rp2350/core0/position.h:48`, `src/rp2350/ipc/shared_state.h:142-145`:
    comments for the new meanings.
  * `src/rp2350/core0/usb_protocol.h`: `CFG_NACK_SCHEMA`.
  * `status cfg`: report decoded / schema version / reject reason.
* Web:
  * `package.json`: `@msgpack/msgpack`.
  * `web/src/wire/format/cfg.ts` + a sender in `web/src/wire/link/`: encode the
    resolved config with `v`, push via `CFG_SET`, pull via `CFG_GET`.
    `CFG_NACK_SCHEMA` added to the NACK namespace.
  * `web/src/wire/format/status.ts:49`: `ALARM_NODE_FAULT` and
    `ALARM_CONFIG` descriptions for their new meanings.
  * `web/src/controller/controller.ts:396-424`: connect no longer owes an
    `axis_map`; `commit()` is for head switches. Update the doc comments that
    say the Pico sits in `ALARM_CONFIG` until a map commits, including
    `web/src/machine/setup.ts:155`.
  * Tests: encode round trip; the shared fixture set written to
    `web/test/fixtures/config/` for the Pico decoder test.
* Pico test: `pio test -e native` runs `config_decode` against the fixture
  blobs.
* Docs: `docs/engage_and_axis_map.md` §6, `docs/config_storage.md`,
  `docs/wire_protocol.md` (new NACK reason).
* Depends on: branch 1.
* Overlap: `platformio.ini`, `web/src/wire/`.
* Checks: `pio run -e pico`, `pio test -e native`, `pnpm typecheck` and
  `pnpm test` in `web/`. Human: boot with no config → ALARM_CONFIG; push a
  config → IDLE and mapped without the host sending `axis_map`; push a bad
  config → NACK, old config and state kept; head switch still works; boot with
  a node unplugged → `ALARM_NODE_FAULT`, `unalarm` refused, plug it in and
  `axis_map` → IDLE. Plus branch 1's storage checks: pull matches push and
  survives a power cycle; power cut mid-write keeps the old config; steps still
  stream after a config write.

**Status:** merged. All planned branches are done. On the bench: push, readback and persistence across
power cycles pass; boot with nodes off → `ALARM_NODE_FAULT`, nodes on then
`unalarm` → IDLE. Remaining hardware checks: bad blob (`CFG_NACK_SCHEMA`),
power cut mid-push, partial `axis_map`, motion after a config write.

**Outcome:**

* Changed from the plan (agreed on the bench): "complete" means the bound map
  equals the last requested map, not the config's map for some head. A host
  `axis_map` may be partial (bench testing one node) and clears
  `ALARM_NODE_FAULT` once its nodes engage. `unalarm` retries the requested
  map once before answering `err unmapped`.
* The barebones demo gained Push/Pull config and `status cfg` buttons.
* Follow-up: the web Sim (`web/src/wire/link/backends/sim.ts`) still models
  the old boot gate (host `axis_map` clears `ALARM_CONFIG`) and has no
  `CFG_SET`/`CFG_GET`. It needs the config store, the controller's default
  map commit, and the complete-map rule. Deferred by the user.

## Open questions

* Which controller-layer piece moves next once the config is readable (homing
  and probe recipes are the obvious candidates).
* Layering of Core 0 code (needs more deliberation before planning). A likely
  shape: `cmd/` only parses and replies; `controller/` runs sequences
  (default map, homing, probing, head switch) and is called by `cmd/`;
  operations (`axisMapApply`, node RPC, position model, alarm gate) sit below
  both. That means moving the operations out of `cmd/` (`axisMapApply` lives in
  `cmd/axis.cpp` only because the command was its first caller), and later
  `homing.cpp`/`probe.cpp` into `controller/`. The move would be its own
  `refactor/` branch, landing before homing/probing move. Open: where the
  operations live (`core0/ops/`, or folded into `position.*`), and how Core 0's
  lifecycle hooks (soft reset, `CFG_SET`) enter the controller.
  * Commands split in two. Primitive commands (today's `cmd/`) are atomic and
    config-free: everything they need is an argument, so a host or a person at
    a terminal can drive the machine step by step, as when the host was the
    controller. Controller commands read the config and run sequences built
    from the same operations.
  * `ALARM_CONFIG` would gate only controller commands; primitives keep working
    without a config, for bench work. The partial `axis_map` from branch 2 is
    already this pattern. Production builds could block primitive commands
    entirely (a build flag).
  * Branch 2 leaks config into a primitive: `axis_map` answers
    `err unconfigured` and `err node N not_in_config`. The refactor moves those
    checks to the controller.
  * `unalarm` is a controller command: it recovers according to `alarmReason`
    (retry the requested map for `ALARM_NODE_FAULT` today; backing off a
    latched switch is a candidate). Primitives may need no `unalarm`, since the
    exit rule checks state: re-issuing the fixing primitive (`axis_map`, a
    reverse `home`) clears the alarm through `resumeOrHold()`. Check which alarm
    reasons have no fixing primitive and need a plain acknowledge instead.
  * Still to decide: what `ALARM_CONFIG` means for the machine state when
    primitives may run, and which side the data plane (segment streaming) is on.
* Motion gating replaces the unmapped alarm (needs its own planning pass).
  Branch 2 keeps the unmapped-map `ALARM_NODE_FAULT` gate as a conservative
  interim; it is safe but too broad.
  * Problem: `ALARM_NODE_FAULT` means two things, "a node failed during an
    operation" (e.g. the knife timing out) and "slots are not bound". Recovery
    for one asks about the other, and an unmapped machine blocks non-motion
    work: testing the knife board needed a throwaway `axis_map` first.
  * Direction: an unmapped machine is IDLE, not alarmed. Only motion is gated.
    Motion primitives (`step`, jog, segment ingest, `home`, `probe`) NACK or
    answer `err unmapped` when the map is incomplete, with no state change.
    Motion controller commands (home the machine, probe, run a job) run
    `axis_map` first, even when already mapped, so every sequence starts with
    every node engaged. `ALARM_NODE_FAULT` goes back to meaning only a node
    failure during an operation.
  * Jogs are the exception: an `axis_map` before every jog is too slow. Jobs
    and jogs both need the correct frame set up first, so the jog path is
    part of that design, not solved by the map alone.
  * Costs: motion ingest gates on `machineState` alone today, so a shared
    `motionAllowed()` check is threaded through every motion entry point
    (`engage_and_axis_map.md` §6 argues for the alarm on exactly this ground and
    gets rewritten); `STATUS_RSP` must report map state so a host can tell why
    motion was refused (wire change, `web/src/wire/`); `unalarm`'s map retry
    goes away with the unmapped alarm.
  * Depends on the command split above: which commands are motion primitives
    and which are controller commands.
