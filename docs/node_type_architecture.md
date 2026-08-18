# Node Type Architecture

**Branch:** `node-types`
**Date:** 2026-07-15
**Status:** IMPLEMENTED. The core↔type split, HAL contract, and build-system
composition described here are live. Sections 5 and 7 note where the
implementation refined the original design. Captures the plan for
generalizing the node firmware from "stepper-only" to a typed multi-node bus.

Single source of truth for how the RS485 bus grows beyond stepper axes
(oscillating knife, vacuum, tool-changer, …) without forking the firmware per
node type. Cross-links:
[wire_protocol.md](wire_protocol.md) (host↔Pico framing),
[../web/src/machine/index.ts](../web/src/machine/index.ts) (host-side node/machine model).

---

## 1. Problem

The node firmware today is stepper-shaped end to end: `loop()` in
[src/node/main.cpp](../src/node/main.cpp) is a flat `switch (cmdId)` where every
command assumes a motor, and the stream byte's 4×2-bit packing hardwires
"node 1–4 = axis slot" in [src/node/isr.cpp](../src/node/isr.cpp)
(`stepBitMask = 1 << ((NODE_ID-1)*2)`).

We are adding other node types on the same bus. Each type has its own commands,
but all types share a common core (RS485 framing, CRC, addressing, a handful of
generic commands). We want **core logic + type-specific logic**, selected at
build time, with no per-type fork of the shared code.

Two independent concerns fall out and are treated separately:

- **Command plane** — node types, generic vs type-specific commands, dispatch.
  (Sections 2–6.) Low-risk, do first.
- **Motion/stream plane** — driving >4 steppers / dual heads within the 4-wide
  stream byte. (Section 7.) Only stepper nodes care.

---

## 2. Node type identity — the host↔firmware contract

Canonical numeric registry in [include/common.h](../include/common.h) (the one
file both planes already share), mirrored on the host in `config.ts` next to
`ToolType` (which already uses this exact stable-byte-value pattern):

```c
// include/common.h — canonical, mirrored by host
#define NODE_TYPE_STEPPER   0x01
#define NODE_TYPE_VACUUM    0x02
#define NODE_TYPE_KNIFE_OSC 0x03  // oscillating knife controller
```

`CMD_GET_TYPE` returns this byte. The orchestrator enumerates the bus and
validates each node's reported type against config — cheap insurance against a
miswired or misflashed node before a job runs.

### Host side (`config.ts`) — IMPLEMENTED

`BusNode` today carries `role: string` (`"stepper"`, peripheral roles).
**Decision: `type: NodeType` (a numeric mirror of the firmware enum) becomes the
contract, and `role` is retired.** `role` was a stringly-typed shadow of what
`type` states precisely; a single typed field removes the drift risk between the
two. `ToolProfile.requiredPeripheralRoles` becomes `requiredPeripheralTypes` and
matches on `NodeType`. The config loader validates each node's `CMD_GET_TYPE`
reply against its declared `type` at connect time.

**Companion cleanup — `nodeId` → `id`.** `BusNode.nodeId` stammers (the field is
already *on* `BusNode`, so `node.nodeId` restates the noun); rename to `id`, so
axis/peripheral references read `node.id`. **Decided**, but deferred to land with
the config feed/accel migration rather than as a second standalone wide rename.
(A standing TODO to this effect already sits in `config.ts`.)

---

## 3. Command ID namespacing

Split the `0x00–0xFF` command space:

| Range | Class | Handled by | Notes |
|---|---|---|---|
| `0x01–0x1F` | **Generic** | core | Every node type must honor. `CMD_PING`/`PONG`, `CMD_GET_TYPE`, `CMD_ENABLE`, `CMD_DISABLE`. |
| `0x20+` | **Type-specific** | node type | Values **may overlap between types** — only one type is ever compiled into a binary. A knife's `CMD_SET_OSC_FREQ` and a stepper's `CMD_SET_POS` can share a byte value. |

