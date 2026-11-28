# Node bootloader: update DB32 nodes over the bus

**Open.** No branch started.

Today every node is flashed over UPDI (`upload_protocol = serialupdi` in
`platformio.ini`); the application starts at address 0 and owns the chip, so
an update means walking the UPDI adapter to the board. This plan adds a small
bootloader to the AVR128DB32 nodes so the host can update them over RS-485,
through the Pico, with UPDI kept for first setup and recovery.

Prior art: [nodeflash](https://github.com/joginfrancis/nodeflash) (STM32
nodes, ESP32 gateway) — same shape, none of its flash code carries over.

Numbers marked ~ are estimates; branch 1 measures them.

## Scope

* **AVR128DB32 nodes only.** The ATtiny boards are removed from the repo
  (branch 2); their DB32 replacements already exist (`vac_db_node7`,
  `knife_node8`, `db_node1`–`db_node6`).
* **Out of scope:** hot plugging the knife and BLTouch modules. This plan's
  wait-for-`RUN` boot gives it a clean attach path (a new module appears in its
  bootloader, is checked, updated if needed, then run and bound), but the
  electrical side and the presence poll get their own plan.
* **No A/B slots, no image archive.** A failed update leaves the node in its
  bootloader, still reachable. The fallback is `READ`: the host can read back
  an image before erasing it.

## Vocabulary

| Word | Means |
| --- | --- |
| bootloader | the program in the boot section; runs first after every reset |
| application | the node firmware, after the boot section |
| identity | node ID, node type and board, in USERROW; written over UPDI only |
| record | the last application page: length, folded CRC-32, magic |
| folded CRC | `CRC32(identity ‖ start address ‖ image)` |
| bootloader mode | the node is in its bootloader, answering the bus |

## Decisions

### Ownership

| What | UPDI | Bootloader |
| --- | --- | --- |
| Application flash | yes | yes |
| EEPROM | yes | yes, if a node uses it |
| Identity (USERROW) | yes | no: no command writes it |
| Boot section | yes | no: hardware-protected by `BOOTSIZE` |
| Fuses, lock bits | yes | no: software can't write them on the Dx |

Anything that could make a node unreachable on the bus is UPDI-only. The DB's
UPDI pin can't be fused off, so UPDI recovery is always possible with access
to the board.

### Clock

The bootloader runs from the internal oscillator only, at 24 MHz. Its clock is
independent of the application's: it only has to make 921 600 baud, and the
internal oscillator is on every board, whatever crystal (16, 20, 24 MHz, or
none) the application uses. Before jumping it returns the clock to its reset
state.

One binary for every DB32 board. Branch 1 measures its baud error against the
bus; if the margin is too thin, the fallbacks are auto-baud or autotune
against a 32 kHz crystal.

### Boot

At reset, the bootloader stays in bootloader mode until the controller sends
`RUN`, unless one of these keeps it there regardless:

* no valid record (blank, or an update cut off);
* the folded CRC fails;
* the application asked (a magic value in a GPR, then a software reset);
* crash loop: N consecutive watchdog resets, counted in RAM that survives a
  reset; the application clears the count once it has run healthily.

`RUN` is broadcast at the end of the controller's boot sweep, and sent to one
node after an update. A node never runs without a controller, as today.

### Bus

The bootloader looks like a node to Core 1:

