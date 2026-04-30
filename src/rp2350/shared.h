#pragma once
#ifndef SHARED_H
#define SHARED_H

#include <Arduino.h>
#include <stdint.h>

// ─── Pins ──────────────────────────────────────────────────────────────────────
#define RS485_TX_PIN  4
#define RS485_RX_PIN  5
#define RS485_EN_PIN  6

// ─── Bus ───────────────────────────────────────────────────────────────────────
#define RS485_BAUD          230400
#define RESPONSE_TIMEOUT_MS    20

// ─── Protocol ──────────────────────────────────────────────────────────────────
#define BROADCAST    0xFF
#define RESP_CHAR    0xFD
#define STATUS_OK    0x00
#define STATUS_DATA  0x01
#define STATUS_ERR   0xFF

#define CMD_PING     0x01
#define CMD_QUEUE    0x02
#define CMD_GO       0x03
#define CMD_STOP     0x04
#define CMD_STATUS   0x05
#define CMD_ENABLE   0x06

// ─── Machine config ────────────────────────────────────────────────────────────
#define NODE_A  1
#define NODE_B  2
#define NODE_Z  3

// GT2 belt + 20-tooth pulley
// Set MICROSTEP to match your DRV8825 jumpers (1/2/4/8/16/32)
#define BELT_PITCH_MM   2.0f
#define PULLEY_TEETH    20
#define MOTOR_FULL_SPS  200
#define MICROSTEP       32
#define MM_PER_REV      (BELT_PITCH_MM * PULLEY_TEETH)              // 40 mm
#define STEPS_PER_MM    (MOTOR_FULL_SPS * MICROSTEP / MM_PER_REV)   // 80 steps/mm
#define DEFAULT_SPD_MM_S  35.0f   // used when speed arg omitted

// Pen Z axis
#define PEN_DOWN_MM    2.0f
#define PEN_UP_MM      2.0f
#define PEN_SPEED_SPS  800

#define SLAVE_BUF_SIZE    8
#define SLAVE_BUF_TARGET  6
#define MASTER_BUF_SIZE   128

// ─── Shared Memory Structures ─────────────────────────────────────────────────
struct Segment {
    int16_t  aSteps, bSteps;
    uint16_t aSps, bSps;
    bool     aCw, bCw;
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────
// These tell Core 0 and Core 1 that these variables exist, allowing them to 
// share the exact same memory addresses safely.

extern Segment masterBuf[MASTER_BUF_SIZE];
extern volatile uint8_t mBufHead; 
extern volatile uint8_t mBufTail;

extern volatile bool emergencyStop;

// Shared variables for simple UI commands (Core 0 requests, Core 1 executes)
extern volatile uint8_t reqPingAddr;
extern volatile uint8_t reqEnableAddr;
extern volatile int8_t  reqEnableVal; // -1 = no request, 0 = off, 1 = on
extern volatile int8_t  pingResult;   // -1 = pending/none, 0 = timeout, 1 = OK

#endif // SHARED_H