`CMD_GET_POS` and the stepper `ENGAGE` command are **type-specific** (motion
only) — they move out of core into the stepper type.

---

## 4. Firmware code layout

Three orthogonal axes. Keeping them independent is what stops build
environments from exploding combinatorially.

| Axis | Selects | Mechanism |
|---|---|---|
| **Board** | pins / peripherals (`attiny3224`, `avr128db32`) | `-I` include path to `board/<board>/` (`board.h` + per-type binding) + `-I src/node/board` for the HAL contract |
| **Driver chip** | `DM542` / `TMC2660` (stepper only) | build flags (already how it works) |
| **Node type** | behavior (stepper, vacuum, …) | `build_src_filter` picks `types/<x>/` + `-DNODE_TYPE=` identity flag |
| **NODE_ID** | bus address | build flag (per upload) |

The node-core / command-plane files stay flat at the `src/node/` root — no
`core/` wrapper directory. The RS485 transport is now three files, so it earns its
own `rs485/` folder; the type and board dirs are their own folders as before. Only
one type and one board compile per build.

```
src/node/
  main.cpp             ← setup()/loop() skeleton; calls the hooks   (node core)
  dispatch.cpp         ← handleGenericCommand(); falls through to node hook
  node_hooks.h         ← the core↔type contract (declarations)
  protocol.h           ← CommandPacket / cmdQueue shape (shared contract)
  rs485/               ← 9-bit UART transport (bus-level, type-agnostic)
    rs485.cpp            ← TX, cmdQueue plumbing, sendCommandPacket   (always compiled)
    frame.h              ← static inline frame_command_byte() (RX command framing)
    isr_generic.cpp      ← generic RX ISR for non-motion types (excluded from stepper build)
  board/               ← board-level config + HAL contract; ONE include path per build
    hal/                 ← the board↔consumer contract (mirrors node_hooks.h)
      hal.h              ← umbrella: #include usart.h + led.h (board-level self-check)
      hal_stepper.h      ← umbrella: #include step.h + motor.h + extras.h (cell self-check)
      usart.h            ← RS485/USART contract + #error guards
      led.h              ← LED contract (monochrome + optional RGB)
      step.h             ← step/dir/timer contract + #error guards
      motor.h            ← enable/disable driver contract + drivers_init() decl
      extras.h           ← limit switch, thermistor (feature-flagged)
      motor.cpp          ← weak default for drivers_init() (always compiled)
    attiny3224/
      board.h            ← type-agnostic: RS485/USART/LED; #include hal/hal.h at end
      stepper/stepper.h  ← the (attiny3224 × stepper) hardware binding; #include hal/hal_stepper.h
    avr128db32/
      board.h
      stepper/
        stepper.h        ← (avr128db32 × stepper) binding + driver-chip select
        drivers.cpp      ← TMC SPI impl (pulled into the stepper composition)
  types/               ← portable (board-agnostic) type logic; ONE compiled per build
    stepper/stepper.cpp  ← own RX ISR (stream), ENGAGE, position, GET_POS, ENABLE=energize motor
    vacuum/vacuum.cpp
```

The stepper's RX ISR lives in `types/stepper/` (not `rs485/`) because it's the one
ISR that is type-specific — it inlines the stream path and includes
`rs485/frame.h` for the shared command-framing half. Every non-motion type uses
`rs485/isr_generic.cpp` instead.

**The HAL contract (`board/hal/`) is the board-side mirror of `node_hooks.h`.**
Just as `node_hooks.h` pins the core↔type interface, the HAL headers pin the
board↔consumer interface: every symbol the portable code reaches for is
contracted with `#error` self-check guards. A new board provider is a
checklist, not a grep hunt. The HAL is **macro-based, not function-based** —
after preprocessing, `HAL_USART_INST.RXDATAH` *is* `USART1.RXDATAH`, identical
machine code, zero indirection, safe inside ISRs without register-spill.

