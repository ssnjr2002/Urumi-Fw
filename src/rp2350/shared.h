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
    uint8_t  nodeId[MAX_MOTORS];  // Node IDs for this move (1 to 4)
    uint32_t steps[MAX_MOTORS];
    uint32_t sps[MAX_MOTORS];
    bool     cw[MAX_MOTORS];
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────

extern Segment masterBuf[MASTER_BUF_SIZE];
extern volatile uint8_t mBufHead; 
extern volatile uint8_t mBufTail;

extern volatile bool emergencyStop;
extern volatile bool alarmTriggered;

// Ping command interaction between core0 and core1
enum PingStatus {
    PING_IDLE = 0,
    PING_PENDING,
    PING_OK,
    PING_TIMEOUT
};
extern volatile uint8_t pendingPingNode;
extern volatile PingStatus pingStatus;

#endif // SHARED_H