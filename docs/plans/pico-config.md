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

**Status:** not started

**Outcome:**

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
  `axis_map` → IDLE.

**Status:** not started

**Outcome:**

## Open questions

* Which controller-layer piece moves next once the config is readable (homing
  and probe recipes are the obvious candidates).