**Config is split along the same concern boundary.** The rule is *"does this
change when you swap the board?"* — step-pulse logic doesn't (→ `types/stepper/`),
but the STEP pin, port/bitmask, `STEP_PULSE_CCMP`, timer peripheral, and driver
chip do (→ `board/<board>/stepper/`). So the `(board × type)` hardware binding
is a **grid cell**: `board/<board>/stepper/` mirrors `types/<type>/`, one filed
by the board the build selects (via `-I`), the other board-agnostic. `board.h`
at the board root holds what every type on that board shares (RS485/USART/LED);
a future vacuum node adds `board/<board>/vacuum/` beside it and reuses
`board.h`. The type's portable code names no board — it `#include "board.h"` +
`#include "stepper/stepper.h"`, both resolved by `-I src/node/board/<board>`.

The critical payoff: **the stream byte becomes a type hook, not core logic.** A
vacuum node's stream handler is empty — it is simply not a motion participant.
The `ENGAGE`/slot mechanism is therefore a *stepper* feature, not something baked
into every node.

---

## 5. The core↔type hook contract

One shared header both sides include, so declaration and definition cannot drift:

```c
// src/node/node_hooks.h — the core↔type contract, single source
#pragma once
#include <stdint.h>

uint8_t node_type(void);                 // returns NODE_TYPE_*
void    node_setup(void);                // type-specific init, called from setup()
void    node_set_enabled(bool on);       // ENABLE/DISABLE *effect* (motor vs pump …)
bool    node_handle_command(const uint8_t* pkt, uint8_t len,
                            uint8_t* reply, uint8_t* replyLen);
// NOTE: node_on_stream_byte() was originally listed here but is NOT in the
// implementation — the stream byte is handled inside the type's own RX ISR
// (see §6, "The stream byte is handled in the ISR"). The ISR is per-type, so
// no stream hook is needed.
```

Design notes:

- `ENABLE`/`DISABLE` are **recognized generically** (uniform command + ACK
  framing) but **delegate the effect** via `node_set_enabled()` — a stepper
  energizes its motor, a vacuum node spins its pump.
- `node_type()` and `PING` are **fully core** — universal in both recognition
  and implementation.
- These hooks are **non-weak / mandatory.** A node with no type is a build
  mistake and must fail at link (contrast `drivers_init()`, which is
  `__attribute__((weak))` — a no-op default lives in `board/hal/motor.cpp`,
  always compiled; a board with real driver init provides a strong override).

---

## 6. Dispatch and the fall-through

Core tries the generic table first; **not recognizing a command ID is the signal
to hand it to the type hook.** Because only one type's `.cpp` is compiled in, the
0x20+ overlap is safe — there is only ever one `node_handle_command` linked.

```c
// src/node/dispatch.cpp — type-agnostic command routing
#include <Arduino.h>
#include "../include/common.h"
#include "protocol.h"
#include "node_hooks.h"
#include "rs485/rs485.h"        // sendCommandPacket

// Reply convention: reply[] holds [id][cmd][payloadLen][payload…]; replyLen
// counts through the trailing CRC slot, which sendCommandPacket fills in.
static void replyAck(uint8_t cmd, uint8_t* reply, uint8_t* replyLen) {
    reply[0] = NODE_ID; reply[1] = cmd; reply[2] = 0;
    *replyLen = 4;
}

static bool handleGenericCommand(const uint8_t* pkt, uint8_t len,
                                 uint8_t* reply, uint8_t* replyLen) {
    switch (pkt[1]) {                    // pkt[1] = cmdId
        case CMD_PING:
            reply[0] = NODE_ID; reply[1] = CMD_PONG; reply[2] = 0;
            *replyLen = 4; return true;

        case CMD_GET_TYPE:
            reply[0] = NODE_ID; reply[1] = CMD_GET_TYPE; reply[2] = 1;
            reply[3] = node_type(); *replyLen = 5; return true;

        case CMD_ENABLE:
            node_set_enabled(true);  replyAck(CMD_ENABLE,  reply, replyLen); return true;
        case CMD_DISABLE:
            node_set_enabled(false); replyAck(CMD_DISABLE, reply, replyLen); return true;

        default:
            return false;                // ← the fall-through: not generic
    }
}

// core/main.cpp loop() calls this once node-id + CRC have passed.
void dispatchCommand(const uint8_t* pkt, uint8_t len) {
    uint8_t reply[MAX_PACKET_LEN];
    uint8_t replyLen = 0;

    bool handled = handleGenericCommand(pkt, len, reply, &replyLen);
    if (!handled)
        handled = node_handle_command(pkt, len, reply, &replyLen);

    if (handled && replyLen)
        sendCommandPacket(reply, replyLen);
    // else: unknown command → silently dropped, same as a bad-CRC packet today
}
```

