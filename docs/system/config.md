# Config

**Checked against `90984f4`.** To see what may have changed since:

```bash
git diff 90984f4 -- src/rp2350/core0/config/ src/rp2350/core0/data_plane.cpp src/rp2350/core0/controller/cmd/config.cpp src/rp2350/core0/cmd/query.cpp web/src/machine/ web/scripts/config-image.ts scripts/pio/config_image.py
```

Commands, replies, status keys and code names are written as the
[notation](../README.md#notation) says.

The config is everything the controller knows about the machine it drives:
which node turns which axis, how many steps make a millimetre, how fast each
axis may go, how it homes, where the heads sit, which tools exist. It is
written by hand as a JSONC file, compiled by the host, and kept by the
controller in its own flash, where it is read at every boot and can be edited
while the machine runs.

The config takes four forms on its way:

| Form | Lives in | Made by |
| --- | --- | --- |
| JSONC | a file on the host, `config/controller.jsonc` | you |
| Blob, on the host | host memory | the loader and encoder (`loadConfig`, `encodeConfigBlob`) |
| Blob, on the controller | controller flash, `/config.bin` | a push (`CFG_SET`), `uploadfs`, or `save` |
| Runtime struct | controller RAM | the decoder (`configDecode`); `set` edits it |

The blob is one msgpack document. It crosses the wire unchanged and is stored
unchanged; the host and the controller each wrap it in their own small header.

## 1. Design

1. **The controller's stored config is the single source of truth.** The
   machine behaves the same whichever host connects to it, from any PC or
   browser, or none at all. A host reads the config back from the controller
   and never treats a copy of its own as the authority.
2. **The controller stores one msgpack blob as it received it, with a schema
   version `v`.** It refuses a `v` it does not know, since the two sides must
   agree on the shape.
3. **A blob is decoded before it is written.** One the controller would refuse
   at boot never reaches flash. The write is a new file renamed over the old,
   so a power cut leaves the old config.

4. **The host compiles the JSONC into a fully resolved config.** Every default
   is filled in on the host, so the controller has no defaults table: a field
   it reads and does not find is a rejection, except a few optional ones.
5. **Each side checks what it uses.** The controller checks only the fields
   it reads, for what would make its own use of them wrong. The host's
   validator checks everything. A shared set of good and bad configs runs
   through both.

6. **The controller holds one decoded config in RAM.** Everything reads that
   copy, never the blob, and only Core 0 reads it.
7. **Edits while running change that copy, and only fields named in a table.**
   A changed field may leave the machine's idea of its position wrong (a
   datum, a work offset); each row names what a change to it sets off.
8. **The table is host data; the controller's copy is generated from it.**
   Both sides accept the same fields with the same ranges, and a test fails
   when the generated copy is stale.
9. **An edit lasts until `save`, which needs a dirty flag.** `reset` drops
   unsaved edits by loading the stored blob again; there is no `revert`.
10. **`save` writes the edits into the stored blob and keeps everything else.**
    For that to lose nothing, every table field must fit a float32 exactly,
    which the host enforces.
11. **A push replaces the whole config, with a soft reset that voids every
    datum.** A `save` stores values the machine already runs with, so it does
    neither.

## 2. JSONC

`config/controller.jsonc` is JSON with `//` and `/* */` comments. It is the
one place a machine's numbers are written by hand.

* **Required or defaulted, never both.** A field is required exactly when it
  has no default in `web/src/machine/defaults.ts` (`DEFAULTS`). The required
  ones are the machine's own: `machine.x`, `machine.y`, `machine.meshOn`,
  `machine.heads` with each head's `z`, `a` and `accepts`, each axis's
  `node.id` and `stepsPerUnit`, each peripheral's `id` and `type`, each
  tool's `id`, and the numbers inside a homing or probe block that is given.
  Anything else left out takes its default.
* **Tools start from presets.** Each key under `tools` is the name of a preset
  in `web/src/machine/tools.ts` (`pen`, `knife`, `crease`, `revolver_pen`),
  and its entry overrides only the fields it gives.
