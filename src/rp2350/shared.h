#pragma once
#ifndef SHARED_H
#define SHARED_H

#include <Arduino.h>
#include <stdint.h>
#include "common.h"

// ─── Pins ──────────────────────────────────────────────────────────────────────
#define RS485_TX_PIN  4
#define RS485_RX_PIN  5
#define RS485_EN_PIN  6

// ─── Machine config ────────────────────────────────────────────────────────────

// #define NUM_MOTORS           4 
// static const uint8_t MOTOR_ADDRESSES[NUM_MOTORS] = {3, 2, 1, 4}; // x, y, z, a

// #define NODE_X  3
// #define NODE_Y  2
// #define NODE_Z  1
// #define NODE_A  4

// #define NODE_X  1
// #define NODE_Y  2
// #define NODE_Z  3
// #define NODE_A  4

// GT2 belt + 20-tooth pulley
// Set MICROSTEP to match your DRV8825 jumpers (1/2/4/8/16/32)
// #define BELT_PITCH_MM   2.0f
// #define PULLEY_TEETH    20
// #define MOTOR_FULL_SPS  200
// #define MICROSTEP       32
// #define MM_PER_REV      (BELT_PITCH_MM * PULLEY_TEETH)              // 40 mm
// #define STEPS_PER_MM    (MOTOR_FULL_SPS * MICROSTEP / MM_PER_REV)   // 80 steps/mm
// #define DEFAULT_SPD_MM_S  35.0f   // used when speed arg omitted

// // Pen Z axis
// #define PEN_DOWN_MM    2.0f
// #define PEN_UP_MM      2.0f
// #define PEN_SPEED_SPS  800

#define MASTER_BUF_SIZE          512
#define MASTER_BUF_LOW_WATERMARK 384

// ─── MicroSegment ─────────────────────────────────────────────────────────────
// One pre-computed step event produced by the host PC and consumed by Core 1.
// All kinematics are resolved on the PC; the Pico is a dumb step emitter.
//
// dx/dy/dz/da: signed step counts per axis for this segment (major axis = 1).
// interval:    time to wait before emitting, in RP2350 CPU cycles.
// flags:       MSEG_FLAG_* bitmask (see below).

#define MSEG_FLAG_NONE      0x00
#define MSEG_FLAG_PATH_END  0x01  // Last segment in a path — Core 1 can signal idle
#define MSEG_FLAG_ESTOP     0x02  // Poison pill — flush and halt immediately

struct MicroSegment {
    int32_t  dx;        // X axis steps (signed)
    int32_t  dy;        // Y axis steps (signed)
    int32_t  dz;        // Z axis steps (signed, +lift / -lower)
    int32_t  da;        // A axis steps (signed, tangential rotation)
    uint32_t interval;  // Step interval in CPU cycles (major axis timing)
    uint8_t  flags;     // MSEG_FLAG_* bitmask
    uint8_t  pad[3];    // Alignment padding — total struct size = 24 bytes
};

// ─── USB Wire Packet ──────────────────────────────────────────────────────────
// Binary packet framing for MicroSegments sent from host PC over USB CDC.
//
// Layout (26 bytes total):
//   [0]      magic  = 0xAB
//   [1..24]  MicroSegment (24 bytes, little-endian)
//   [25]     CRC8 over bytes [0..24]

#define MSEG_MAGIC       0xAB   // host production  — pre-computed step events
#define TILE_MAGIC       0xAD   // local production — SplineTile geometry packets
#define TOOL_MAGIC       0xAC   // local production — ToolConfig packets
#define MSEG_PACKET_SIZE 26     // magic(1) + MicroSegment(24) + CRC8(1)

// ACK/NACK responses (Pico → Host, 3 bytes each):
//   ACK:  [0xAA] [seq_lo] [seq_hi]
//   NACK: [0xBB] [reason] [0x00]
//     reason 0x01 = CRC error
//     reason 0x02 = buffer full (backpressure)
//     reason 0x03 = bad magic

#define MSEG_ACK         0xAA
#define MSEG_NACK        0xBB

// ─── Core0 → Core1 FIFO encoding ──────────────────────────────────────────────
// Normal command word : (CMD << 8) | node          — top 16 bits zero
// Debug step word      : (FIFO_STEP_DEBUG << 24) | (node << 16) | (count & 0xFFFF)
//   count is signed-magnitude: bit15 of the low word = direction (1 = negative)
#define FIFO_STEP_DEBUG  0xF0
#define STEP_DEBUG_SPS   1000   // fixed emit rate for debug stepping (steps/sec)
#define MSEG_NACK_CRC    0x01
#define MSEG_NACK_FULL   0x02
#define MSEG_NACK_MAGIC  0x03

// ─── Machine State ──────────────────────────────────────────────────────────
// Single authoritative state for the controller, owned across both cores.
//   IDLE/RUNNING transition freely (Core 1, driven by the segment queue).
//   ESTOP/ALARM are sticky — only cleared by setorigin / unalarm (Core 0).
//
// Transition map:
//   IDLE    → RUNNING   Core 1, queue non-empty
//   RUNNING → IDLE      Core 1, queue drained
//   any     → ESTOP     Core 0 `stop`, or MSEG_FLAG_ESTOP poison pill
//   ESTOP   → ALARM     Core 1, after flushing the queue (position now invalid)
//   ALARM   → IDLE      Core 0 `setorigin` (zeros position) or `unalarm`
enum MachineState : uint8_t {
    STATE_IDLE    = 0,
    STATE_RUNNING = 1,
    STATE_ESTOP   = 2,
    STATE_ALARM   = 3,
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────

extern MicroSegment masterBuf[MASTER_BUF_SIZE];
extern volatile uint16_t mBufHead;
extern volatile uint16_t mBufTail;

extern volatile uint8_t machineState;     // one of MachineState

// Machine position in steps (X,Y,Z,A), owned and accumulated by Core 1 per
// completed segment. The consumer (Core 1) is the single source of truth so it
// stays correct regardless of whether segments came from the host or, later,
// a local on-Pico planner. Invalid until a setorigin; invalidated by estop.
extern volatile int32_t machinePos[4];
extern volatile bool    positionValid;

// Job timing diagnostic (owned by Core 1, reset at each RUNNING transition).
// Gated behind DEBUG_TIMING (define it in platformio.ini build_flags to enable).
//   expected = sum of interval*maxSteps converted to us at F_CPU
//   measured = time spent inside segment emission, by the 1 MHz hardware timer
//   wall     = whole job (RUNNING -> IDLE), including buffer-empty gaps
// A ratio measured/expected != 1.0 means the cycle-counter wait loop runs at a
// different rate than F_CPU assumes; wall >> measured means streaming starvation.
#ifdef DEBUG_TIMING
extern volatile uint32_t jobExpectedUs;
extern volatile uint32_t jobMeasuredUs;
extern volatile uint32_t jobWallUs;
#endif

#endif // SHARED_H