`loop()` collapses to the ring drain, with no stepper knowledge in core:

```c
void loop() {
    if (cmdHead == cmdTail) return;
    CommandPacket* pkt = &cmdQueue[cmdTail];

    bool validNode = (pkt->data[0] == NODE_ID || pkt->data[0] == 0xFF);
    bool validCrc  = (pkt->data[pkt->length - 1] == crc8(pkt->data, pkt->length - 1));
    if (validNode && validCrc)
        dispatchCommand(pkt->data, pkt->length);

    cmdTail = (cmdTail + 1) % MAX_COMMANDS;
}
```

### Why the linker resolves `node_handle_command` with no `#include`

- **Compile time:** `dispatch.cpp` needs only the *declaration* (from
  `node_hooks.h`). It emits a call to an **undefined symbol** and moves on.
- **Link time:** the *definition* lives in `types/stepper/stepper.o`, which is in
  the build because `build_src_filter` pulled it in. The linker matches the
  undefined symbol to it.

Failure modes are loud and at build time:

| Situation | Result |
|---|---|
| Zero type dirs compiled | `undefined reference to node_handle_command` — cannot flash a typeless node |
| Two type dirs compiled | `multiple definition` — enforces "exactly one type per binary" |

Both files are C++, so the symbol is name-mangled from the full signature —
declaration and definition must match exactly (down to `const`). The shared
`node_hooks.h` guarantees this. No `extern "C"` needed.

### Unknown-command policy

Both layers declining → **silent drop**, consistent with today's silent
bad-CRC/foreign-packet drops. The master already knows each node's type (config +
`GET_TYPE`) so it should never send an unsupported command. If we later want the
master informed, the single change is staging a generic `CMD_NACK` (unsupported
reason) in `dispatchCommand` when both handlers decline.

### Two dispatch paths: commands vs stream

`dispatchCommand` is only *half* the story — it handles **command frames
(9th bit = 1)**. **Stream bytes (9th bit = 0)** take a completely separate path,
because the two have opposite timing requirements:

| | Command frame (9th bit = 1) | Stream byte (9th bit = 0) |
|---|---|---|
| Path | RX ISR → `cmdQueue` → `loop()` → `dispatchCommand` | RX ISR → step *inline*, in the ISR |
| Timing | deferred, latency-tolerant | synchronous — must step *now* |
| Integrity | CRC-checked in `loop()` | none (per-step, no room) |
| Reply | yes (ACK/PONG/…) | none |

#### The stream byte is handled in the ISR — so the ISR itself is per-type

The stream byte must move a step *now*, in interrupt context, at up to one byte
every ~12 µs. That rules out the earlier "core ISR calls a `node_on_stream_byte`
hook" sketch: on AVR an ISR calling a **non-inlined external function** forces the
compiler to spill the call-clobbered register bank in the ISR prologue (~15
registers, ~60 cycles ≈ 3 µs at 20 MHz) — paid on *every* stream byte, even when
the node early-returns. Too expensive on the hottest path.

Resolution: **the stepper type owns its own RX ISR; every non-motion type shares
one generic RX ISR.** `build_src_filter` compiles exactly one of them per build,
so exactly one `USART_RXC` vector exists in any binary (two would be a
`multiple definition` link error — a useful guardrail). The stream handling is
written *in the same TU as the ISR*, so it inlines with no register-spill and no
LTO dependency.

