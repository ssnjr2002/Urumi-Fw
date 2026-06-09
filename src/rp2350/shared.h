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

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────

extern MicroSegment masterBuf[MASTER_BUF_SIZE];
extern volatile uint16_t mBufHead;
extern volatile uint16_t mBufTail;

extern volatile bool emergencyStop;
extern volatile bool alarmTriggered;

#endif // SHARED_H