* same frame (`[id][cmd][len][payload][crc8]`, 9th bit set), software ID
  filtering (multiprocessor mode can't be used: nodes must hear stream bytes);
* answers `CMD_PING` and `CMD_GET_TYPE`, with a new node type for bootloader
  mode; NAKs every application command with a new reason, `NAK_IN_BOOTLOADER`,
  so the controller can tell "in bootloader" from "unknown command";
* ignores stream bytes, refuses engage, so it is never bound to a slot.

New commands, in `include/common.h`. Names proposed, not settled: `CMD_` stays
the prefix of every command byte, and `BOOT_` marks the bootloader's.

| Command | Does |
| --- | --- |
| `CMD_BOOT_ENTER` | application side: set the magic, software reset |
| `CMD_BOOT_INFO` | identity, bootloader version, record status, application CRC, crash count |
| `CMD_BOOT_ERASE` | wipes the record first, then the application pages |
| `CMD_BOOT_WRITE addr, data` | writes a chunk; refuses addresses outside the application |
| `CMD_BOOT_COMMIT length, crc` | computes the folded CRC over flash; writes the record only on a match |
| `CMD_BOOT_READ addr, n` | reads a chunk back |
| `CMD_BOOT_RUN` | jumps, if the record is valid; broadcast allowed |

`len` is one byte, so `WRITE` and `READ` move chunks of up to ~240 bytes, not
whole 512-byte pages; the bootloader buffers a page.

### Checks

* **Host:** reads `INFO`, checks it against the image's build metadata and
  refuses with a readable reason.
* **Node:** the folded CRC at `COMMIT` and at every boot. It trusts nothing the
  host says.
* A valid image with a bug passes every check; recovery is a power-cycle,
  which lands in bootloader mode.

### Pico

The Pico carries the host's update to the node, refused unless the machine is
idle (no stream, no job). The update loop lives on the host.

Flow control reuses the wire's existing patterns (`system/wire.md` §7, §9).

**Write: the job stream, with pages as records.**

* A record is one 512-byte page: seq, page address, data, CRC. The Pico
  splits it into bus chunks.
* Window of 16 pages, batched writes, cumulative ACK held back up to 8,
  resend from the oldest in flight; `SEQRESET` and `ABORT` as for jobs.
* **A page is ACKed once the node has acked it written**, not when the Pico
  takes it. An ACK means "in flash"; a NACK means this page failed on the
  node, now, with its address. The window keeps the bus busy, so this costs
  no speed, and the Pico holds no error past an ACK it sent.
* The bootloader erases each page just before writing it, so a resent page is
  harmless.
* `INFO`, `COMMIT`, `RUN` are single request and reply.

**Read: one request, pages streamed back.**

* The host sends `READ(start, length)`. The Pico pulls chunks from the node,
  retrying on the bus itself, and sends one frame per page, `CFG_DATA`-style
  (address, length, payload, CRC, read by count in the demux), then a done
  frame with a CRC-32 over the range, or a NACK.
* No window: USB is ~10× the bus and lossless, and the host always drains its
  input. The host checks order and CRCs, and re-requests only on a gap.
* New on the Pico: Core 0 sends a page only when the USB transmit buffer has
  room for all of it, otherwise on a later pass, never blocking.

Both directions are bus-bound, ~1.5 s per 128 KB plus page writes; branch 1
measures it.

### Debug bootloader

The application's console (`src/node/debug_console.cpp`) is a second front end
to the bus's handlers: it parses an ASCII hex line, adds the CRC, calls the
same `routeCommand()` and prints the reply. The bootloader does the same: one
command handler, the bus as its front end, and in the `_dbg` build the console
as a second. On the bench `scripts/node_console.py` then plays the controller:
it sends `RUN`, and reads and flashes a node with no Pico.

* **Line length:** the application's console line is 64 characters, about 20
  bytes; a `WRITE` chunk in hex is three characters a byte. The bootloader's
  console takes a longer line or smaller chunks; branch 3 settles which.
* **No Arduino core:** the bootloader is its own program, with its own minimal
  USART code for both ports, not `Serial`.

## Branches

### Branch 1: `bench/node-bootloader`

**Type:** bench. **Depends on:** nothing.

**Purpose:** pin the plan's estimates on one DB32 board:

* bootloader size, which sets `BOOTSIZE`;
* boot-time CRC over a full application;
* internal-oscillator baud error against the bus, cold and warm;
* page write time;
* the jump to an application linked after the boot section.

**Files:** `src/scratch/node_bootloader/`, its own scratch env in
`platformio.ini`, a script in `scripts/`.

**Status:** not started.

### Branch 2: `refactor/db32-nodes`

**Type:** refactor. **Depends on:** nothing; runs alongside branch 1.

**Purpose:** remove the ATtiny nodes. The DB32 stepper and vacuum boards
replace them. Before the removal, tag the last commit with them as
`archive/attiny-nodes-<yyyy-mm>` (the user creates the tag).

**Files:** the `attiny_base`, `attiny3226_base`, `node1`–`node4`,
`vac_node7`, `vac_node7_dbg` and `hello_serial0_tiny` envs in
`platformio.ini`; `src/node/board/attiny3224/`, `src/node/board/attiny3226/`;
ATtiny branches in `src/node/board/avr128db32/vacuum/vacuum.h`,
`src/node/types/vacuum/`, `src/scratch/hello_serial.cpp`,
`scripts/node_console.py`. **Docs:** `system/firmware-build.md`,
`system/nodes.md`, `README.md`.

**Status:** not started.

### Branch 3: `feature/node-bootloader`

**Type:** feature. **Depends on:** branches 1 and 2.

**Purpose:** the bootloader, release and `_dbg` builds: the boot decision,
the crash counter, the command handler with the bus as its front end and the
console as a second in `_dbg`, `NAK_IN_BOOTLOADER`, the new commands and node
type in `include/common.h`, `CMD_BOOT_ENTER` in the application, and the DB32
applications linked after the boot section. Tested from the PC through
`scripts/node_console.py`, which learns to run, read and flash a node, before
the Pico can relay.

**Files:** a bootloader source tree and its two envs, `avr128db_base` in
`platformio.ini`, `include/common.h`, `src/node/`, `scripts/node_console.py`.
**Docs:** `system/nodes.md`, `system/bus.md`, `system/firmware-build.md`,
`reference/commands.md`, `reference/packets.md`.

**Status:** not started.

### Branch 4: `feature/pico-bus-passthrough`

**Type:** feature. **Depends on:** branch 3.

**Purpose:** the Core 0 relay command, refused unless idle; `RUN` broadcast at
the end of the boot sweep; bootloader-mode nodes recognised and never bound.

**Files:** `src/rp2350/core0/cmd/table.h`, `src/rp2350/core1/bus/`.
**Docs:** `system/controller.md`, `system/bus.md`, `reference/commands.md`.

**Status:** not started.

### Branch 5: `feature/host-node-update`

**Type:** feature. **Depends on:** branch 4.

**Purpose:** the update in `urumi-host`: `INFO`, metadata check, optional
`READ` backup, `ENTER_BOOTLOADER`, `ERASE`, `WRITE`…, `COMMIT`, `RUN`, `INFO`.

**Files:** `web/src/` (packets, a new update module), `web/demo` if it gets a
UI. **Docs:** `reference/host-api/`, `system/host-build.md` if the build
changes.

**Status:** not started.

## Open questions

* **Command names:** `CMD_BOOT_*` is proposed; settle it before branch 3.
* **Core 0 ↔ Core 1:** how update requests reach the bus side, which Core 1
  owns.
* **Command numbers:** bus.md gives 0x01–0x1F to generic commands, but
  `CMD_SERVO_SET` (0x10) to `CMD_KNIFE_BLOWER` (0x13) are type-specific and sit
  in that range. Settle the ranges before numbering the new commands.
* **Crash-loop threshold N**, and how long the application runs before it
  clears the count.
* **Bootloader size:** `BOOTSIZE` is in 512-byte steps; branch 1 sets it.
* **Image metadata:** where the build puts node type, board and version so the
  host can read them from the image file.
