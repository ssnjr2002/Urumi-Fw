// dispatch.cpp — type-agnostic command routing.
// Core handles the generic command table; anything it doesn't recognise falls
// through to the node type's node_handle_command().
#include <Arduino.h>
#include <util/atomic.h>
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

// A refusal. The opcode is CMD_NAK rather than the command's, so the payload
// carries what was refused and why. Also used by the types (node_hooks.h).
void node_reply_nak(uint8_t cmd, uint8_t reason, uint8_t* reply,
                    uint8_t* replyLen) {
    reply[0] = NODE_ID;
    reply[1] = CMD_NAK;
    reply[2] = 2;
    reply[3] = cmd;
    reply[4] = reason;
    *replyLen = 6;                       // header + 2 payload + CRC slot
}

// NODE_FLAG_* (common.h). Boots 0: not energised, and no datum.
static uint8_t g_nodeFlags = 0;

// Loop context only (node_hooks.h).
void node_set_flag(uint8_t bit, bool on) {
    if (on) g_nodeFlags |=  bit;
    else    g_nodeFlags &= ~bit;
}

bool node_flag(uint8_t bit) { return (g_nodeFlags & bit) != 0; }

uint8_t buildNodeStatus(uint8_t* buf) {
    buf[0] = node_type();
    buf[1] = g_nodeFlags;
    return 2 + node_status(&buf[2]);
}

void node_make_safe(void) {
    node_set_enabled(false);
    g_nodeFlags &= ~(NODE_FLAG_ENABLED | NODE_FLAG_DATUM);
    node_release();
}

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

        case CMD_BUS_STATS: {
            uint16_t ferr, ovf;
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                ferr = busFerrCount;
                ovf  = busOvfCount;
            }
            const uint16_t v[3] = { ferr, ovf, busCrcCount };
            reply[0] = NODE_ID; reply[1] = CMD_BUS_STATS;
            reply[2] = CMD_BUS_STATS_REPLY_LEN;
            for (uint8_t i = 0; i < 3; i++) {
                reply[3 + 2 * i] = (uint8_t)(v[i] >> 8);
                reply[4 + 2 * i] = (uint8_t)v[i];
            }
            *replyLen = 3 + CMD_BUS_STATS_REPLY_LEN + 1;
            return true;
        }

        case CMD_ENABLE:
            node_set_enabled(true);
            // Not DATUM: energising does not say where the shaft is.
            g_nodeFlags |= NODE_FLAG_ENABLED;
            replyAck(CMD_ENABLE, reply, replyLen);
            return true;

        case CMD_DISABLE:
            node_set_enabled(false);
            // De-energised, the shaft can be turned by hand: the datum is gone.
            g_nodeFlags &= ~(NODE_FLAG_ENABLED | NODE_FLAG_DATUM);
            replyAck(CMD_DISABLE, reply, replyLen);
            return true;

        case CMD_MAKE_SAFE: {
            node_make_safe();
            reply[0] = NODE_ID;
            reply[1] = CMD_MAKE_SAFE;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }

        case CMD_DATUM_SET: {
            // The reply carries the counter the datum refers to.
            g_nodeFlags |= NODE_FLAG_DATUM;
            reply[0] = NODE_ID;
            reply[1] = CMD_DATUM_SET;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }

        case CMD_NODE_STATUS: {
            reply[0] = NODE_ID;
            reply[1] = CMD_NODE_STATUS;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;                        // payload length
            *replyLen = 3 + n + 1;               // header + payload + CRC slot
            return true;
        }

        default:
            return false;                // not generic — let the node type try
    }
}

// Route a checked command to its handler, staging the reply into `reply` (at
// least MAX_PACKET_LEN). Returns replyLen, 0 if unhandled. Shared by the bus
// (dispatchCommand) and the debug console, so both run the same handlers.
uint8_t routeCommand(const uint8_t* pkt, uint8_t len, uint8_t* reply) {
    uint8_t replyLen = 0;
    bool handled = handleGenericCommand(pkt, reply, &replyLen);
    if (!handled)
        handled = node_handle_command(pkt, len, reply, &replyLen);
    return handled ? replyLen : 0;
}
// Called by loop() once the id and CRC have passed.
void dispatchCommand(const uint8_t* pkt, uint8_t len, bool broadcast) {
    // A broadcast runs only if the command is on the allowlist.
    if (broadcast && !cmdAllowsBroadcast(pkt[1])) return;

    uint8_t reply[MAX_PACKET_LEN];
    uint8_t replyLen = routeCommand(pkt, len, reply);

    // Unhandled → NAK, so a node that refuses can be told from one that is not
    // there. Not for CMD_NAK, which is reply-only.
    if (!replyLen && !broadcast && pkt[1] != CMD_NAK)
        node_reply_nak(pkt[1], NAK_UNSUPPORTED, reply, &replyLen);

    // Never answer a broadcast: every node would transmit at once.
    if (replyLen && !broadcast)
        sendCommandPacket(reply, replyLen);
}
