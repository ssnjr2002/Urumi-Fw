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

void rpcReset(void) {
    RpcRequest req;
    RpcReply   rep;
    while (queue_try_remove(&s_reqQ, &req)) {}
    while (queue_try_remove(&s_repQ, &rep)) {}
    s_inFlight = false;
}

bool rpcPost(const RpcRequest* req, uint16_t* idOut) {
    const bool wantsReply = (req->id != 0);
    if (wantsReply && s_inFlight) return false;

    RpcRequest r = *req;
    if (!queue_try_add(&s_reqQ, &r)) return false;

    if (wantsReply) s_inFlight = true;
    if (idOut) *idOut = r.id;
    return true;
}

bool rpcServerTake(RpcRequest* out)   { return queue_try_remove(&s_reqQ, out); }
bool rpcServerReply(const RpcReply* rep) { return queue_try_add(&s_repQ, rep); }

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

// Reason from the last completed call. Safe as a single global because rpcPost
// admits one reply-bearing transaction at a time.
static uint8_t s_lastNakReason = 0;

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
                out->result = RPC_BAD_REPLY;
                out->len = 0;
                return RPC_BAD_REPLY;
            }
            s_lastNakReason = (out->result == RPC_NAK) ? out->nakReason : 0;
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

const char* rpcResultText(RpcResult r) {
    switch (r) {
        case RPC_OK:        return "ok";
        case RPC_TIMEOUT:   return "timeout";
        case RPC_BAD_REPLY: return "bad_reply";
        case RPC_NAK: break;
    }
    switch (s_lastNakReason) {
        case NAK_UNSUPPORTED:      return "nak unsupported";
        case NAK_BAD_TOKEN:        return "nak bad_token";
        case NAK_BAD_ARG:          return "nak bad_arg";
        case NAK_INTENT_MISMATCH:  return "nak intent_mismatch";
        default: break;
    }
    // An unknown reason still reports as a nak. Degrading it to "timeout" would
    // undo the whole point of the opcode on the first firmware that adds one.
    static char buf[16];
    snprintf(buf, sizeof buf, "nak %u", (unsigned)s_lastNakReason);
    return buf;
}

// ─── Node status decoding ─────────────────────────────────────────────────────
// Field offsets. These do not leave this file — that is the whole point of
// decoding into a struct. Consumers read st.pos, never buf[NS_STEP_POS].
#define NS_TYPE       0
#define NS_FLAGS      1
#define NS_HEAD_LEN   2    // [type][flags] — §8.2 grows this by session/cause/fw
#define NS_STEP_POS   2    // …5, int32 big-endian
#define NS_STEP_SLOT  6
#define NS_HOME_KIND  7    // HOMING_KIND_* — DECLARED, not inferred from length
#define NS_STEP_LEN   8    // full stepper payload length
#define NS_STEP_SPAN  8    // …11, int32 big-endian — homing-capable boards only
#define NS_SPAN_LEN   12   // stepper payload length WITH the homing span
#define NS_IDX_POS   12    // …15, int32 big-endian — rotary (Hall index) only
#define NS_IDX_CAUSE 16
#define NS_IDX_LEN   17    // stepper payload length WITH the rotary index
#define NS_HALL_RAW  17    // …18, int16 BE — live sensor value (bring-up)
#define NS_HALL_BASE 19    // …20, int16 BE — last sweep's baseline
#define NS_HALL_LEN  21
#define NS_LAP_STEPS 21    // …24, int32 BE — measured steps per revolution
#define NS_LAP_CROSS 25    // index crossings the sweep completed
#define NS_LAP_LEN   26

