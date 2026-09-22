// knife.cpp — oscillating drag-knife node type.
// Two independent outputs, both on the control plane (no stream bytes):
//   • the knife oscillator — a digital on/off enable, and
//   • the blower — a variable-speed fan on a PWM duty of 0..100 %.
//
// Everything a node needs off the wire — the 9-bit RX ISR, TX/DE handling, CRC,
// command framing, PING/GET_TYPE/ENABLE — comes from the type-agnostic node
// core; this file keeps only the knife-specific logic and wires it to the four
// core↔type hooks. The RX ISR is the shared rs485/isr_generic.cpp (this is a
// non-motion type); board pins come from the HAL binding
// (board/<mcu>/knife/knife.h via knife/knife.h).
#include <Arduino.h>
#include "board.h"
#include "knife/knife.h"
#include "common.h"
#include "node_hooks.h"

// Guard against an env that compiles this type dir with the wrong identity flag.
#ifdef NODE_TYPE
static_assert(NODE_TYPE == NODE_TYPE_KNIFE_OSC,
              "knife.cpp compiled with a non-knife -DNODE_TYPE");
#endif

// ─── Output helpers ─────────────────────────────────────────────────────────
static bool    oscOn      = false;
static uint8_t blowerDuty = 0;          // last commanded duty %, for node_status

static void oscSet(bool on) {
    oscOn = on;
    digitalWrite(HAL_KNIFE_OSC_PIN, on ? HIGH : LOW);
}

// Scale a 0..100 % duty to the 8-bit analogWrite range (0..255).
static void blowerSet(uint8_t dutyPct) {
    if (dutyPct > 100) dutyPct = 100;
    blowerDuty = dutyPct;
    analogWrite(HAL_KNIFE_BLOWER_PIN, (uint16_t)dutyPct * 255u / 100u);
}

// ─── Hooks: identity ────────────────────────────────────────────────────────
uint8_t node_type(void) { return NODE_TYPE_KNIFE_OSC; }

// ─── Hooks: setup ───────────────────────────────────────────────────────────
void node_setup(void) {
    pinMode(HAL_KNIFE_OSC_PIN, OUTPUT);
    pinMode(HAL_KNIFE_BLOWER_PIN, OUTPUT);
    oscSet(false);
    blowerSet(0);
}

// ─── Hooks: CMD_ENABLE / CMD_DISABLE effect ─────────────────────────────────
// ENABLE is a no-op arming step — outputs stay as commanded via CMD_KNIFE_*.
// DISABLE is a master safe-off: kill the oscillator and stop the blower, so a
// single generic command parks the node no matter what it was doing.
void node_set_enabled(bool on) {
    if (!on) {
        oscSet(false);
        blowerSet(0);
    }
}

// ─── Hooks: per-loop tick ───────────────────────────────────────────────────
// Both outputs are level-driven (digital pin / hardware PWM), so there is no
// state machine to advance between commands.
void node_loop(void) {}

// Type-specific status tail: [osc on][blower duty %].
uint8_t node_status(uint8_t* buf) {
    buf[0] = oscOn ? 1 : 0;
    buf[1] = blowerDuty;
    return 2;
}

// ─── Hooks: type-specific commands ──────────────────────────────────────────
// Reply convention (see dispatch.cpp): reply[] = [id][cmd][payloadLen][payload…];
// replyLen counts through the trailing CRC slot, which the core fills in.
bool node_handle_command(const uint8_t* pkt, uint8_t len,
                         uint8_t* reply, uint8_t* replyLen) {
    switch (pkt[1]) {
        case CMD_KNIFE_OSC: {
            // [dest][cmd][len=1][state][crc]
            if (len < 5) return false;
            oscSet(pkt[3] != 0);
            reply[0] = NODE_ID; reply[1] = CMD_KNIFE_OSC; reply[2] = 0;
            *replyLen = 4;
            return true;
        }
        case CMD_KNIFE_BLOWER: {
            // [dest][cmd][len=1][duty 0..100][crc]
            if (len < 5) return false;
            blowerSet(pkt[3]);
            reply[0] = NODE_ID; reply[1] = CMD_KNIFE_BLOWER; reply[2] = 0;
            *replyLen = 4;
            return true;
        }
        default:
            return false;
    }
}
