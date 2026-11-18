# Node Firmware

Firmware for RS485 bus slave nodes (stepper motors, future vacuum/knife/…).
Each node is a small AVR MCU that drives one physical axis or peripheral,
listening for command frames and (for motion types) stream bytes from the
RP2350 master over a 9-bit UART protocol at 921.6 kbaud.

---

## Directory layout

```
src/node/
  main.cpp               setup()/loop() skeleton — calls the hooks, drains the command ring
  dispatch.cpp           generic command table (PING/GET_TYPE/ENABLE/DISABLE), falls through to the type specific commands
  node_hooks.h           the core↔type contract (mandatory, non-weak; link-time guardrail)
  protocol.h             CommandPacket / cmdQueue shape (shared contract)

  rs485/                 9-bit UART transport (type-agnostic, always compiled)
    rs485.cpp              TX path, cmdQueue plumbing, sendCommandPacket()
    frame.h                static-inline command framer — inlined into whichever RX ISR is compiled
    isr_generic.cpp        RX ISR for non-motion types (ignores stream bytes)

  board/                 board-level config; ONE include path per build
    attiny3224/
      board.h              RS485/USART/LED (HAL_ symbols, type-agnostic)
      stepper/stepper.h    (attiny3224 × stepper) hardware binding: pins, timer, motor enable
    avr128db32/
      board.h              RS485/USART/RGB LED (HAL_ symbols, type-agnostic)
      stepper/stepper.h    (avr128db32 × stepper) binding: pins, timer, driver-chip select

  types/                 portable (board-agnostic) type logic; ONE compiled per build
    stepper/
      stepper.cpp          own RX ISR (stream stepping), TCB one-shot, position, GET_POS, hooks
      drivers.cpp          driver-chip init (TMC2660/DRV8825/DM542), #ifdef-selected; board via -I
```
---

## Directory Overview

### /

Core files that tie everything together. 

### board/

This directory deals with the two types of boards. It is split by generic board definition, `board.h` and type specific board definitions for eg `stepper.h`. So both board and type are accounted for.

### rs485/

Type and board agnostic rs485 comms. The bus has two planes: control plane and stream plane. The control plane is just for sending commands while the stream plane is used by the stepper type nodes for stepping. Generic implementation of the control plane is the same across standard and stepper types however the stepper type actually uses the stream plane.

### types/

Basically a definition for various types of nodes, like stepper nodes which are attached to the stepper motors, vacuum controller node which toggles the vacuum bed on and off etc. 

---

## Entry points

Program execution starts in `main.cpp` `setup()`. Setup does the generic setup and the calls the node hook to the setup specific to a node type. Then interrupts are enabled:

* USART RX Interrupt: This is for receiving commands in both control plane by default. Stepper nodes will handle stream commands as well.

* Timer interrupt: This is used by the stepper node in stream mode to toggle the step pin low after setting it high during stream byte execution.

After the `setup()` we have the `loop()`. Loop by default takes care of control plane commands that are already in the queue. Generic commands (all types can handle them) is checked first and if its not a generic command it falls down to node specific command through the node hooks in dispatch.

---

## Three orthogonal axes

Every node binary is the composition of exactly one choice from each column:

| Axis | Selects | Mechanism |
|---|---|---|
| **Board** | pins, peripherals, LED wiring | `-I` include path to `board/<board>/` |
| **Node type** | behavior (stepper, vacuum, …) | `build_src_filter` picks `types/<x>/` + `-DNODE_TYPE=` flag |
| **NODE_ID** | bus address (1–4) | `-DNODE_ID=<n>` per env |

A fourth dimension — **driver chip** (DM542 / DRV8825 / TMC2660) — is
stepper-only and selected by build flags (`-DDM542`, `-DTMC_2660`, etc.).
`types/stepper/drivers.cpp` uses `#ifdef` on these flags to compile only the
relevant init code; the board's `stepper.h` uses the same flags to select pin
config and motor-enable polarity.

Only one type and one board compile per binary. The link enforces this:
zero types → `undefined reference to node_handle_command`; two types →
`multiple definition`.

---

## Core↔type contract (`node_hooks.h`)

The node core (`main.cpp`, `dispatch.cpp`, `rs485/`) is type-agnostic. Each
type provides four mandatory hooks:

