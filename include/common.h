#pragma once
#ifndef COMMON_H
#define COMMON_H

// The bus contract shared by the nodes and the controller. See
// docs/system/bus.md and docs/system/nodes.md.

// ─── Bus ───────────────────────────────────────────────────────────────────────
#define RS485_BAUD          921600
#define RESPONSE_TIMEOUT_MS    20

// Never answered. 0xFF, so a zeroed buffer cannot broadcast.
#define BUS_ADDR_BROADCAST 0xFF

// ─── Generic commands, handled by the node core ──────────────────────────────
#define CMD_PING     0x01
#define CMD_PONG     0x02
#define CMD_ENABLE   0x04   // effect delegated per type (motor energize / pump on …)
#define CMD_DISABLE  0x05
#define CMD_GET_TYPE 0x06   // reply payload: [NODE_TYPE_*]

// Reply-only: [ID][CMD_NAK][len=2][orig_cmd][NAK_*][crc]
#define CMD_NAK      0x07

// Reply payload: [framing errors][overflows][bad CRCs], u16 BE each, wrapping,
// never cleared.
#define CMD_BUS_STATS 0x08
#define CMD_BUS_STATS_REPLY_LEN 6

// Disable, then drop the slot and park the outputs; replies status. Also run
// by a node after BUS_SILENCE_MS without a byte (NODE_HAS_SILENCE_TIMEOUT).
#define CMD_MAKE_SAFE 0x09
#define BUS_SILENCE_MS   1000
#define BUS_KEEPALIVE_MS (BUS_SILENCE_MS / 3)

// ─── NAK reasons ─────────────────────────────────────────────────────────────
#define NAK_UNSUPPORTED 0x01  // this node does not implement that opcode
#define NAK_BAD_TOKEN   0x02  // session token mismatch; no node sends it
#define NAK_BAD_ARG     0x03  // opcode known, payload rejected
#define NAK_INTENT_MISMATCH 0x04  // CMD_HOME_LEG: intent disagrees with the pin;
                                  // CMD_PARK_LEG: on the switch
#define NAK_BUSY        0x05  // a leg is already running
#define NAK_NO_DATUM    0x06  // CMD_PARK_LEG: NODE_FLAG_DATUM is clear

// ─── Status flags ────────────────────────────────────────────────────────────
// The status payload is [node_type][flags][type's tail…] (docs/system/nodes.md).
#define NODE_FLAG_ENABLED 0x01  // energised
#define NODE_FLAG_DATUM   0x02  // counting without a break since CMD_DATUM_SET
#define CMD_DATUM_SET 0x23  // no payload; sets NODE_FLAG_DATUM, replies status payload
#define NODE_FLAG_LIMIT   0x04  // refusing stream steps: switch pressed or latched
#define NODE_FLAG_LEG     0x08  // a leg is running or its answer is not ready

// ─── Stepper commands ────────────────────────────────────────────────────────
#define CMD_GET_POS  0x03   // stepper: reply = status payload (above)
#define CMD_ENGAGE   0x20   // stepper: payload [slot] 0..3, 0xFF = disengage; ack = status payload
#define CMD_LASER    0x21   // stepper (-DNODE_HAS_LASER only): payload [state 0/1]; NAK elsewhere
#define CMD_NODE_STATUS 0x22 // any type; no payload; reply = status payload (above)

// ─── Legs (docs/system/nodes.md §8) ──────────────────────────────────────────
// CMD_HOME_LEG: one seek, retract or sweep.
//   payload (11 bytes, big-endian):
//     [0]    dir/intent     bit0 = wire dir bit: which way this leg goes
//                           bit1 = intent: 0 = seek expected, 1 = retract
//     [1..2] start_interval microseconds, pull-in rate
//     [3..4] floor_interval microseconds, cruise rate
//     [5..6] ramp_steps     steps from start to floor; 0 = no ramp
//     [7..10] max_steps     runaway budget
//   ack: the status payload. A Hall build ignores the intent bit.
#define CMD_HOME_LEG 0x24
#define CMD_HOME_LEG_PAYLOAD_LEN 11

// CMD_PARK_LEG: to a counter value, ramping up and down.
//   payload (10 bytes, big-endian):
//     [0..3] target         signed node counter to stop at
//     [4..5] start_interval microseconds, at both ends of the move
//     [6..7] floor_interval microseconds, cruise rate
//     [8..9] ramp_steps     steps from start to floor, and back; 0 = no ramp
//   ack: the status payload.
#define CMD_PARK_LEG 0x25
#define CMD_PARK_LEG_PAYLOAD_LEN 10

#define CMD_LEG_ABORT 0x26  // no payload; stops the pulser, stays energised

// How a Hall sweep ended. Only OK is a pass.
#define ROTARY_IDX_NONE       0  // no sweep has resolved since boot
#define ROTARY_IDX_OK         1  // index found, and proven periodic by the crossings
#define ROTARY_IDX_NOTFOUND   2  // budget ran out before enough crossings
#define ROTARY_IDX_DEGENERATE 3  // no usable dip in the window
#define ROTARY_IDX_OVERFLOW   4  // dip wider than the capture buffer
#define ROTARY_IDX_SLIP       5  // the laps disagree

// Which terminator a stepper has, in its status tail.
#define HOMING_KIND_NONE  0  // no switch, no index: this node NAKs CMD_HOME_LEG
#define HOMING_KIND_LIMIT 1  // limit switch on the terminator pin (linear)
#define HOMING_KIND_INDEX 2  // Hall index, analog dip (rotary)