The only shared part — the type-agnostic **command framing** — goes in a
`static inline` header so it's written once and inlined into whichever single ISR
is compiled. No cross-TU call, no binary duplication (only one ISR per build):

```c
// rs485/frame.h — written once, inlined into whichever ISR is compiled
static inline void frame_command_byte(uint8_t status, uint8_t b) { /* cmdQueue framing */ }

// rs485/isr_generic.cpp — compiled for every NON-stepper type
ISR(USART_RXC_vect_) {
    uint8_t s = NODE_USART.RXDATAH, b = NODE_USART.RXDATAL;
    if (s & 0x01) frame_command_byte(s, b);   // stream bytes: ignored
}

// types/stepper/stepper.cpp — compiled for the stepper type only
ISR(USART_RXC_vect_) {
    uint8_t s = NODE_USART.RXDATAH, b = NODE_USART.RXDATAL;
    if (s & 0x01) { frame_command_byte(s, b); return; }

    // 9th bit = 0 → stream byte. ENGAGE/slot state is stepper-private.
    if (!engaged) return;                 // ENGAGE gate (slot set by CMD_ENGAGE)
    bool stepReq = b & stepBitMask;       // slot decode
    bool newDir  = b & dirBitMask;
    // …dir handling, STEP pulse, absolutePosition, TCB0 one-shot… (today's logic)
}
```

**Build wiring:** the shared TX / `cmdQueue` / `sendCommandPacket` live in an
always-compiled `rs485/rs485.cpp`. The *ISR provider* is per-type-category in
`build_src_filter`: the stepper build pulls `types/stepper/stepper.cpp`; every
other build pulls `rs485/isr_generic.cpp`. `rs485/isr_generic.cpp` must be
**excluded from the stepper build** or the vector is doubly defined — e.g.
`+<node/rs485/*.cpp> -<node/rs485/isr_generic.cpp> +<node/types/stepper/*.cpp>`.