| Hook | Purpose |
|---|---|
| `node_type()` | Returns `NODE_TYPE_*` (answered by `CMD_GET_TYPE`) |
| `node_setup()` | Type-specific init (pins, drivers, timers, slot masks) |
| `node_set_enabled(bool)` | Effect of `CMD_ENABLE`/`CMD_DISABLE` (stepper: energize; vacuum: pump) |
| `node_handle_command(pkt, len, reply, &replyLen)` | Type-specific commands (0x20+); returns true if handled |

These are **non-weak** — a build with no type selected fails at link.

---

## Board headers (`board/<board>/`)

Each board provides two headers, selected by `-I`:

- **`board.h`** — type-agnostic: USART/RS485 config, LED pins. Defines `HAL_`
  symbols (`HAL_USART_INST`, `HAL_LED_PIN`, `HAL_USART_INIT()`, etc.) consumed
  by `main.cpp` and `rs485/rs485.cpp`.
- **`stepper/stepper.h`** — the (board × stepper) cell: step/dir/enable pins,
  timer peripheral, motor-enable polarity, driver-chip pin config. Defines
  `HAL_STEP_*`, `HAL_DIR_*`, `HAL_MOTOR_ENABLE/DISABLE()`, etc. consumed by
  `types/stepper/stepper.cpp` and `types/stepper/drivers.cpp`.

The `HAL_` prefix is cosmetic — it distinguishes board-provided symbols from
local variables. After preprocessing, `HAL_USART_INST.RXDATAH` *is*
`USART1.RXDATAH` — direct register access, zero indirection, safe inside ISRs.

### Adding a new board

1. Create `board/<board>/board.h` — define the `HAL_` symbols for USART/LED.
2. Create `board/<board>/stepper/stepper.h` — define step/dir/timer/motor
   `HAL_` symbols. Declare `void drivers_init(void);`.
3. Add a `[<board>_base]` section in `platformio.ini` with
   `-I src/node -I src/node/board/<board>`.
4. Add per-node `[env:…]` sections extending it with a type + `-DNODE_ID=n`.

---

## Two dispatch paths

| | Command frame (9th bit = 1) | Stream byte (9th bit = 0) |
|---|---|---|
| Path | RX ISR → `cmdQueue` → `loop()` → `dispatchCommand()` | RX ISR → step inline, in the ISR |
| Timing | deferred, latency-tolerant | synchronous — must step now |
| Integrity | CRC-checked in `loop()` | none (per-step, no room) |
| Reply | yes (ACK/PONG/…) | none |

### Why the stepper owns its RX ISR

The stream byte must move a step *now*, in interrupt context, at up to one
byte every ~12 µs. A core ISR calling a non-inlined `node_on_stream_byte`
hook would force the compiler to spill the call-clobbered register bank in
the ISR prologue (~15 registers, ~60 cycles ≈ 3 µs at 20 MHz) — paid on
*every* stream byte.

Resolution: **the stepper type owns its RX ISR** (`types/stepper/stepper.cpp`);
every non-motion type shares one generic RX ISR (`rs485/isr_generic.cpp`).
`build_src_filter` compiles exactly one per binary, so exactly one
`USART_RXC` vector exists (two would be a `multiple definition` link error).
The shared command-framing half (`frame.h`) is `static inline` — written
once, inlined into whichever ISR is compiled, no cross-TU call cost.

---

## Building

How `platformio.ini` builds a node: [docs/system/firmware-build.md](../../docs/system/firmware-build.md).

---

## Adding a new node type (e.g. vacuum)

1. Register the type in `include/common.h`:
   `#define NODE_TYPE_VACUUM 0x02`.
2. Create `types/vacuum/vacuum.cpp` — implement the four `node_hooks.h`
   functions. Include `rs485/frame.h` if the ISR needs command framing, or
   rely on `rs485/isr_generic.cpp` (non-motion types have no stream handler).
3. Create `board/<board>/vacuum/vacuum.h` for each board it runs on — define
   the cell's `HAL_` symbols for that type's peripherals.
4. Add a `[type_vacuum]` section in `platformio.ini`:
   ```ini
   [type_vacuum]
   build_src_filter = +<node/types/vacuum/vacuum.cpp> +<node/rs485/isr_generic.cpp>
   build_flags      = -DNODE_TYPE=NODE_TYPE_VACUUM
   ```
   Note `isr_generic.cpp` is pulled in here (not in `node_core`) because the
   stepper build excludes it to avoid a doubly-defined `USART_RXC` vector.
5. Add per-node envs interpolating `type_vacuum` instead of `type_stepper`.

---

## See also

- [include/common.h](../../include/common.h) — protocol constants, command IDs, CRC8/CRC32
