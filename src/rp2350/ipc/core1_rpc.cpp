// core1_rpc.cpp — Core 0's side of channel 1. See core1_rpc.h for the contract.
#include <Arduino.h>
#include "pico/util/queue.h"
#include "core1_rpc.h"

// Depth 4, though rpcPost admits one reply-bearing transaction at a time: the
// spare entries are for fire-and-forget requests (debug step), which do not
// occupy the in-flight slot and can therefore stack up behind a live one.
#define RPC_QUEUE_DEPTH 4

static queue_t  s_reqQ;
static queue_t  s_repQ;
static uint16_t s_nextId  = 1;      // 0 is reserved for fire-and-forget
static bool     s_inFlight = false;

void rpcInit(void) {
    queue_init(&s_reqQ, sizeof(RpcRequest), RPC_QUEUE_DEPTH);
    queue_init(&s_repQ, sizeof(RpcReply),   RPC_QUEUE_DEPTH);
}

bool rpcBusy(void) { return s_inFlight; }

bool rpcPost(const RpcRequest* req, uint16_t* idOut) {
    const bool wantsReply = (req->id != 0);
    if (wantsReply && s_inFlight) return false;

    RpcRequest r = *req;
    if (!queue_try_add(&s_reqQ, &r)) return false;

    if (wantsReply) s_inFlight = true;
    if (idOut) *idOut = r.id;
    return true;
}

bool rpcPoll(RpcReply* out) {
    if (!queue_try_remove(&s_repQ, out)) return false;
    s_inFlight = false;
    return true;
}

// Allocate an id and stamp the request. Fire-and-forget callers pass id 0 and
// never reach here.
static uint16_t rpcNextId(void) {
    uint16_t id = s_nextId++;
    if (s_nextId == 0) s_nextId = 1;      // wrap past the reserved value
    return id;
}

RpcResult rpcCall(const RpcRequest* req, RpcReply* out, uint32_t timeoutMs) {
    RpcRequest r = *req;
    r.id = rpcNextId();

    if (!rpcPost(&r, nullptr)) {
        out->result = RPC_TIMEOUT;        // queue full or one already in flight
        out->len = 0;
        return RPC_TIMEOUT;
    }

    const absolute_time_t deadline = make_timeout_time_ms(timeoutMs);
    for (;;) {
        if (rpcPoll(out)) {
            // The old encoding threw the echo away (`resp & 0xFFFF`). Keep it and
            // check it: a mismatch means the two sides disagree about what is on
            // the wire, which is a bug, not a bus condition.
            if (out->id != r.id || out->cmd != r.cmd || out->node != r.node) {
                out->result = RPC_TIMEOUT;
                out->len = 0;
                return RPC_TIMEOUT;
            }
            return out->result;
        }
        if (time_reached(deadline)) {
            // Core 1 never answered. Drop the in-flight claim, or one wedge would
            // lock out every later command.
            s_inFlight  = false;
            out->result = RPC_TIMEOUT;
            out->len    = 0;
            return RPC_TIMEOUT;
        }
        tight_loop_contents();
    }
}

// ─── Node status decoding ─────────────────────────────────────────────────────
// Field offsets. These do not leave this file — that is the whole point of
// decoding into a struct. Consumers read st.pos, never buf[NS_STEP_POS].
#define NS_TYPE       0
#define NS_FLAGS      1
#define NS_HEAD_LEN   2    // [type][flags] — §8.2 grows this by session/cause/fw
#define NS_STEP_POS   2    // …5, int32 big-endian
#define NS_STEP_SLOT  6
#define NS_STEP_LEN   7    // full stepper payload length

bool nodeStatusDecode(const uint8_t* buf, uint8_t len, NodeStatus* out) {
    if (len < NS_HEAD_LEN) return false;

    // Zero-fill first, so every field absent from this payload reads as absent
    // rather than as stale. This is what lets a node running older firmware —
    // one that does not send the §8.2 head yet — decode without special-casing.
    *out = NodeStatus{};

    out->type  = buf[NS_TYPE];
    out->flags = buf[NS_FLAGS];

    if (len >= NS_STEP_LEN) {
        out->pos = ((int32_t)buf[NS_STEP_POS]     << 24) |
                   ((int32_t)buf[NS_STEP_POS + 1] << 16) |
                   ((int32_t)buf[NS_STEP_POS + 2] <<  8) |
                    (int32_t)buf[NS_STEP_POS + 3];
        out->slot = buf[NS_STEP_SLOT];
        out->hasStepperTail = true;
    }
    return true;
}

