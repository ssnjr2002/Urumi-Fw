// rpc_server.cpp — Core 1's side of channel 1. See ipc/core1_rpc.h.
//
// This is core1's control_plane: it was a 236-line switch inside processBus with
// the same disease, every arm hand-packing its own frame and hand-packing its
// own reply into FIFO words. The framing moved to bus/packet.h and the reply
// moved to RpcReply, so what is left here is the part that is actually specific
// to each command: how many argument bytes it carries, and whether its ack is a
// bare ok or a status payload.
#include <Arduino.h>
#include "../ipc/core1_rpc.h"
#include "../ipc/shared_state.h"
#include "bus/packet.h"
#include "emit/emit.h"
#include "common.h"

// Longest frame we build here: [id][cmd][len][payload…][crc].
#define PKT_MAX (3 + RPC_ARG_MAX + 1)

// Reply staging. Every path fills one of these and posts it exactly once, which
// is the property the old code could not state: there, eleven separate sites
// each pushed their own words, and a missed push wedged Core 0 in
// pop_blocking() forever.
static void replyWith(const RpcRequest* req, RpcResult result,
                      const uint8_t* payload, uint8_t len,
                      uint8_t nakReason = 0) {
    RpcReply rep = {};
    rep.id     = req->id;
    rep.cmd    = req->cmd;                 // the REQUEST's opcode, even for a NAK:
                                           // rpcCall asserts the echo, and the
                                           // refusal travels in `result` instead.
    rep.node   = req->node;
    rep.result = result;
    rep.nakReason = nakReason;
    if (payload && len) {
        if (len > RPC_PAYLOAD_MAX) len = RPC_PAYLOAD_MAX;
        memcpy(rep.payload, payload, len);
        rep.len = len;
    }
    rpcServerReply(&rep);
}

// Quiesce the wire before a command frame: let the TX drain, drop anything
// stale in RX, then send a NOP stream byte so slave parsers start from a known
// state. Every command path did this identically.
static void busQuiesce(void) {
    while (!rs485.txEmpty());
    rs485.flushRX();
    rs485.writeStream(0);
}

// Which commands answer with a status payload ([type][flags][tail…]) rather than
// a bare ack. The node has one serializer (buildNodeStatus), so this is a
// property of the command, not a per-command reply shape.
static bool answersWithStatus(uint8_t cmd) {
    return cmd == CMD_NODE_STATUS || cmd == CMD_DATUM_SET ||
           cmd == CMD_ENGAGE      || cmd == CMD_HOME_LEG;
}

// Expand a request's argument bytes into the node's on-wire payload.
//
// Only CMD_SERVO_SET differs from a straight copy: the host talks on/off, but
// the node wants a raw angle, so `on` expands to SERVO_ON_ANGLE here. That
// translation stayed on this side because it is a property of the node's wire
// format, not of the operator's vocabulary.
static uint8_t buildPayload(const RpcRequest* req, uint8_t* out) {
    if (req->cmd == CMD_SERVO_SET) {
        out[0] = (req->args[0] >> 4) & 0x0F;                       // channel idx
        out[1] = (req->args[0] & 0x01) ? SERVO_ON_ANGLE : 0;       // angle
        return 2;
    }
    // CMD_PING, CMD_NODE_STATUS, CMD_DATUM_SET and CMD_SWITCH_GET are queries and
    // carry nothing; the rest carry their argument bytes verbatim.
    if (req->cmd == CMD_PING || req->cmd == CMD_NODE_STATUS ||
        req->cmd == CMD_DATUM_SET || req->cmd == CMD_SWITCH_GET) {
        return 0;
    }
    uint8_t n = req->argLen;
    if (n > RPC_ARG_MAX) n = RPC_ARG_MAX;
    memcpy(out, req->args, n);
    return n;
}

// Fold an energisation fact out of a CONFIRMED reply into nodeEnabled.
//
// Core 1 is the sole writer of that mask (ipc/shared_state.h) and this is the
// only place a bus transaction produces one, so every path that could learn it
// -- enable, disable, axes_enable's per-node relay, bind-time ENGAGE, a nodestat
// poll, a homing leg -- funnels through here. Core 0 used to do this at four
// separate call sites, each keying on the slot and each ASSUMING success:
// cmdEnable discarded the RpcResult entirely and set the bit even on a timeout.
//
// Two evidence kinds, because the node answers CMD_ENABLE/CMD_DISABLE with a
// bare ack (node/dispatch.cpp) and everything else with a status payload:
//   ack     -> the command we sent IS the fact, since the node acked doing it
//   status  -> read NODE_FLAG_ENABLED, the node's own live self-report
//
// ONLY CONFIRMED REPLIES WRITE. A timeout or a NAK leaves the mask alone rather
// than guessing in either direction -- see busDisableAll() in bus/packet.cpp for
// what that costs and why it is still the honest choice.
//
// Decoded rather than peeking buf[1]: core1_rpc.h keeps the NS_* offsets inside
// that module, and nodeStatusDecode is a pure function either core may call.
static void noteEnabled(const RpcRequest* req, const uint8_t* buf, uint8_t rxLen) {
    if (req->node > BUS_ADDR_MAX) return;
    const uint16_t bit = 1u << req->node;

    if (req->cmd == CMD_ENABLE)  { nodeEnabled |=  bit; return; }
    if (req->cmd == CMD_DISABLE) { nodeEnabled &= ~bit; return; }

    if (!answersWithStatus(req->cmd)) return;
    NodeStatus ns;
    if (!nodeStatusDecode(buf, rxLen, &ns)) return;
    if (ns.flags & NODE_FLAG_ENABLED) nodeEnabled |=  bit;
    else                              nodeEnabled &= ~bit;
}

