# Firmware build

**Checked against `dd2cc69`.** To see what may have changed since:

```bash
git diff dd2cc69 -- platformio.ini scripts/pio/ src/rp2350/main.cpp src/rp2350/core1/bus/uart_9bit.pio src/node/board/
```

Commands and code names are written as the [notation](../README.md#notation)
says.

The machine runs two kinds of firmware. The controller is one RP2350 that
plans motion and talks to the host. The nodes are small AVR chips, one per
motor or tool, that the controller drives over the RS485 bus. Both are built
from this one repository, with PlatformIO, from one file: `platformio.ini`.

This page explains how that build is put together, from the chips up. It
assumes you know what an Arduino core, a vendor SDK and PlatformIO are in
general; it says which ones this project uses, what it uses from them, and
why the build file is shaped the way it is.

Two names look alike here and mean different things. **PlatformIO** is the
build tool, and its command is `pio`. **Programmable I/O** is a block of the
RP2350 that runs tiny state-machine programs; the page writes it out in full,
or as PIO only after it has been named in a section. PlatformIO is never
abbreviated.

There are three build targets, each a stack of the same four layers:

| Target | Silicon | Arduino core | SDK and libraries | Our source |
|---|---|---|---|---|
| Nodes | ATtiny3224, ATtiny3226, AVR128DB32 | megaTinyCore, DxCore | TMCStepper, SPI, Servo | `src/node/` |
| Controller | RP2350 (Pico 2) | arduino-pico | Pico SDK, ArduinoJson, LittleFS | `src/rp2350/`, `lib/planner/` |
| Tests | the host PC | none | doctest, ArduinoJson | `test/`, `lib/planner/`, parts of `src/rp2350/` |

The sections below take the stack from the bottom: the silicon, the Arduino
cores, the SDK and libraries, our source, and last PlatformIO, which ties
them together and uploads the result. First, a short history, because the
build file still carries it.

## History

The history shows in `platformio.ini`, so two of its terms come first. An
**env** is one build: one firmware for one board, such as `node1`. A
**template** is a section of shared settings that envs borrow, one per chip,
such as `attiny_base`. Both are PlatformIO's; the
[PlatformIO](#platformio) section covers how they work.

The controller has always been an RP2350. The nodes have not always been the
same chip.

* **Stepper nodes** started on the ATtiny3224 (`node1`–`node4`, May 2026).
  A few weeks later the first stepper ran on the AVR128DB32 (`db_node1`,
  June), and every stepper since has been a DB32.
* **The vacuum node** started on the ATtiny3226 (`vac_node7`, July), the
  3224's larger sibling, because six servos and a pump need more pins than
  the 3224 has. Its second board is a DB32 (`vac_db_node7`, September).
* **The knife node** started on the DB32 (`knife_node8`, July) and has never
  been anything else.

So the nodes are converging on the DB32, but the older envs are still in the
file. That is why there are three MCU templates, two stepper env families,
and two vacuum envs with the same node ID. The two vacuum boards share ID 7
and can't both be on the bus at once; only one is powered at a time.

## Silicon and boards

PlatformIO needs to know two things about the hardware under each build: the
chip, and the few facts about the board around it that the firmware can't
find out by itself.

### The node chips

All three node chips are AVRs from Microchip's newer families, with the same
style of peripherals, so one source tree builds for all of them.

| Chip | Package | Used by | Bus USART | Console USART |
|---|---|---|---|---|
| ATtiny3224 | 14 pins | early steppers | `USART1` | none |
| ATtiny3226 | 20 pins | the first vacuum board | `USART1` | `USART0` (`Serial`) |
| AVR128DB32 | 32 pins | steppers, knife, vacuum | `USART2` | `USART1` (`Serial1`) |

The bus USART carries RS485 to the controller. The console USART is a second
serial port, wired on some boards to a USB adapter, that the debug builds use
to talk to a PC directly (see [Debug builds](#debug-builds)). Which USART is
which is set in the board's `board.h`, not in the build file, but it matters
to the build: a probe or console build has to pick the free one.

### Clocks

Every baud rate on a node is divided down from its CPU clock, so PlatformIO
has to be told the clock the board really runs at, in `board_build.f_cpu`.
The chip can't check this. If the number is wrong, the firmware still builds,
flashes and runs, and every serial line is off by the ratio: a 16 MHz board
built as 24 MHz sends 921 600 baud as 614 400, and the bus silently garbles.

Where the clock comes from depends on the board:

* The ATtinys run from an internal oscillator, 16 or 20 MHz, chosen by a fuse
  (`OSCCFG`). Their PlatformIO templates, `attiny_base` and
  `attiny3226_base`, assume 20 MHz; the 3226 vacuum board runs at 16, so its
  envs override it.
* The DB32 boards run from a crystal (`board_hardware.oscillator = crystal`).
  Their PlatformIO template, `avr128db_base`, assumes 24 MHz; the knife
  board has a 16 MHz crystal, so its envs override it.

When bringing up a new board, check its real clock before trusting the bus.
The `hello_serial` scratch envs exist for this: they print a banner at
115 200 baud, and if it arrives garbled the clock is wrong (see
[Scratch envs](#scratch-envs)).

### Board facts that are flags

A few things about a board can't be detected, so they are build flags. They
describe the hardware; they don't choose it.

* `RS485_USE_XDIR`: the board routes the USART's XDIR pin to the RS485
  transceiver, so the USART switches the bus direction in hardware. Without
  it the firmware toggles the direction pin in software. Every DB32 board
  routes it; the ATtiny boards don't.
* `BOARD_DB32_VACUUM`: the DB32 vacuum board has one debug LED where the
  other DB32 boards have an RGB LED.
* `DRV_MICROSTEPPING`: on some DRV8825 boards the microstep pins are
  strapped in solder, not routed to the chip, so the firmware can't set the
  microstepping; the flag has to say what the solder says. Wrong, it puts
  every distance on that axis out by a factor.
* `LIMIT_ACTIVE_HIGH`: the limit switch reads high when pressed.
* The driver chip, `DM542`, `DRV8825` or `TMC_2660`, which is soldered to
  the board.

### The controller chip

The controller is a Raspberry Pi Pico 2, an RP2350 on a standard board, so
there is nothing board-specific to tell the build except the board name
(`rpipico2`). What the build does configure:

* **Two cores.** Core 0 runs the control plane and the planner; Core 1 runs
  the bus. The Arduino core starts both (see
  [The controller's core](#the-controllers-core)).
* **Programmable I/O.** The bus's 9-bit transmit runs on a PIO state machine,
  whose program the build assembles (see
  [The PIO program](#the-pio-program)).
* **Flash.** Part of the flash is a LittleFS filesystem for the config and
  the bed mesh; the build sets its size and can flash its contents (see
  [The controller env](#the-controller-env)).
* **The FPU.** The RP2350 has a single-precision FPU. The build tells the
  compiler that nothing reads `errno` (`-fno-math-errno`), so `sqrtf` is one
  hardware instruction instead of a software routine that sets `errno`.

## Arduino cores

Each chip family has its own Arduino core: the framework that supplies
`setup()`, `loop()`, `Serial`, pin functions and the startup code.

| Chip | Arduino core | PlatformIO platform |
|---|---|---|
| ATtiny3224, ATtiny3226 | megaTinyCore | `atmelmegaavr` |
| AVR128DB32 | DxCore | `atmelmegaavr` |
| RP2350 | arduino-pico (Earle Philhower) | maxgerhardt's `platform-raspberrypi` |

### The node cores

The nodes use the Arduino core for the parts that aren't timing-critical:
`setup()` and `loop()`, `pinMode()` and `digitalWrite()` for the outputs,
`millis()` for timeouts, and `Serial`/`Serial1` for the debug console.

The bus doesn't go through the core. The bus USART's registers and its
receive interrupt are written directly, because a stream byte must become a
step within microseconds and the core's serial driver isn't built for 9-bit
frames at that pace. The nodes page covers why.

### The controller's core

arduino-pico comes through maxgerhardt's fork of the Raspberry Pi platform,
which the `pico` env names by URL; `board_build.core = earlephilhower`
selects the core.

The project uses it for:

* **Both cores.** arduino-pico runs `setup()` and `loop()` on Core 0 and,
  when they exist, `setup1()` and `loop1()` on Core 1. Defining them is all
  it takes to start the second core; `src/rp2350/main.cpp` only allocates
  the variables both cores share.
* **USB serial.** `Serial` is the USB link to the host.
  `SERIAL_BUFFER_SIZE=256` sets its buffer size.
* **Time.** `millis()`, `micros()`, `delay()`, and
  `rp2040.getCycleCount()` for the probe and debug-step loops on Core 1.

The follower, which emits the steps of every planned move, reads the cycle
counter straight from the core's DWT register instead (`core1/cycles.h`).
`getCycleCount()` builds its count with the SysTick interrupt, so it stalls
while interrupts are off; the DWT counter is one register load and counts
regardless.
* **LittleFS**, which ships with the core.

## SDK and libraries

### The Pico SDK

arduino-pico is built on the Raspberry Pi Pico SDK, and the SDK's headers are
available to any sketch. The controller calls a few of them directly:

| Header | What for |
|---|---|
| `hardware/sync.h` | spin locks and memory barriers between the cores, such as the planner's lock |
| `hardware/pio.h` | loading and starting the PIO program |
| `hardware/clocks.h` | the system clock, to set the PIO program's baud |
| `hardware/gpio.h` | pin setup for the bus |
| `pico/util/queue.h` | the request queue from Core 0 to Core 1 |
### The PIO program

The bus sends 9-bit frames, which the RP2350's UARTs can't, so the transmit
side is a PIO program: `src/rp2350/core1/bus/uart_9bit.pio`. It shifts out
the nine bits and drives the transceiver's direction pin around them.

A `.pio` file is assembled into a C header by `pioasm`. The platform does
this by itself: before compiling, it finds every `.pio` file under `src/`
and writes the header beside it, `uart_9bit.pio.h`, whenever the header is
missing or older than the `.pio`. The header is committed too, so a build
after editing the `.pio` rewrites a tracked file; commit the two together.
Edit the `.pio`, never the header.

### Libraries

| Library | Where | What for |
|---|---|---|
| TMCStepper | stepper nodes on the DB32 | configuring the TMC2660 driver over SPI |
| SPI | stepper nodes | TMCStepper's transport; ships with the core |
| Servo | vacuum node | the six vacuum gates; ships with the core |
| ArduinoJson 7 | controller, tests | decoding the msgpack config blob and runtime edits |
| LittleFS | controller | storing the config blob and the bed mesh; ships with the core |

The libraries that ship with a core need no entry in `lib_deps`; TMCStepper
and ArduinoJson do.

## Our source

PlatformIO compiles everything under `src/` by default. This project turns
that off in every env (`-<*>`) and names what each target compiles:

* `src/node/`: the node firmware. A core that every node compiles, one
  directory per node type under `types/`, and one directory per chip under
  `board/`.
* `src/rp2350/`: the controller.
* `lib/planner/`: the motion planner. PlatformIO's library finder compiles a
  `lib/` library into any env whose source includes one of its headers
  (`#include <planner/planner.h>`), so the controller and the tests share
  it.
* `src/scratch/`: one-off bench programs, each with its own env.

A node binary is the combination of exactly one node type, one board and one
node ID. The build picks the type by which `types/` directory it compiles,
the board by an include path (`-I src/node/board/<chip>`), so that
`#include "board.h"` finds that chip's header, and the ID by a flag. The link
refuses anything else: the type's hooks have no default, so a build with no
type fails, and two types define the same hooks and the same receive
interrupt, so a build with two fails. The nodes page covers the hooks and
the interrupts.

## PlatformIO

`platformio.ini` is a set of sections. A section named `[env:<name>]` is an
env: something PlatformIO builds. Any other section is just a named group of
settings that envs can borrow from.

### Templates and types

The file has two kinds of non-env section for the nodes:

* **MCU templates**, one per chip: `attiny_base`, `attiny3226_base`,
  `avr128db_base`. Each sets the platform, board, clock and upload protocol,
  and adds `node_core` (the sources every node compiles) and the chip's
  board include path.
* **Node types**: `type_stepper`, `type_vacuum`, `type_knife`. Each adds its
  `types/` directory and its `NODE_TYPE` flag. The vacuum and knife types
  also add the generic receive interrupt, `rs485/isr_generic.cpp`; the
  stepper type has its own.

A node env joins one of each:

```ini
[env:vac_db_node7]
extends          = avr128db_base
build_src_filter = ${avr128db_base.build_src_filter} ${type_vacuum.build_src_filter}
build_flags      = ${avr128db_base.build_flags} ${type_vacuum.build_flags} -DNODE_ID=7 -DRS485_USE_XDIR -DBOARD_DB32_VACUUM -DNODE_HAS_PROBE_REPLY
```

`extends` takes every setting from the template. That alone isn't enough,
because when an env sets a key the template also sets, the env's value
replaces the template's; it doesn't add to it. Both the template and the type
need to contribute to `build_src_filter` and `build_flags`, so the env
rebuilds those two keys by naming both sections with `${section.key}`, then
adds its own flags. Everything else, such as the clock and upload protocol,
comes through `extends`.

### Flags

The flags an env sets fall into four groups.

* **Identity:** `NODE_TYPE` (from the type section) and `NODE_ID` (from the
  env).
* **Board facts:** `RS485_USE_XDIR`, `BOARD_DB32_VACUUM`, the driver chip,
  `DRV_MICROSTEPPING`, `LIMIT_ACTIVE_HIGH` (see
  [Board facts that are flags](#board-facts-that-are-flags)).
* **What's fitted:** `HAS_LIMIT_SWITCH` and `HAS_HALL_INDEX` (the homing
  sensor; they share a pin, so one or the other), `NODE_HAS_LASER` (only one
  node carries the laser; the others refuse its command), `TMC_CURRENT` and
  `TMC_MICROSTEPPING`.
* **Behaviour:** `NODE_HAS_PROBE_REPLY`, `NODE_IGNORE_FERR`,
  `NODE_HAS_SILENCE_TIMEOUT`, `NODE_DEBUG_CONSOLE`.

What each flag does is listed in `AGENTS.md`.

### Debug builds

An env ending in `_dbg` is a node built with a serial console on the console
USART, so a PC can drive the node through `scripts/node_console.py` with no
controller or bus. It needs two things, not one: the flag
`NODE_DEBUG_CONSOLE` and the source `+<node/debug_console.cpp>`. The bus
still works in a debug build.

### The controller env

`[env:pico]` sets everything itself.

* **Platform and core:** maxgerhardt's platform by URL, `board = rpipico2`,
  `board_build.core = earlephilhower`.
* **Sources:** `-<*> +<rp2350/**>`, plus `lib/planner/`.
* **Flags:**
  * `PICO_ALLOW_UNCONFIGURED`, set here, so the bench build has `uncfg`.
  * `ACCEPT_LASER_AS_POWER_TOOL`, commented out.
  * `PLANNER_RAM`, a section attribute that puts the planner's tick path in
    RAM, like the other code that emits steps, so it never waits on flash.
  * `-fno-math-errno` (see [The controller chip](#the-controller-chip)).
  * `SERIAL_BUFFER_SIZE=256`.
* **The filesystem:** `board_build.filesystem_size = 128k`. This takes the
  space from the program region, so the program can never overlap it. It
  holds the config blob, its temporary copy while saving, and the mesh, each
  up to 32 KB.
* **The config image:** `extra_scripts = pre:scripts/pio/config_image.py`,
  with `custom_config_json` and `custom_mesh_json`. See
  [Upload](#upload).

### The test env

`[env:native]` builds for the PC, not a board, and is run with
`pio test -e native`, never `pio run`. It runs the C++ test suites listed in
`test_filter` with doctest.

It compiles with `-msse2 -mfpmath=sse`. The controller's FPU computes in
single precision; the local Windows compiler would otherwise keep
intermediates in 80-bit x87 registers, and results would differ from the
controller's. The tests themselves are covered on the tests page.

### Scratch envs

A scratch env compiles one file from `src/scratch/` and nothing else: no
node core, no bus. Each borrows a template only for its chip, clock and
upload settings.

* `hello_serial1` (DB32) and `hello_serial0_tiny` (ATtiny3226) print a banner
  on the console USART, to check a board's clock.
* `hall_capture` streams hall sensor samples from the A axis's node, for
  working out its index offline.

A scratch env inherits its template's clock. Pointed at a board with a
different clock, it needs the same `f_cpu` override as that board's envs.

### Upload

Every env uploads the same way:

```bash
pio run -e <env> -t upload
```

The env decides how: `upload_protocol` (`serialupdi` for the nodes,
`picotool` for the controller), `upload_port`, and for the ATtinys
`board_upload.speed`. Some envs set `upload_port` to a COM port on the
machine they were written on; on another machine, override it with
`--upload-port` or edit the env.

The controller has a second target for its filesystem:

```bash
pio run -e pico -t uploadfs
```

Before building the filesystem image, the pre-script `config_image.py`
encodes `custom_config_json` into `data/config.bin`, and
`custom_mesh_json`, if set, into `data/mesh.bin`. It does the encoding with
the web package's own encoder (`web/scripts/config-image.ts`,
`mesh-image.ts`), so it needs pnpm on the PATH and `pnpm install` run in
`web/`. `uploadfs` replaces the whole filesystem, so it overwrites any
config pushed to the controller since; a plain `upload` leaves the
filesystem alone.

## Glossary

* **env:** a `[env:<name>]` section; something PlatformIO builds.
* **template:** a non-env section an env borrows from with `extends`.
* **interpolation:** `${section.key}`, which pastes another section's value
  in place.
* **`pio`:** PlatformIO's command.
* **PIO:** Programmable I/O, the RP2350's state-machine block.
* **`f_cpu`:** the CPU clock the build assumes.
* **XDIR:** a USART pin that drives the RS485 transceiver's direction.
* **UPDI:** the AVRs' one-wire programming interface; `serialupdi` drives it
  from a USB serial adapter.

## Decisions

* **Interpolation, not chains of `extends`.** A template and a type both add
  sources and flags; `extends` would let only one of them. The alternative,
  a template per chip and type pair, multiplies sections with every type.
* **The board by include path, not by flag.** `-I src/node/board/<chip>`
  makes `board.h` mean that chip's header, with no `#if` chains in the
  source. A flag per chip would put every chip's pins in one header.
* **The config image built by the web's encoder.** The pre-script calls the
  same encoder the host uses to push a config, so the flashed blob and a
  pushed one can't drift. The cost is that `uploadfs` needs the web package
  installed.

## Code

* `platformio.ini`
* `scripts/pio/config_image.py`
* `src/rp2350/main.cpp`
* `src/rp2350/core1/bus/uart_9bit.pio`, `uart_9bit.pio.h`
* `src/node/board/<chip>/board.h`
