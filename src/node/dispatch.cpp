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

// A refusal. Same frame shape as any reply, but the opcode is CMD_NAK rather
// than the command's, so the payload has to carry what was refused.
//
// Exposed to node types (node_hooks.h: node_reply_nak) as well as used here for
// the generic unhandled-command case, so a type that has a specific reason for
// refusing (CMD_HOME_LEG's intent check, docs/homing.md §1.4) can report it instead
// of falling through to the generic NAK_UNSUPPORTED every plain `return false`
// produces below.
void node_reply_nak(uint8_t cmd, uint8_t reason, uint8_t* reply,
                    uint8_t* replyLen) {
    reply[0] = NODE_ID;
    reply[1] = CMD_NAK;
    reply[2] = 2;
    reply[3] = cmd;
    reply[4] = reason;
    *replyLen = 6;                       // header + 2 payload + CRC slot
}

// Generic node state the core tracks itself, so CMD_NODE_STATUS can report it
// uniformly across all types. Bit 0 = enabled (CMD_ENABLE/DISABLE); more generic
// flags can join here without touching any node type. Boots disabled.
// NODE_FLAG_* live in common.h (shared with the master). Boots 0: not energised,
// and no datum witness — a freshly booted node never claims continuity.
static uint8_t g_nodeFlags = 0;

// The single serializer for this node's whole state — see node_hooks.h. Callers:
// the generic CMD_NODE_STATUS below, and the stepper's CMD_ENGAGE / CMD_GET_POS.
// Let a type own one flag bit without owning the byte. g_nodeFlags is a
// read-modify-write shared with the generic ENABLE/DISABLE/DATUM handlers, so
// this must be called from loop context only — never from an ISR, or an
// interrupted RMW would drop a bit. The stepper drives NODE_FLAG_LIMIT from
// node_loop() for exactly this reason.
void node_set_flag(uint8_t bit, bool on) {
    if (on) g_nodeFlags |=  bit;
    else    g_nodeFlags &= ~bit;
}

uint8_t buildNodeStatus(uint8_t* buf) {
    buf[0] = node_type();
    buf[1] = g_nodeFlags;
    return 2 + node_status(&buf[2]);
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

        case CMD_ENABLE:
            node_set_enabled(true);
            // Sets ENABLED only. Deliberately does NOT set the datum witness:
            // re-energising does not restore knowledge of where the shaft is.
            g_nodeFlags |= NODE_FLAG_ENABLED;
            replyAck(CMD_ENABLE, reply, replyLen);
            return true;

        case CMD_DISABLE:
            node_set_enabled(false);
            // De-energised → back-drivable with no counter change → the datum is
            // gone. Clear both; only CMD_DATUM_SET can restore the witness.
            g_nodeFlags &= ~(NODE_FLAG_ENABLED | NODE_FLAG_DATUM);
            replyAck(CMD_DISABLE, reply, replyLen);
            return true;

        case CMD_DATUM_SET: {
            // The master is datuming this node right now. Arm the witness and
            // report the counter it refers to in the same transaction, so the
            // master's origin and the node's witness describe one instant.
            g_nodeFlags |= NODE_FLAG_DATUM;
            reply[0] = NODE_ID;
            reply[1] = CMD_DATUM_SET;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }

        case CMD_NODE_STATUS: {
            // Uniform status via the shared serializer. One command reports any
            // node's whole state; the same bytes back other commands' ACKs.
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
// Called by loop() once node-id + CRC have passed. `broadcast` is true when the
// frame was addressed to BUS_ADDR_BROADCAST rather than to this node's NODE_ID.
void dispatchCommand(const uint8_t* pkt, uint8_t len, bool broadcast) {
    // Deny-by-default: an unlisted command addressed to the wildcard is dropped
    // without acting. Checked before routing, so a command that is not cleared
    // for broadcast can never take effect via one.
    if (broadcast && !cmdAllowsBroadcast(pkt[1])) return;

    uint8_t reply[MAX_PACKET_LEN];
    uint8_t replyLen = routeCommand(pkt, len, reply);

    // Unhandled → NAK. It used to be dropped like a bad-CRC frame, which made a
    // node that refused indistinguishable from a node that is not there.
    //
    // routeCommand still returns 0 for unhandled, so debug_console.cpp keeps its
    // existing behaviour: the NAK is a property of the WIRE, where the ambiguity
    // lives, not of the dispatcher.
    //
    // CMD_NAK is excluded because it is reply-only. A node receiving one has been
    // sent something no master sends; NAKing it back would put a refusal on the
    // wire addressed at nobody listening.
    if (!replyLen && !broadcast && pkt[1] != CMD_NAK)
        node_reply_nak(pkt[1], NAK_UNSUPPORTED, reply, &replyLen);

    // Never answer a broadcast: every node would transmit at once, and the
    // collision would take out the confirm pass that follows it. That still
    // holds for a NAK — hence the !broadcast above as well as here.
    if (replyLen && !broadcast)
        sendCommandPacket(reply, replyLen);
}
