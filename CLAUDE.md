# ATtiny3224 × RP2350 RS485 CNC Motion Controller

**Last updated:** 2026-06-06

## Project Overview

Distributed stepper motor controller for CNC/plotter/fabrication machines. An RP2350 (Raspberry Pi Pico 2) acts as master host; up to 4 ATtiny3224 nodes act as slave stepper drivers, all connected via RS485 at 921.6 kbaud with a custom 9-bit UART protocol.

---

## Hardware Summary

| Component | Role | Key Pins |
|---|---|---|
| RP2350 (Pico 2) | Master host | TX GPIO4, RX GPIO5, DE GPIO6 |
| ATtiny3224 (×4) | Slave motor node | STEP PB0, DIR PB1, EN PA4 (active LOW) |
| MAX485E | RS485 transceiver | Per node |
| DM542 | Stepper driver | Step ≥2.5μs pulse, Dir ≥5μs setup |

**ATtiny clock:** 20 MHz | **RP2350 9-bit UART:** PIO state machines

---

## Directory Structure

```
src/
  rp2350/
    main.cpp          — global memory allocation, core launch
    core0.cpp         — USB serial CLI, command queuing, backpressure
    core1.cpp         — real-time RS485 streaming (time-critical)
    shared.h          — cross-core globals, Segment struct, ring buffer
    RS485Bus.h/.cpp   — PIO 9-bit UART abstraction
    uart_9bit.pio.h   — auto-generated PIO state machine header
  node/
    main.cpp          — slave node firmware (ISR-driven)
    isr.cpp           — interrupt handlers
    protocol.h        — node-side protocol
    attiny3224/       — ATtiny3224 board-specific config
    avr128db32/       — AVR128DB32 board-specific config + drivers
include/
  common.h            — protocol constants, CMD_* defines, CRC8
platformio.ini        — build targets: node1–node4 (ATtiny), pico (RP2350)
```

---

## Communication Protocol

### 9-Bit UART — Frame Type (bit 8)

| Bit 8 | Type | Direction | Response |
|---|---|---|---|
| 1 | Command frame | Master → Slave | Yes (CRC reply) |
| 0 | Stream frame | Master → Slave | None |

### Command Packet Structure
```
[Node_ID 1B] [CMD 1B] [Payload_Len 1B] [Payload…] [CRC8 1B]
```

### Stream Byte (one byte per step event, 9th bit = 0)
```
Bits 1-0: motor 1 (dir | step)
Bits 3-2: motor 2
Bits 5-4: motor 3
Bits 7-6: motor 4
```

### Commands (`include/common.h`)
| Constant | Value | Description |
|---|---|---|
| CMD_PING | 0x01 | Heartbeat → expects CMD_PONG |
| CMD_PONG | 0x02 | Ping response from slave |
| CMD_GET_POS | 0x03 | Query 4-byte signed absolute position |
| CMD_ENABLE | 0x04 | Enable motor + stream processing |
| CMD_DISABLE | 0x05 | Disable motor + stream processing |

**CRC:** CRC-8, polynomial 0x8C (industrial variant)  
**Command timeout:** 20 ms

---

## Key Data Structure — `Segment` (`src/rp2350/shared.h`)

```cpp
struct Segment {
    uint8_t  numMotors;    // 1–4
    uint8_t  nodeId[4];    // node addresses (1–4)
    uint32_t steps[4];     // steps per motor
    bool     cw[4];        // direction (true = clockwise)
    float    v_entry;      // entry velocity (steps/sec)
    float    v_cruise;     // cruise velocity
    float    v_exit;       // exit velocity
    float    accel;        // acceleration (steps/sec²)
};
```

**Motion ring buffer:** 128 entries (`MASTER_BUF_SIZE`), low-water mark 96 (`MASTER_BUF_LOW_WATERMARK`). Lock-free via atomic head/tail + `__dmb()` barriers.

---

## RP2350 Dual-Core Architecture