This retires the whole `-flto`-on-the-hot-path discussion: inlining is guaranteed
by same-TU compilation, not by a link-time optimizer whose interaction with weak
symbols and ISR vectors is finicky. (LTO remains fine to enable for code-size
elsewhere; it's just no longer load-bearing for stream latency.)

---

## 7. Stream slots and dual heads (`ENGAGE`) — stepper-only

> **Now specced in [engage_and_axis_map.md](engage_and_axis_map.md).** That doc
> is the source of truth for the ENGAGE / `axis_map` work; the sketch below is
> the original framing it grew out of.

The hard limit on >4 steppers is the **stream byte** (4 motors × 2 bits), not the
command layer. Each node's slot is hardwired from `NODE_ID` today. `CMD_ENGAGE`
makes the slot **runtime-assigned state**, decoupling stream slot from bus
address. This unlocks dual heads with **no wire-format change**: `engage 1 2 5 6`
cuts head A, `engage 1 2 7 8` cuts head B, same 4-wide stream byte.

- **CLI ergonomic form:** `engage 1 2 5 6` — list position = slot index (node 5 →
  slot 2 → reads bits 4 and 5).
- **On the wire:** master translates the list into **one addressed packet per
  node** — `[id][CMD_ENGAGE][len=1][slot][crc]` — so each `ENGAGE` gets an ACK
  (a stream byte carries no node id; a dropped engage that silently moved the
  wrong motor must be catchable). A broadcast list can't confirm — four nodes
  replying would collide.
- **Disengage:** omit from the list (explicit "no slot", e.g. `slot = 0xFF`) or a
  bare `disengage`. A node with no slot ignores stream bytes → its
  `absolutePosition` freezes, correct for a parked axis.
- **`ENGAGE` is orthogonal to `ENABLE`.** `ENABLE` = motor energized (holding
  torque); `ENGAGE` = reads the stream at slot N. A parked head stays energized
  but disengaged (holds its Z height while the other head cuts). Both are needed
  to actually move.
- **Timing / safety:** re-engaging mid-stream corrupts in-flight motion, so a head
  switch happens at a bus-idle boundary — the PAUSED tool-change boundary the
  protocol already defines (see [wire_protocol.md](wire_protocol.md)). Engage the
  new set while PAUSED, ACK all four, then resume.
- **Master side:** Core 1's stream packer is unchanged — the logical
  axis→slot map (X→0, Y→1, Z→2, A→3) stays fixed; `ENGAGE` only rebinds which
  physical node occupies each slot. The MSEG format (dx/dy/dz/da) is untouched.
  The master owns both the per-node slot assignment *and* the packer, so slot
  ordering has a single source of truth on the Pico.

**Requires wider frame (deferred):** only fully-independent simultaneous motion
of >4 axes (both heads cutting different geometry at once). Alternating and
ganged/mirrored heads both fit the 4-wide byte via `ENGAGE`.

### PROPOSAL (not strictly necessary): pipelined DIR to remove the in-ISR delay

**Status: not implemented, not required.** The current design applies a direction
change with a `delayMicroseconds(5)` busy-wait inside the RX ISR to satisfy the
DM542's ≥5 µs DIR-before-STEP setup. That is **correct** — the only cost is
stalling the ISR for ~5 µs, and only on an actual direction change, which occurs
at path reversals where the step rate is already low and the bus is not
saturated. So the delay is a rare, bounded latency blip, not a correctness or
throughput problem. This proposal removes it; implement only if that ISR stall
ever proves to matter.

The idea: the ≥5 µs setup fits entirely inside the inter-byte gap. At 921.6 kbaud
a 9-bit frame is ~11.9 µs; the STEP pulse is 3 µs, leaving **~8.9 µs** before the
next step edge — comfortably over 5 µs, with ~4 µs margin for ISR jitter. So DIR
can be flipped *after* the current STEP pulse falls and still be stable well
before the next step.

Mechanics:

- **Semantic shift — the DIR bit leads its step by one.** Today the invariant is
  "DIR bit in byte K = direction of byte K's step." This proposal breaks it on
  purpose: byte K's step runs in the *current* direction, and byte K's DIR bit
  sets up the *next* step (a one-step pipeline). The master's Core 1 packer emits
  each run's new direction one motor-step early — a trivial one-step lookahead in
  the pre-computed stream.
- **Node side.** DIR must not change while STEP is high, so the flip is deferred
  to the **TCB0 one-shot ISR** (the handler that already pulls STEP low at 3 µs):
  the RX ISR stashes a `pendingDir`; TCB0_INT applies it right after
  `STEP_PORT.OUTCLR`. No busy-wait. The RX→TCB0 gap (3 µs) vs the next byte
  (~11.9 µs) leaves no race.
- **Stream start.** There is no "previous byte" to have preset the first
  direction, so the master leads a run's first stepping byte with DIR already
  established (a dir-only preset byte, or seed `currentDir` at enable/engage).
- **Fallback.** Keep the 5 µs guard for the "DIR must change *this* step" path so a
  packer bug degrades to a one-off stall rather than a wrong-direction step.

---

## 8. Build system

`build_src_filter` and `-DNODE_TYPE` do different jobs, and you want both:

- **`build_src_filter` selects behavior** — *which* `types/<x>/` and
  `board/<board>/<x>/` files compile (e.g. `board/avr128db32/stepper/drivers.cpp`).
  Physically excludes the stepper stream ISR from a vacuum binary.
- **`-DNODE_TYPE=NODE_TYPE_*` is identity only** — so `node_type()` reports the
  right byte and a `static_assert` can check the compiled type matches the flag.
  **Never `#ifdef`-branch behavior on it** — that's the src filter's job.

Compose with **`${section.option}` interpolation**, not `extends`. PlatformIO's
`extends` *overrides* a repeated key (e.g. `build_src_filter`) rather than
concatenating, so a type and an board base can't both contribute filter fragments
via `extends` alone. Instead, name the reusable pieces in plain sections and
interpolate them. The always-compiled core/transport (including the HAL's weak
`drivers_init` default) is one section; each type is another carrying its source
fragment and `NODE_TYPE` flag; the board base stitches them together:

```ini
[node_core]
build_src_filter = +<node/main.cpp> +<node/dispatch.cpp> +<node/rs485/rs485.cpp> +<node/board/hal/motor.cpp>

[type_stepper]
build_src_filter = +<node/types/stepper/stepper.cpp>
build_flags      = -DNODE_TYPE=NODE_TYPE_STEPPER

[attiny_base]
build_src_filter =
    -<*>
    ${node_core.build_src_filter}
    ${type_stepper.build_src_filter}
build_flags = -I src/node -I src/node/board -I src/node/board/attiny3224 ${type_stepper.build_flags}

[env:node1]
extends     = attiny_base
build_flags = ${attiny_base.build_flags} -DNODE_ID=1
```

The `-I src/node/board` path is what lets each provider's `board.h` and
`stepper.h` resolve `#include "hal/hal.h"` and `#include "hal/hal_stepper.h"` —
the contract headers that self-check completeness at the end of every provider.

Every node on the bus is currently a stepper, so the board bases interpolate
`type_stepper` directly. A future non-motion type defines its own
`[type_vacuum]` (source `+<node/types/vacuum/*.cpp>` **plus**
`+<node/rs485/isr_generic.cpp>` for the generic RX ISR, and
`-DNODE_TYPE=NODE_TYPE_VACUUM`) and a board base that interpolates it instead —
never listing `types/stepper/`, so the stepper's RX ISR is excluded and the
`USART_RXC` vector stays singly defined.

`NODE_ID` stays a per-upload flag (flash-storing it to make it runtime would
collapse many envs — a separate discussion).

---

## 9. Worked proxy: vacuum hold-down node

Chosen because it is maximally *unlike* a stepper — if the split survives it, it
is real.

- **Not a motion participant** → empty `node_on_stream_byte`, never sent
  `ENGAGE`. Proves stream handling is not core.
- **Generic set, different meaning** → `ENABLE`/`DISABLE` power the pump instead
  of energizing a motor (via `node_set_enabled`). Proves generics are
  behaviorally abstract.
- **Genuinely type-specific commands** → `CMD_SET_LEVEL <setpoint>` (0x20+),
  `CMD_GET_PRESSURE` → ADC readback. Exercises `node_handle_command` and a
  non-PONG response path.
- **Ties into config** → a `BusNode` of `type: VACUUM`; a KNIFE tool's
  `requiredPeripheralRoles` can require it; pre-flight checks it is present and of
  the right type before a job that needs hold-down.

An oscillating-knife node is the same shape (`ENABLE`=power, `CMD_SET_OSC_FREQ`,
`CMD_GET_OSC_STATE`) — if vacuum fits, knife/laser/tool-changer fall out of the
same mold.

---

## 10. Open decisions

1. ~~**`role` vs `type` in `config.ts`**~~ — **DECIDED + IMPLEMENTED (host)**
   (Section 2): `type` absorbs `role`; `role` retired,
   `requiredPeripheralRoles` → `requiredPeripheralTypes`.
1a. ~~**`nodeId` → `id` in `BusNode`**~~ — **DECIDED** (Section 2): redundant
   `nodeId` renamed to `id`; deferred to land with the config feed/accel
   migration.
2. **Unknown-command policy** (Section 6) — silent drop (current) vs generic
   `CMD_NACK`.
3. **Runtime `NODE_ID`** (Section 8) — flash-stored address to collapse
   per-node-id build envs. Separate effort.
4. **Wider stream frame** (Section 7) — only if fully-independent simultaneous
   multi-head motion is ever required.
5. **Pipelined DIR** (Section 7 proposal) — remove the in-ISR `delayMicroseconds(5)`
   via a one-step DIR lead. Not strictly necessary; implement only if the ISR
   stall on direction changes ever proves to matter.
