// Raspberry Pi Pico 2 — RS485 master matching ATTINY_Custom_Slave protocol
//
// Custom RS485 protocol — same framing style as tomrodinger/servomotor
//
// Frame structure (request):
//   [SIZE] [ADDR] [CMD] [PAYLOAD...] [CRC16 LE 2 bytes]
//   SIZE = ((remaining_bytes_after_size) << 1) | 1
//   LSB of SIZE is always 1 — used for self-synchronizing frame detection
//
// Frame structure (response):
//   [SIZE] [STATUS] [DATA...] [CRC16 LE 2 bytes]
//   STATUS: 0x00=ok/no data, 0x01=ok/data follows, 0xFF=error
//
// Broadcast address 0xFF: all slaves execute, none respond.
//
// Commands (match slave exactly):
//   0x01 CMD_PING    → reply: node_id (1 byte)
//   0x02 CMD_QUEUE   → payload: dir(1)+steps(2 LE)+speed(2 LE)
//                    → reply: buf_free (1 byte);
//   0x03 CMD_GO      → broadcast: start all armed motors simultaneously
//   0x04 CMD_STOP    → stop immediately, clear buffer; reply: STATUS_OK if not broadcast
//   0x05 CMD_STATUS  → reply: running(1)+buf_used(1)+buf_free(1)+steps_remaining(2)
//   0x06 CMD_ENABLE  → payload: enable(1); reply: STATUS_OK if not broadcast
//
// Wiring (SP3485EN):
//   GP4 TX → DI,  GP5 RX ← RO,  GP6 → DE+/RE
//
// CoreXY node map:
//   Node 1 = Motor A,  Node 2 = Motor B,  Node 3 = Z (pen)
//   Motor A steps = dx + dy
//   Motor B steps = dx - dy
//
// USB commands:
//   ping <addr>
//   enable <addr|all> <0|1>
//   stop
//   <addr> f/b <steps> [sps]            — single-axis jog
//   xy <dx> <dy> [sps]                   — CoreXY in steps (signed)
//   rect <W_mm> <H_mm> [speed_mm_s]     — draw rectangle
//   circle <R_mm> [speed_mm_s] [segs]   — draw circle

#include <Arduino.h>
#include "shared.h"

// ─── Cross-Core Global Variables (Memory Allocation) ──────────────────────────

// The Ring Buffer
Segment masterBuf[MASTER_BUF_SIZE];
volatile uint8_t mBufHead = 0; 
volatile uint8_t mBufTail = 0;

// Emergency Stop Flag
volatile bool emergencyStop = false;

// UI Command Flags
volatile uint8_t reqPingAddr = 0;
volatile uint8_t reqEnableAddr = 0;
volatile int8_t  reqEnableVal = -1;  
volatile int8_t  pingResult = -1;    

// Note: 
// setup() and loop() are defined in core0.cpp
// setup1() and loop1() are defined in core1.cpp
//
// The Earles F. Philhower RP2040/RP2350 core automatically links them and 
// launches Core 0 and Core 1 independently. No further code is needed here!