### Core 0 — CLI & Command Queue (`src/rp2350/core0.cpp`)
- Reads USB serial at 115,200 baud
- Parses commands:
  - **Control** (`stop`, `unalarm`): sets volatile flags instantly
  - **Streaming** (`move …`): pushes `Segment` into ring buffer; replies `"nope"` if full, `"ready"` when level drops below watermark
  - **Non-streaming** (`ping`, `enable`, `disable`, `getpos`): pushed to multicore FIFO for Core 1 to send on RS485
- Reads Core 1 FIFO responses and prints results to USB serial

**`move` command syntax:**
```
move <numMotors> <nodeId…> <steps…> <v_entry> <v_cruise> <v_exit> <accel>
# e.g.: move 2 1 2 1000 500 0.5 35 10 100
```

### Core 1 — Real-Time RS485 Engine (`src/rp2350/core1.cpp`)
- Marked `__time_critical_func` (executes from SRAM)
- Pulls segments from ring buffer, computes per-step kinematics:
  - Velocity at step N: `v = sqrt(v_entry² + 2·accel·distance)`
  - Step interval: `F_CPU / v_current` (cycle-accurate)
- **Bresenham multi-axis sync:** major axis always steps; minor axes use error accumulator (`error_init = maxSteps/2`)
- Packs direction+step bits per motor into one stream byte, sends with 9th bit = 0
- Polls FIFO for single-packet commands from Core 0, sends them with 9th bit = 1, awaits response ≤20 ms
- Emergency stop: `emergencyStop` flag clears buffer and exits immediately

---

## Slave Node (`src/node/main.cpp`)

**Node ID** set at compile time: `-DNODE_ID=<1–4>` (platformio.ini per-env flag)

### Interrupt Handlers
- **USART1_RXC ISR:** classifies byte by 9th bit
  - Stream byte: extracts this node's 2 bits, applies direction with 5 μs setup guard, pulses STEP high, starts TCB0 (3 μs one-shot)
  - Command byte: accumulates into `cmdQueue[4]`, stream traffic resets parse state
- **TCB0_INT ISR:** pulls STEP low after 3 μs (satisfies DM542 ≥2.5 μs minimum)

### Main Loop
- Validates CRC + node ID from assembled packet
- Executes: PING → PONG reply; GET_POS → 4-byte position; ENABLE/DISABLE → sets `streamEnabled`, drives EN pin

### Position Tracking
- `absolutePosition` (int32): incremented/decremented in stream ISR
- Read with double-check technique (lock-free safe for 32-bit on 8-bit MCU)

---

## Build Targets (`platformio.ini`)

| Env | MCU | Core | Upload |
|---|---|---|---|
| `node1`…`node4` | ATtiny3224 @ 20 MHz | megaTinyCore | Serial UPDI |
| `pico` | RP2350 | earlephilhower/arduino-pico | picotool |

---

## Performance

| Parameter | Value |
|---|---|
| RS485 baud | 921,600 |
| Max step rate | ~30,000 steps/sec per motor |
| Command timeout | 20 ms |
| Motion buffer | 128 segments |
| ATtiny ISR jitter | <1 μs |

---

## Active Branch: `motion-plan`

Current work-in-progress on this branch includes:
- Motion planning utilities (`svg_to_moves.py`, `plot_moves.py`, `plot_scrollable.py`)
- G-code / NC file toolchain (`*.nc` files: snake, star, circle, triangle test paths)
- Test output images for verifying path generation
- Core RS485 firmware updated: multicore FIFO refactor, enable/disable/getpos commands added

---

## Conventions & Gotchas

- **9th bit = 1 demands a response; 9th bit = 0 does not.** Never send a stream byte and wait for a reply.
- **Direction setup time:** ATtiny enforces a 5 μs guard before a step after direction change — do not step immediately.
- **CRC must be correct** on every command packet; slave silently drops malformed packets.
- **`streamEnabled` must be set** via CMD_ENABLE before any stream bytes are processed by the slave.
- **Emergency stop** (`stop` USB command) is handled synchronously on Core 0 and asynchronously clears Core 1 buffer via `emergencyStop` volatile flag — do not rely on in-flight motion completing.
- **Ring buffer backpressure:** Core 0 will block-reply `"nope"` when buffer ≥ 96/128 full; host should retry or throttle.
