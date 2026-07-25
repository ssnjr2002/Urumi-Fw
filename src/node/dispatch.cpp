// dispatch.cpp — type-agnostic command routing.
// Core handles the generic command table; anything it doesn't recognise falls
// through to the node type's node_handle_command().
#include <Arduino.h>
#include "common.h"
#include "protocol.h"
#include "node_hooks.h"
#include "rs485/rs485.h"

// Reply convention: reply[] holds [id][cmd][payloadLen][payload…]; replyLen
// counts through the trailing CRC slot, which sendCommandPacket fills in.
static void replyAck(uint8_t cmd, uint8_t* reply, uint8_t* replyLen) {
    reply[0] = NODE_ID;
    reply[1] = cmd;
    reply[2] = 0;
    *replyLen = 4;
}

// Generic node state the core tracks itself, so CMD_NODE_STATUS can report it
// uniformly across all types. Bit 0 = enabled (CMD_ENABLE/DISABLE); more generic
// flags can join here without touching any node type. Boots disabled.
#define NODE_FLAG_ENABLED 0x01
static uint8_t g_nodeFlags = 0;

// Returns true iff this was a generic command (reply staged in `reply`).
static bool handleGenericCommand(const uint8_t* pkt, uint8_t* reply,
                                 uint8_t* replyLen) {
    switch (pkt[1]) {                    // pkt[1] = cmdId
        case CMD_PING:
            reply[0] = NODE_ID; reply[1] = CMD_PONG; reply[2] = 0;
            *replyLen = 4;
            return true;

        case CMD_GET_TYPE:
            reply[0] = NODE_ID; reply[1] = CMD_GET_TYPE; reply[2] = 1;
            reply[3] = node_type();
            *replyLen = 5;
            return true;

        case CMD_ENABLE:
            node_set_enabled(true);
            g_nodeFlags |= NODE_FLAG_ENABLED;
            replyAck(CMD_ENABLE, reply, replyLen);
            return true;

        case CMD_DISABLE:
            node_set_enabled(false);
            g_nodeFlags &= ~NODE_FLAG_ENABLED;
            replyAck(CMD_DISABLE, reply, replyLen);
            return true;

        case CMD_NODE_STATUS: {
            // Uniform status: generic head [node_type][flags] + a type-specific
            // tail from node_status(). One command reports any node's whole state.
            reply[0] = NODE_ID;
            reply[1] = CMD_NODE_STATUS;
            reply[3] = node_type();
            reply[4] = g_nodeFlags;
            uint8_t tail = node_status(&reply[5]);
            reply[2] = 2 + tail;                 // payload length
            *replyLen = 3 + (2 + tail) + 1;      // header + payload + CRC slot
            return true;
        }

        default:
            return false;                // not generic — let the node type try
    }
}

// Route a validated command to its handler, staging the reply into `reply`.
// Returns replyLen (0 = command unhandled / no reply). Shared by the RS485 path
// (dispatchCommand) and the optional USART0 debug console (debug_console.cpp) so
// both drive the exact same handlers — the console can never diverge from wire
// behaviour. `reply` must be at least MAX_PACKET_LEN.
uint8_t routeCommand(const uint8_t* pkt, uint8_t len, uint8_t* reply) {
    uint8_t replyLen = 0;
    bool handled = handleGenericCommand(pkt, reply, &replyLen);
    if (!handled)
        handled = node_handle_command(pkt, len, reply, &replyLen);
    return handled ? replyLen : 0;
}
//  1x 2y 3a 4z
// Called by loop() once node-id + CRC have passed.
void dispatchCommand(const uint8_t* pkt, uint8_t len) {
    uint8_t reply[MAX_PACKET_LEN];
    uint8_t replyLen = routeCommand(pkt, len, reply);

    if (replyLen)
        sendCommandPacket(reply, replyLen);
    // Unknown command → silently dropped (same as a bad-CRC packet).
}