// ─── Convenience wrappers ─────────────────────────────────────────────────────

RpcResult rpcNodeCmd(uint8_t cmd, uint8_t node, uint8_t arg) {
    RpcRequest req = {};
    req.op  = RPC_OP_NODE;
    req.cmd = cmd;
    req.node = node;
    req.argLen = 1;
    req.args[0] = arg;

    RpcReply rep;
    return rpcCall(&req, &rep, RPC_CALL_TIMEOUT_MS);
}

RpcResult rpcNodeStatus(uint8_t cmd, uint8_t node, uint8_t arg, NodeStatus* out) {
    RpcRequest req = {};
    req.op  = RPC_OP_NODE;
    req.cmd = cmd;
    req.node = node;
    req.argLen = 1;
    req.args[0] = arg;

    RpcReply rep;
    RpcResult r = rpcCall(&req, &rep, RPC_CALL_TIMEOUT_MS);
    if (r != RPC_OK) return r;

    // The node answered, but with something this decoder cannot read. That is a
    // protocol fault, not a bus timeout — but there is no result code for it
    // until CMD_NAK (§8.1), so report the conservative one.
    if (!nodeStatusDecode(rep.payload, rep.len, out)) return RPC_TIMEOUT;
    return RPC_OK;
}

RpcResult rpcSwitchGet(uint8_t node, uint8_t* level) {
    RpcRequest req = {};
    req.op   = RPC_OP_NODE;
    req.cmd  = CMD_SWITCH_GET;
    req.node = node;

    RpcReply rep;
    RpcResult r = rpcCall(&req, &rep, RPC_CALL_TIMEOUT_MS);
    if (r != RPC_OK) return r;
    if (rep.len < 1)  return RPC_TIMEOUT;
    *level = rep.payload[0];
    return RPC_OK;
}

RpcResult rpcHome(uint8_t node, uint8_t dir, uint16_t startIntervalUs,
                  uint16_t floorIntervalUs, uint16_t rampSteps,
                  uint32_t maxSteps, NodeStatus* out) {
    // The node's CMD_HOME payload, big-endian, laid out once here instead of
    // being smeared across four FIFO words and unpacked on the far side.
    RpcRequest req = {};
    req.op   = RPC_OP_NODE;
    req.cmd  = CMD_HOME;
    req.node = node;
    req.argLen = CMD_HOME_PAYLOAD_LEN;
    req.args[0]  = dir & 0x01;
    req.args[1]  = (uint8_t)(startIntervalUs >> 8);
    req.args[2]  = (uint8_t)(startIntervalUs);
    req.args[3]  = (uint8_t)(floorIntervalUs >> 8);
    req.args[4]  = (uint8_t)(floorIntervalUs);
    req.args[5]  = (uint8_t)(rampSteps >> 8);
    req.args[6]  = (uint8_t)(rampSteps);
    req.args[7]  = (uint8_t)(maxSteps >> 24);
    req.args[8]  = (uint8_t)(maxSteps >> 16);
    req.args[9]  = (uint8_t)(maxSteps >>  8);
    req.args[10] = (uint8_t)(maxSteps);

    RpcReply rep;
    RpcResult r = rpcCall(&req, &rep, RPC_CALL_TIMEOUT_MS);
    if (r != RPC_OK) return r;
    if (!nodeStatusDecode(rep.payload, rep.len, out)) return RPC_TIMEOUT;
    return RPC_OK;
}

bool rpcStepDebug(uint8_t slot, uint16_t sps, int32_t steps) {
    RpcRequest req = {};
    req.op = RPC_OP_STEP_DEBUG;      // not a node command — Core 1 acts locally
    req.id = 0;                      // fire-and-forget: no reply, no in-flight slot
    req.argLen = 7;
    req.args[0] = slot;
    req.args[1] = (uint8_t)(sps >> 8);
    req.args[2] = (uint8_t)(sps);
    // Plain two's complement, all four bytes -- the sign IS the direction, and
    // STEP_DEBUG_MAX (1e8) does not fit in three. Retires the old
    // signed-magnitude packing, which needed a sign-bit hack.
    req.args[3] = (uint8_t)((uint32_t)steps >> 24);
    req.args[4] = (uint8_t)((uint32_t)steps >> 16);
    req.args[5] = (uint8_t)((uint32_t)steps >>  8);
    req.args[6] = (uint8_t)((uint32_t)steps);
    return rpcPost(&req, nullptr);
}
