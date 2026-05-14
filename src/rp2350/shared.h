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

#define SLAVE_BUF_SIZE    8
#define SLAVE_BUF_TARGET  6
#define MASTER_BUF_SIZE   128
#define MASTER_BUF_LOW_WATERMARK 96

// ─── Shared Memory Structures ─────────────────────────────────────────────────
// struct Segment {
//     int16_t  steps[NUM_MOTORS];
//     uint16_t sps[NUM_MOTORS];
//     bool     cw[NUM_MOTORS];
// };

#define MAX_MOTORS 4 // Maximum motors that can move in a single synchronized segment

struct Segment {
    uint8_t  numMotors;           // How many motors are in this specific move
    uint8_t  addr[MAX_MOTORS];    // RS485 addresses for this move
    uint16_t steps[MAX_MOTORS];
    uint16_t sps[MAX_MOTORS];
    bool     cw[MAX_MOTORS];
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────
// These tell Core 0 and Core 1 that these variables exist, allowing them to 
// share the exact same memory addresses safely.

extern Segment masterBuf[MASTER_BUF_SIZE];
extern volatile uint8_t mBufHead; 
extern volatile uint8_t mBufTail;

extern volatile bool emergencyStop;
extern volatile bool alarmTriggered;

// Shared variables for simple UI commands (Core 0 requests, Core 1 executes)
extern volatile uint8_t reqPingAddr;
extern volatile uint8_t reqEnableAddr;
extern volatile int8_t  reqEnableVal; // -1 = no request, 0 = off, 1 = on
extern volatile int8_t  pingResult;   // -1 = pending/none, 0 = timeout, 1 = OK
extern volatile uint8_t failedNode;

#endif // SHARED_H