// ─── Vacuum commands ─────────────────────────────────────────────────────────
#define CMD_SERVO_SET 0x10  // payload: [idx(0=all,1..N)][angle(0..180)]; ACK echoes cmd
#define SERVO_ON_ANGLE  180  // the controller's "on"
#define CMD_SSR_SET   0x11  // payload: [state(0=off, 1=on w/ soft-start)]; ACK echoes cmd
#define CMD_SWITCH_GET 0x14  // no payload; reply payload [level]: 0 closed, 1 open

// ─── Knife commands ──────────────────────────────────────────────────────────
#define CMD_KNIFE_OSC    0x12  // payload: [state(0=off, 1=on)]; ACK echoes cmd
#define CMD_KNIFE_BLOWER 0x13  // payload: [duty(0..100 %)]; ACK echoes cmd

// The only commands either side broadcasts. Each must be idempotent and useful
// with no answer.
static inline bool cmdAllowsBroadcast(uint8_t cmd) {
    return cmd == CMD_DISABLE || cmd == CMD_ENABLE;
}

// ─── Node types (CMD_GET_TYPE) ───────────────────────────────────────────────
// Mirrored on the host (web/src/machine/schema.ts).
#define NODE_TYPE_STEPPER  0x01
#define NODE_TYPE_VACUUM   0x02
#define NODE_TYPE_KNIFE_OSC 0x03  // oscillating (drag/tangential) knife controller

// CRC-8, reflected, poly 0x8C, init 0x00, no final xor. Table in flash on AVR.
#ifdef __AVR__
#include <avr/pgmspace.h>
static const uint8_t crc8_table[256] PROGMEM = {
#else
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
static const uint8_t crc8_table[256] = {
#endif
    0x00, 0x5E, 0xBC, 0xE2, 0x61, 0x3F, 0xDD, 0x83, 0xC2, 0x9C, 0x7E, 0x20, 0xA3, 0xFD, 0x1F, 0x41,
    0x9D, 0xC3, 0x21, 0x7F, 0xFC, 0xA2, 0x40, 0x1E, 0x5F, 0x01, 0xE3, 0xBD, 0x3E, 0x60, 0x82, 0xDC,
    0x23, 0x7D, 0x9F, 0xC1, 0x42, 0x1C, 0xFE, 0xA0, 0xE1, 0xBF, 0x5D, 0x03, 0x80, 0xDE, 0x3C, 0x62,
    0xBE, 0xE0, 0x02, 0x5C, 0xDF, 0x81, 0x63, 0x3D, 0x7C, 0x22, 0xC0, 0x9E, 0x1D, 0x43, 0xA1, 0xFF,
    0x46, 0x18, 0xFA, 0xA4, 0x27, 0x79, 0x9B, 0xC5, 0x84, 0xDA, 0x38, 0x66, 0xE5, 0xBB, 0x59, 0x07,
    0xDB, 0x85, 0x67, 0x39, 0xBA, 0xE4, 0x06, 0x58, 0x19, 0x47, 0xA5, 0xFB, 0x78, 0x26, 0xC4, 0x9A,
    0x65, 0x3B, 0xD9, 0x87, 0x04, 0x5A, 0xB8, 0xE6, 0xA7, 0xF9, 0x1B, 0x45, 0xC6, 0x98, 0x7A, 0x24,
    0xF8, 0xA6, 0x44, 0x1A, 0x99, 0xC7, 0x25, 0x7B, 0x3A, 0x64, 0x86, 0xD8, 0x5B, 0x05, 0xE7, 0xB9,
    0x8C, 0xD2, 0x30, 0x6E, 0xED, 0xB3, 0x51, 0x0F, 0x4E, 0x10, 0xF2, 0xAC, 0x2F, 0x71, 0x93, 0xCD,
    0x11, 0x4F, 0xAD, 0xF3, 0x70, 0x2E, 0xCC, 0x92, 0xD3, 0x8D, 0x6F, 0x31, 0xB2, 0xEC, 0x0E, 0x50,
    0xAF, 0xF1, 0x13, 0x4D, 0xCE, 0x90, 0x72, 0x2C, 0x6D, 0x33, 0xD1, 0x8F, 0x0C, 0x52, 0xB0, 0xEE,
    0x32, 0x6C, 0x8E, 0xD0, 0x53, 0x0D, 0xEF, 0xB1, 0xF0, 0xAE, 0x4C, 0x12, 0x91, 0xCF, 0x2D, 0x73,
    0xCA, 0x94, 0x76, 0x28, 0xAB, 0xF5, 0x17, 0x49, 0x08, 0x56, 0xB4, 0xEA, 0x69, 0x37, 0xD5, 0x8B,
    0x57, 0x09, 0xEB, 0xB5, 0x36, 0x68, 0x8A, 0xD4, 0x95, 0xCB, 0x29, 0x77, 0xF4, 0xAA, 0x48, 0x16,
    0xE9, 0xB7, 0x55, 0x0B, 0x88, 0xD6, 0x34, 0x6A, 0x2B, 0x75, 0x97, 0xC9, 0x4A, 0x14, 0xF6, 0xA8,
    0x74, 0x2A, 0xC8, 0x96, 0x15, 0x4B, 0xA9, 0xF7, 0xB6, 0xE8, 0x0A, 0x54, 0xD7, 0x89, 0x6B, 0x35,
};

static inline uint8_t crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    while (len--)
        crc = pgm_read_byte(&crc8_table[crc ^ *data++]);
    return crc;
}

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), bitwise.
static inline uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (uint8_t k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

#endif