bool nodeStatusDecode(const uint8_t* buf, uint8_t len, NodeStatus* out) {
    if (len < NS_HEAD_LEN) return false;

    // Zero-fill first, so every field absent from this payload reads as absent
    // rather than as stale. This is what lets a node running older firmware —
    // one that does not send the §8.2 head yet — decode without special-casing.
    *out = NodeStatus{};

    out->type  = buf[NS_TYPE];
    out->flags = buf[NS_FLAGS];

    out->tailLen = (uint8_t)(len - NS_HEAD_LEN);
    if (out->tailLen) memcpy(out->tail, &buf[NS_HEAD_LEN], out->tailLen);

    if (len >= NS_STEP_LEN) {
        out->pos = ((int32_t)buf[NS_STEP_POS]     << 24) |
                   ((int32_t)buf[NS_STEP_POS + 1] << 16) |
                   ((int32_t)buf[NS_STEP_POS + 2] <<  8) |
                    (int32_t)buf[NS_STEP_POS + 3];
        out->slot = buf[NS_STEP_SLOT];
        out->homingKind = buf[NS_HOME_KIND];
        out->hasStepperTail = true;
    }

    // Everything below is gated on the node's DECLARED kind, not on how long the
    // payload happens to be. Length still bounds each read -- a truncated frame
    // must not be indexed past -- but it no longer DECIDES what the bytes mean.
    // Inferring from length conflated capability with firmware age and would
    // have silently mis-decoded a linear node as rotary the first time anyone
    // appended a field to the linear tail.
    const bool rotary = (out->homingKind == HOMING_KIND_INDEX);

    // Appended by boards that have a switch, so its absence is a fact about the
    // node (no switch ⇒ no homing ⇒ no leg to measure), not about the firmware
    // being old. Either way `hasHomeSpan` says so rather than letting a
    // zero-filled 0 read as "the last leg travelled nothing".
    if (len >= NS_SPAN_LEN) {
        out->homeSpan = ((int32_t)buf[NS_STEP_SPAN]     << 24) |
                        ((int32_t)buf[NS_STEP_SPAN + 1] << 16) |
                        ((int32_t)buf[NS_STEP_SPAN + 2] <<  8) |
                         (int32_t)buf[NS_STEP_SPAN + 3];
        out->hasHomeSpan = true;
    }

    // Third length, same rule. A rotary node reports WHERE ITS INDEX IS, which
    // is not where the axis stopped -- a dip's centre is only knowable after
    // passing it, so the sweep runs through the feature and `pos` is somewhere
    // past it. Both numbers are real and neither substitutes for the other.
    //
    // The cause rides alongside because `index` is meaningful only for
    // ROTARY_IDX_OK: a sweep that found nothing has no index, and a 0 there is a
    // legitimate step coordinate rather than a sentinel.
    if (rotary && len >= NS_IDX_LEN) {
        out->indexPos = ((int32_t)buf[NS_IDX_POS]     << 24) |
                        ((int32_t)buf[NS_IDX_POS + 1] << 16) |
                        ((int32_t)buf[NS_IDX_POS + 2] <<  8) |
                         (int32_t)buf[NS_IDX_POS + 3];
        out->indexCause = buf[NS_IDX_CAUSE];
        out->hasIndex   = true;
    }
    if (rotary && len >= NS_HALL_LEN) {
        out->hallRaw      = (int16_t)(((uint16_t)buf[NS_HALL_RAW]  << 8) |
                                                  buf[NS_HALL_RAW + 1]);
        out->hallBaseline = (int16_t)(((uint16_t)buf[NS_HALL_BASE] << 8) |
                                                  buf[NS_HALL_BASE + 1]);
    }
    if (rotary && len >= NS_LAP_LEN) {
        out->stepsPerRev = ((int32_t)buf[NS_LAP_STEPS]     << 24) |
                           ((int32_t)buf[NS_LAP_STEPS + 1] << 16) |
                           ((int32_t)buf[NS_LAP_STEPS + 2] <<  8) |
                            (int32_t)buf[NS_LAP_STEPS + 3];
        out->crossings   = buf[NS_LAP_CROSS];
        out->hasLap      = true;
    }
    return true;
}

const char* rotaryIdxCauseText(uint8_t cause) {
    switch (cause) {
        case ROTARY_IDX_NONE:       return "none";
        case ROTARY_IDX_OK:         return "ok";
        case ROTARY_IDX_NOTFOUND:   return "notfound";
        case ROTARY_IDX_DEGENERATE: return "degenerate";
        case ROTARY_IDX_OVERFLOW:   return "overflow";
        case ROTARY_IDX_SLIP:       return "slip";
        default: break;
    }
    // Same discipline as rpcResultText: an unknown cause prints as itself
    // rather than degrading to one of the known words, which would make the
    // first firmware that adds a cause report a confident wrong reason.
    static char buf[16];
    snprintf(buf, sizeof buf, "cause_%u", (unsigned)cause);
    return buf;
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

    if (!nodeStatusDecode(rep.payload, rep.len, out)) return RPC_BAD_REPLY;
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
    if (rep.len < 1)  return RPC_BAD_REPLY;
    *level = rep.payload[0];
    return RPC_OK;
}

RpcResult rpcHome(uint8_t node, uint8_t dir, bool intendedRetract,
                  uint16_t startIntervalUs, uint16_t floorIntervalUs,
                  uint16_t rampSteps, uint32_t maxSteps, NodeStatus* out) {
    // The node's CMD_HOME_LEG payload, big-endian, laid out once here instead of
    // being smeared across four FIFO words and unpacked on the far side.
    RpcRequest req = {};
    req.op   = RPC_OP_NODE;
    req.cmd  = CMD_HOME_LEG;
    req.node = node;
    req.argLen = CMD_HOME_LEG_PAYLOAD_LEN;
    // bit0 = dir, bit1 = intent (include/common.h, CMD_HOME_LEG payload).
    req.args[0]  = (dir & 0x01) | (intendedRetract ? 0x02 : 0x00);
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
    if (!nodeStatusDecode(rep.payload, rep.len, out)) return RPC_BAD_REPLY;
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