// One node transaction: build, send, wait, reply.
static void serveNodeCmd(const RpcRequest* req) {
    // Broadcast: one frame to every node, answered by none. RPC_OK here means
    // "the frame went out", NOT "the nodes acted" — nothing on this path can
    // know the latter, and rpcNodeCmd's contract says so.
    //
    // TODO(verify): follow with a per-node CMD_NODE_STATUS poll and check
    // NODE_FLAG_ENABLED to turn this into a real result. What to do about a node
    // that answers with the wrong state — retry, fault mask, alarm — is still
    // undecided, so today the broadcast is fire-and-forget and the estop path
    // keeps its serial CMD_DISABLE sweep as the actual guarantee.
    if (req->node == BUS_ADDR_BROADCAST) {
        busQuiesce();
        const bool sent = sendBroadcast(req->cmd);
        // Asymmetric on purpose, and in the direction that is safe to be wrong
        // in -- the doctrine cmdBusEnable used to carry on Core 0, now applied
        // where the frame actually goes out. OFF clears the whole mask: if a
        // node missed the frame we under-claim, and the operator is told less is
        // armed than is. ON touches NOTHING: motion gates on these bits, so
        // believing a node armed when it never heard us is the direction that
        // moves a machine that is not ready. `axes_enable on` relays per node
        // and gets an ack for each; that is what actually arms the map.
        if (sent && req->cmd == CMD_DISABLE) nodeEnabled = 0;
        replyWith(req, sent ? RPC_OK : RPC_BAD_REPLY, nullptr, 0);
        return;
    }

    uint8_t pkt[PKT_MAX];
    uint8_t payload[RPC_ARG_MAX];
    const uint8_t plen = buildPayload(req, payload);

    pkt[0] = req->node;
    pkt[1] = req->cmd;
    pkt[2] = plen;
    memcpy(&pkt[3], payload, plen);

    busQuiesce();
    sendPacket(pkt, 3 + plen + 1);

    // CMD_PING is the one command whose answer is a DIFFERENT opcode.
    const uint8_t expect = (req->cmd == CMD_PING) ? CMD_PONG : req->cmd;

    uint8_t buf[RPC_PAYLOAD_MAX];
    uint8_t gotCmd = 0;
    const uint8_t rxLen = receivePacket(req->node, expect, buf,
                                        RESPONSE_TIMEOUT_MS, &gotCmd);

    if (rxLen == 0xFF) { replyWith(req, RPC_TIMEOUT, nullptr, 0); return; }

    // The node refused. Payload is [orig_cmd][reason]; a NAK too short to carry
    // one is still a refusal, just an unattributed one, so report it as a NAK
    // with no reason rather than downgrading it to BAD_REPLY — the fact worth
    // having here is "it is there and it said no".
    if (gotCmd == CMD_NAK) {
        replyWith(req, RPC_NAK, nullptr, 0, rxLen >= 2 ? buf[1] : 0);
        return;
    }

    // A status-bearing command that came back too short to be a status is a node
    // talking a protocol we do not know — reportable now that RPC_BAD_REPLY
    // exists, where the old one-bit result had to call it a timeout.
    if (answersWithStatus(req->cmd) && rxLen < 2) {
        replyWith(req, RPC_BAD_REPLY, nullptr, 0);
        return;
    }
    noteEnabled(req, buf, rxLen);      // confirmed — fold in what it told us
    replyWith(req, RPC_OK, buf, rxLen);
}

bool rpcServerPoll(void) {
    RpcRequest req;
    if (!rpcServerTake(&req)) return false;

    switch (req.op) {
        case RPC_OP_STEP_DEBUG: {
            // Fire-and-forget: no reply, and Core 0 is not waiting on one.
            const uint8_t  slot = req.args[0];
            const uint16_t sps  = (uint16_t)((req.args[1] << 8) | req.args[2]);
            const int32_t  steps = (int32_t)(((uint32_t)req.args[3] << 24) |
                                             ((uint32_t)req.args[4] << 16) |
                                             ((uint32_t)req.args[5] <<  8) |
                                              (uint32_t)req.args[6]);
            emitDebugSteps(slot, sps, steps);
            return true;
        }
        case RPC_OP_NODE:
        default:
            serveNodeCmd(&req);
            return true;
    }
}