* **Unknown keys are ignored,** except retired names whose silent loss would
  change what the machine does (a head's `tool`, `machine.work`, the
  machine's `path`, `rapid` and `z`, top-level `heads` and `peripherals`):
  those are refused by name.

The host loads the file in a browser or under Node. The same file can also be
flashed with no host connected:

```bash
pio run -e pico -t uploadfs
```

Before `buildfs` or `uploadfs`, `scripts/pio/config_image.py` runs
`web/scripts/config-image.ts` on the file named by `custom_config_json` in
`env:pico`, which writes `data/config.bin`: the controller's file header and
the blob (section 5). With `custom_mesh_json` set, `web/scripts/mesh-image.ts`
writes `data/mesh.bin` too; without it a stale one is removed. The filesystem
image is built from `data/` and written over the whole span, so it replaces a
config pushed since. A plain `upload` leaves the filesystem alone. It needs
pnpm on the PATH and `pnpm install` run in `web/`.

The image script parses the file but does not run the host's validator: a
file that parses but breaks a rule is flashed, and the controller's own
checks at boot are all that stand in its way.

## 3. Blob on the host

### Loading

`loadConfig` (`web/src/machine/json/load.ts`) runs two passes, never mixed:

1. **Parse** (`parseConfig`): is the file well formed? Shape, types,
   required fields; defaults filled in; tools merged onto their presets. Every
   problem is listed, not only the first.
2. **Validate** (`validateConfig`, `web/src/machine/json/validate.ts`): do the
   values make sense? A list of rules, each a function over the whole config:
   unique node ids, a head for every tool, homing that suits its axis, feeds
   under their ceilings, and so on. A rule returns errors, which refuse the
   config, or warnings, which are shown and never enforced (a feed above its
   axis's ceiling is clamped, so it only warns).

The result is a `ControllerConfig`: the resolved config, every default in
place. The file's `tools` become the model's `toolProfiles`.

### Float32

The controller holds numbers as float32; the host holds them as float64. A
field in the runtime table (section 6) must hold a value float32 keeps
exactly, so that a value the controller saves reads back as the one the host
wrote. `f32(v)` (`web/src/machine/f32.ts`) is the shortest decimal with the
same float32 as `v`; a value fits when `f32(v) === v`. The validator refuses a
table field that doesn't fit ("more precision than the controller keeps").

### The runtime table

`RUNTIME_FIELDS` in `web/src/machine/runtime.ts` lists the fields the
controller can edit while running. A row is a template over a scope: a
machine field, every axis, every head, or every tool. It names the field's
path, its C++ member, its type, its range, the homing kind it needs, and its
effects (section 6). The validator reads the ranges from it.

```bash
pnpm gen:runtime
```

writes the controller's copy, `src/rp2350/core0/config/runtime_fields.h`, an
X-macro the firmware builds its table from. The header is committed, so a
firmware build never needs pnpm; a test in `pnpm test` regenerates it in
memory and fails if the committed one differs.

### Encoding

`encodeConfigBlob` (`web/src/machine/json/blob.ts`) is msgpack of the
resolved config with `v` added (`CONFIG_BLOB_VERSION`). `v` changes when a
field the controller decodes changes shape, and must equal the controller's
`CFG_SCHEMA_VERSION`.

The way back: `decodeConfigBlob` turns a pulled blob into a
`ControllerConfig`, refusing an unknown `v` and trusting the rest, and
`modelToFile` (`web/src/machine/json/save.ts`) turns that into a config file
that loads to the same model. Defaults stay filled in and the comments are
gone.

## 4. Wire

Two packets on the USB serial, each opened by a magic byte. Their byte
layouts are in [reference/packets.md](../reference/packets.md#config); the host's side is
`Link.pushConfig` and `Link.pullConfig` (`web/src/wire/link/link.ts`).

### Push

A push is `CFG_SET`, in two phases, so a refusal never leaves payload bytes
on the wire:

1. The host sends the header: the blob's length and its CRC-32.
2. The controller checks the length (1 to 32768 bytes, `CFG_MAX_BYTES`) and
   the machine's state, then answers `CFG_RDY`, or `CFG_NACK` and a reason.
3. On `CFG_RDY` the host sends the blob. The controller stages it in RAM,
   folding the CRC as bytes arrive.
4. With the last byte in, the controller compares the CRC, decodes the blob
   (section 5), and writes it to flash.
5. It adopts the decoded config, resets the tool state (`toolsReset`, since
   ids may now name other tools), answers `CFG_ACK`, and runs a soft reset:
   the bus sweep, the default head's axis map, then a `ready` line.

`pushConfig` resolves on `CFG_ACK`; its caller sends nothing until `ready`,
since the reset discards serial input. A node that doesn't answer the sweep
shows in the state after, not as a NACK: the config itself was stored. Every
push voids every datum, since the sweep makes every node safe.

A push is taken only at rest in the idle context: nothing running, no step or
sequence live, the e-stop swept. Faults don't stop it, so a push is the way
out of a missing or bad config. The state is checked at the header and again
at the commit.

| Reason | Code | When |
| --- | --- | --- |
| `CFG_NACK_CRC` | `0x01` | the staged bytes don't match the header's CRC |
| `CFG_NACK_TOO_BIG` | `0x02` | length 0 or over `CFG_MAX_BYTES` |
| `CFG_NACK_BAD_STATE` | `0x03` | not at rest in the idle context |
| `CFG_NACK_FLASH` | `0x04` | the filesystem isn't mounted, or the write or its readback failed |
| `CFG_NACK_TIMEOUT` | `0x05` | no byte for 2 s (`CFG_RX_TIMEOUT_MS`) mid-transfer |
| `CFG_NACK_SCHEMA` | `0x06` | the blob didn't decode, or broke a rule (section 5) |

Any NACK leaves the old config stored and active.

### Pull

A pull is `CFG_GET`; the controller answers `CFG_DATA` with the stored
blob's length, its CRC and the blob. A length of 0 means no config is stored:
an answer, not an error. A read that fails after the header is sent can't be
reported, so the blob is padded with zeros and the host's CRC check refuses
it. A pull is answered in any state, but Core 0 sends the whole blob before
it reads anything else, so it is best left to idle times.

## 5. Blob on the controller

### Flash

The controller keeps a LittleFS filesystem in the top 128 KB of its flash,
below the 4 KB the framework keeps for EEPROM emulation
(`board_build.filesystem_size` in `platformio.ini`). Reserving it this way
shrinks the program region, so the linker cannot put code there. LittleFS
spreads writes over the span and keeps its own metadata safe across a power
cut. It holds:

* `/config.bin`: the config.
* `/mesh.bin`: the bed mesh, optional (below).
* `/config.tmp`: only during a write.

`/config.bin` is a 16-byte header (`ConfigBlobHeader`, little-endian) and the
blob:

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | u16 | the file header's format, 1 (`CFG_VERSION`); not the blob's `v` |
| `_rsvd` | u16 | padding |
| `seq` | u32 | write count: one more on every push or `save`; 0 from `uploadfs` |
| `length` | u32 | the blob's bytes, 1 to `CFG_MAX_BYTES` |
| `crc32` | u32 | CRC-32 of the blob (reflected, poly `0xEDB88320`) |

The file is good when `version` is 1, `length` is in range, the file is
exactly 16 + `length` bytes, and the CRC matches.

`/mesh.bin` holds the bed mesh: a 28-byte header (`magic` "MESH", `version`,
`nx`, `ny`, the first point and the spacing), `nx` × `ny` heights as int16
µm, and a CRC-32 of all before it. It is read once at boot; missing or bad,
there is no mesh. `machine.meshOn` says whether the controller follows it.
The planner page covers what the mesh does.

### Writing

`configStoreCommit` (`src/rp2350/core0/config/config_store.cpp`) writes the
blob staged in RAM, after the decode has accepted it:

1. Refuse unless at rest in the idle context, the length is in range and the
   filesystem is mounted.
2. Park Core 1.
3. Write the header (`seq` one more than the stored one) and the blob to
   `/config.tmp`.
4. Read `/config.tmp` back with the same check as boot.
5. Rename it over `/config.bin`.
6. Release Core 1. If 3–5 failed, remove `/config.tmp` and answer
   `CFG_NACK_FLASH`; the old file stays.

LittleFS renames atomically, so the rename is the commit: a power cut before
it leaves `/config.bin` as it was, and a stray `/config.tmp` is overwritten by
the next write. No interrupted write leaves the machine without a config.

**Parking Core 1.** Writing flash stalls code running from flash on both
cores, and Core 1 times the steps. LittleFS idles Core 1 by itself around
each erase or program, but that can land anywhere in Core 1's loop. So the
write first parks Core 1 at a known point, through `flash_op_requested` and
`core1_parked_for_flash`, in a loop that runs from RAM. Commits happen only at
rest, where Core 1 has no motion to time. The cores page covers the
handshake.

### Boot

At boot `configStoreInit` mounts LittleFS (formatting the span if it holds no
filesystem) and checks `/config.bin`. A good file fills `g_cfg`, the RAM copy
of its header. `machineCfgLoad` then reads the blob and decodes it into the
runtime struct.

Without a usable config, the controller boots with the no-config fault
(`F_CONFIG`) held and nothing mapped. Controller commands answer
`err unconfigured`; the primitives still work. `get cfgerr` says why:

| `cfgerr=` | Cause |
| --- | --- |
| `absent` | no `/config.bin` |
| `fs` | LittleFS did not mount |
| `file` | the header, length or CRC is wrong |
| a decode error | the blob didn't decode (below) |

An accepted push clears the fault. A build with `PICO_ALLOW_UNCONFIGURED`
adds `uncfg`, which sets the stored config aside until power-off and clears
the fault, leaving only the primitives (`slot_map` still binds nodes for the
bench).

With a config, the controller maps the default head's axes after every soft
reset's bus sweep (bus page). A soft reset keeps `g_cfg` and the decoded
config: flash hasn't changed. Only unsaved edits make it reload (section 6).

### Decoding

`configDecode` (`src/rp2350/core0/config/config_decode.cpp`) reads the blob
with ArduinoJson, through a filter that keeps only the fields the controller
uses. It runs in two steps:

1. **Parse** (`configParse`): build the struct. Every field must be present
   with its type, except these optional ones: a homing block (absent: the
   axis can't be homed), its `cycle` and `parkPos`; the laser; a head's
   `probeSwitch` and its `z`; `positions.park`, `.load` and `.mount`; and a
   tool's `cutFeed`, `plungeFeed` and `chamberOffsets`. Counts and ids that
   wouldn't fit the struct are refused here.
2. **Validate** (`configValidate`): run every rule on the struct. A rule is
   a function over the whole config, and the first failure is the answer.
   The rules guard the controller's own use: no node id twice, axis nodes
   that are steppers, a latch feed under its seek feed, and so on. Other
   judgements are the host's.

| Error | Means |
| --- | --- |
| `msgpack` | not msgpack, or too large to decode |
| `version` | `v` missing or unknown (`CFG_SCHEMA_VERSION`) |
| `missing` | a field the controller reads is absent or the wrong type |
| `heads` | no heads, too many, or `defaultHead` out of range |
| `periph` | more peripherals than fit |
| `node_id` | a node id outside 1–8 |
| `node_type` | an axis node that isn't a stepper |
| `dup_node` | one node id claimed twice |
| `steps` | `stepsPerUnit` not a positive number |
| `ceiling` | a ceiling or jog feed negative; `maxTravel` 0 on a linear axis |
| `homing` | a homing field out of range, or the wrong kind for its axis |
| `frames` | no anchor, a head's work Z missing, or a point out of reach |
| `buffer` | a buffer time not a positive number |
| `tools` | too many tools or chambers, a tool id 0 or repeated, a bad number |
| `feed` | a machine feed missing or under 1 mm/s |
| `z_clearance` | `machine.zClearance` negative |
| `overcut` | `machine.maxOvercut` negative |
| `power` | a power node not present or not switchable, or a bad settle or duty |

A push decodes into a second struct (`machineCfgStage`), so the active config
is untouched until the commit succeeds and `machineCfgAdopt` promotes it.

## 6. Runtime struct

### MachineCfg

The decoded config is a `MachineCfg` (`config_decode.h`), and every
consumer on Core 0 reads it through `machineCfg()`. Each member has its blob
field's name and nesting, except that `toolProfiles` is an array of tools
found by id, `v` is `version`, and the `machine` level is dropped.

An optional field has a `has` flag (`hasCycle`, `hasParkPos`, `hasCutFeed`,
`probeSwitch.hasZ`, …) beside its member. The member always holds the
resolved value (0, or `cycle`'s default), so consumers ignore the flag; `save`
and `get` read it.

A few helpers answer questions every consumer asks: `configSlotMap` (a
head's four bus ids, as the host's `slotMapFor`), `configTool`,
`configParkPos` (where a linear home leaves the axis), `configAxisRange` (the
soft range), `configUnlockRange` (a head's holder interlock) and
`configWorkOrigin` (the work offset at boot and on `wclear`).

### Editing

```text
set config.<path> <value>
get config.<path>
```

`<path>` is the field's blob path, exact case: `machine.x.maxFeed`,
`machine.heads.0.z.homing.seekFeed`, `toolProfiles.knife.cutFeed`. A tool is
named by its key. `<value>` is a number, `true` or `false`, or `<x> <y>` for a
point; `get` prints a point as `x,y` and an absent field as `-`. The handlers
are in `src/rp2350/core0/controller/cmd/config.cpp`, the logic in
`src/rp2350/core0/config/runtime_edit.cpp`.

`set` and `save` need a config (`err unconfigured` without one). They answer
`err busy` while anything moves, and `err bad_state` outside the idle context
(a paused job) or with the e-stop latched. What is left is IDLE, or a stop
with nothing running.

A `set` runs in order:

1. Find the row; none for this machine is `err no_field`. A probe switch
   height with no probe switch is `err absent`.
2. Parse the value against the row: `err bad_value`, `err range`, or
   `err precision` for more digits than float32 keeps.
3. If the field already holds the value, answer `ok` and do nothing else.
4. Write it, then run every decode rule. A failure puts the old value back
   and answers `err rule <error>`.
5. Mark the config dirty and run the row's effects.

Only Core 0 reads the config and `set` runs to completion in its loop, so
writing in place and rolling back is safe.

| Effect | Rows | Does |
| --- | --- | --- |
| `voidDatum` | an axis's `stepsPerUnit`, `invertDir`, `maxTravel`, `homing.pullOffDist`, `homing.parkPos`, `homing.indexPos` | voids the node's datum; X or Y puts both work offsets back to the work origin, Z unsets that head's work Z |
| `republishBuffer` | `machine.buffer.*` | copies the buffer times to Core 1 |
| `applyMesh` | `machine.meshOn` | turns the mesh on or off, as `mesh on` / `mesh off` |

Fields with no effect are live: consumers read the struct when they next
need it.

### Dirty and reset

An accepted change sets dirty; `cfg` shows `dirty=1` and `get cfgdirty`
answers 1. Unsaved edits end at power-off, at an accepted push, and at a soft
reset: a reset while dirty decodes `/config.bin` again and reapplies
`machine.meshOn`; a clean one touches neither. A reload that fails leaves no
config, as a failed boot does.

### Save

`save` (`machineCfgSave`) turns the struct back into the blob:

1. Read `/config.bin` into a full `JsonDocument`, every field, not only the
   decoded ones.
2. Write every table field from RAM at its path. An optional field without
   its `has` flag is removed. A whole number is written as an integer; any
   other value as the shortest decimal that reads back as the same float32,
   as a double (`runtimeShortest`).
3. Encode it to msgpack in the staging buffer.
4. Decode that, and require every table field to equal RAM.
5. Commit as a push does (section 5), with no soft reset and no effects.

Fields the controller never reads stay as the host wrote them. The saved
blob has the same values, not the same bytes, so its CRC may change even
with no edits; a `save` with nothing dirty still writes flash and moves
`seq`. After a save the host pulls again to see it.

`save` answers `ok`, or `err` with `flash`, `decode`, `too_big`, `verify` or
`bad_state`.

## 7. Inspection

`cfg` reads only RAM, in any state:

```text
cfg seq=3 len=2874 crc=0x1c291ca3 schema=17
```

| Output | Means |
| --- | --- |
| `cfg fs=unmounted` | LittleFS didn't mount; a push answers `CFG_NACK_FLASH` |
| `cfg none` | no good `/config.bin` |
| `seq`, `len`, `crc` | the stored file's header |
| `schema` | the active config's `v` |
| `decoded=0` | the file is good but didn't decode: no active config |
| `rejected=<error>` | the last refusal, at boot or of a push |
| `ignored=1` | `uncfg` set the config aside |
| `dirty=1` | unsaved edits |

`get cfgerr` (only while `F_CONFIG` is held) and `get cfgdirty` are the
status keys.

## 8. Who writes what

| Value | Writer | Readers |
| --- | --- | --- |
| `config/controller.jsonc` | you | `loadConfig`, `config-image.ts` |
| `ControllerConfig` | `loadConfig`, `decodeConfigBlob` | the host |
| `runtime_fields.h` | `pnpm gen:runtime` | `runtime_edit.cpp` |
| `/config.bin` | `configStoreCommit` (push, `save`), `uploadfs` | `configStoreInit`, `configStoreRead` |
| `g_cfg` | `configStoreInit`, `configStoreCommit` | `cfg`, `CFG_GET`, `machineCfgLoad`, `machineCfgSave` |
| staging buffer (`configStageBuf`) | the push receiver, `machineCfgLoad`, `machineCfgSave` | the decoder, `configStoreCommit` |
| active `MachineCfg` | `machineCfgLoad`, `machineCfgAdopt`, `set` | Core 0, through `machineCfg()` |
| pending `MachineCfg` | `machineCfgStage`, `save`'s check | `machineCfgAdopt` |
| dirty | `set`, cleared by `save`, adopt and load | `cfg`, `get cfgdirty`, soft reset |
| `bufferQueueS`, `bufferIdleMs` | `machineCfgPublish` | Core 1 |

## 9. Code

| File | Holds |
| --- | --- |
| `config/controller.jsonc` | the machine's config |
| `web/src/machine/schema.ts` | `ControllerConfig` and its types |
| `web/src/machine/defaults.ts` | every default value |
| `web/src/machine/tools.ts` | the tool presets |
| `web/src/machine/resolve.ts` | policy over the config: axes per head, feed chains |
| `web/src/machine/json/load.ts` | parse and `loadConfig` |
| `web/src/machine/json/validate.ts` | the host's rules |
| `web/src/machine/json/blob.ts` | the blob and `v` |
| `web/src/machine/json/save.ts` | `modelToFile` |
| `web/src/machine/json/image.ts` | `/config.bin` for `uploadfs`, JSONC comments |
| `web/src/machine/json/mesh.ts` | `/mesh.bin` for `uploadfs` |
| `web/src/machine/f32.ts` | the float32 fit |
| `web/src/machine/runtime.ts`, `runtimeHeader.ts` | the runtime table and its header |
| `web/scripts/config-image.ts`, `mesh-image.ts`, `runtime-fields.ts` | the generators |
| `scripts/pio/config_image.py` | runs the image generators for `uploadfs` |
| `web/src/wire/link/link.ts`, `web/src/wire/format/cfg.ts` | push and pull on the host |
| `src/rp2350/core0/data_plane.cpp` | push and pull on the controller |
| `src/rp2350/core0/config/config_store.*` | the file, its check, the commit |
| `src/rp2350/core0/config/config_decode.*` | `MachineCfg`, parse, rules, helpers |
| `src/rp2350/core0/config/machine_cfg.*` | the active and pending configs, dirty, load, save |
| `src/rp2350/core0/config/runtime_edit.*` | `set`, `get`, the save encoder |
| `src/rp2350/core0/config/runtime_fields.h` | the generated table |
| `src/rp2350/core0/controller/cmd/config.cpp` | `set`, `save` and the effects |
| `src/rp2350/core0/cmd/query.cpp`, `cmd/get.cpp` | `cfg`, `get config.*`, `cfgerr`, `cfgdirty` |
