// slot_map.cpp — the slot request, its apply, and the state it settles. See
// slot_map.h.

#include <Arduino.h>
#include <string.h>            // memcpy
#include "slot_map.h"
#include "position.h"
#include "state.h"
#include "../../ipc/shared_state.h"
#include "hardware/sync.h"     // __dmb

// The slot request: what the binding must equal to count as complete, and what
// a retry re-applies. `fromAxes` = written by the axes map.
static uint8_t slotReq[MOTION_SLOTS];
static bool    haveSlotReq = false;
static bool    fromAxes    = false;

uint8_t nodeStatusSlot(const NodeStatus* st) {
    if (st->hasStepperTail) return st->slot;
    if (st->type == NODE_TYPE_VACUUM && st->tailLen >= 3) return st->tail[2];
    return SLOT_NONE;
}

RpcResult slotMakeSafe(uint8_t n, NodeStatus* st) {
    RpcResult r = rpcNodeStatus(CMD_MAKE_SAFE, n, 0, st);
    if (r == RPC_OK && nodeStatusSlot(st) != SLOT_NONE) r = RPC_BAD_REPLY;
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        if (slotNodeAt(i) != n) continue;
        if (r == RPC_OK) slotUnbind(i);
        else             slotFence(i, n);
    }
    return r;
}

// Park every unfenced slot holder, then engage `in` except the `skip` slots.
// A holder that does not confirm its park, or a node that does not answer its
// engage, fences the slot. A fenced slot the request collides with first gets
// make-safe; bit i of `*fenced` = slot i stayed fenced. Returns SLOT_NONE, or
// the node that refused to engage with its result in `*res`.
static uint8_t applySlots(const uint8_t* in, uint8_t skip, RpcResult* res,
                          uint8_t* fenced) {
    // Copy first: `in` may be slotReq itself.
    uint8_t desired[MOTION_SLOTS];
    memcpy(desired, in, sizeof desired);
    *fenced = 0;

    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        if (!slotFencedAt(i) || desired[i] == SLOT_NONE || (skip & (1 << i))) continue;
        NodeStatus st;
        if (slotMakeSafe(slotNodeAt(i), &st) != RPC_OK) *fenced |= (1 << i);
    }

    // NOT a diff. Re-issuing the same map re-sends every engage, so a node that
    // silently lost its slot (reflash / power blip / fresh Pico map) is always
    // re-bound. Park every bound node first, recording the counter each reports
    // (position.h, the frozen-while-parked check). Only a confirmed park frees
    // the slot.
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        const uint8_t n = slotNodeAt(i);
        if (n == SLOT_NONE || slotFencedAt(i)) continue;
        NodeStatus st;
        if (rpcNodeStatus(CMD_ENGAGE, n, SLOT_NONE, &st) == RPC_OK &&
            nodeStatusSlot(&st) == SLOT_NONE) {
            if (st.hasStepperTail) parkRecord(n, st.pos);
            slotUnbind(i);
        } else {
            parkForget(n);
            slotFence(i, n);
        }
    }

    // Engage, and adopt each slot's state straight out of the ack — position
    // and enabled bit in the same transaction as the bind. A head parked
    // through several rebinds comes back with its position intact, and a slot
    // that changed hands never inherits the previous occupant's count.
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        const uint8_t n = desired[i];
        if (n == SLOT_NONE || (skip & (1 << i)) || slotFencedAt(i)) continue;
        NodeStatus st;
        RpcResult r = rpcNodeStatus(CMD_ENGAGE, n, i, &st);
        if (r != RPC_OK) {
            // A NAK is a confirmed refusal; anything else may have engaged.
            if (r != RPC_NAK) slotFence(i, n);
            *res = r;
            return n;
        }
        // Its reply shows it in slot i, so any slot it was fenced in is free.
        for (uint8_t j = 0; j < MOTION_SLOTS; j++)
            if (j != i && slotNodeAt(j) == n) slotUnbind(j);
        // Frozen-while-parked check (position.h). Silent by design: clearing
        // the node's origin is the report — the axis comes back un-homed.
        if (st.hasStepperTail && parkMoved(n, st.pos))
            originInvalidate(n);                // moved while parked
        slotBind(i, n, &st);
    }
    return SLOT_NONE;
}

uint8_t slotMapCommit(const uint8_t* req, bool axes, uint8_t skip, RpcResult* res,
                      uint8_t* fenced) {
    memcpy(slotReq, req, sizeof slotReq);
    haveSlotReq = true;
    fromAxes    = axes;
    const uint8_t bad = applySlots(slotReq, skip, res, fenced);
    slotMapGate();
    return bad;
}

void slotMapPrintFenced(uint8_t fenced) {
    Serial.print("err fenced");
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        if (fenced & (1 << i)) Serial.printf(" %d", slotNodeAt(i));
        else                   Serial.print(" -");
    }
    Serial.println();
}

void slotMapDrop(uint8_t n) {
    for (uint8_t i = 0; i < MOTION_SLOTS; i++)
        if (slotReq[i] == n) slotReq[i] = SLOT_NONE;
}

bool slotMapApply(const uint8_t* desired, bool quiet, uint8_t* failed) {
    RpcResult r = RPC_OK;
    uint8_t fenced;
    uint8_t bad = slotMapCommit(desired, /*fromAxes=*/false, /*skip=*/0, &r, &fenced);
    if (fenced) {
        for (uint8_t i = 0; i < MOTION_SLOTS && bad == SLOT_NONE; i++)
            if (fenced & (1 << i)) bad = slotNodeAt(i);
        if (failed) *failed = bad;
        if (!quiet) slotMapPrintFenced(fenced);
        return false;
    }
    if (failed) *failed = bad;
    if (bad != SLOT_NONE) {
        // `nak unsupported` here means a node type that takes no slot.
        if (!quiet) Serial.printf("err node %d %s\n", bad, rpcResultText(r));
        return false;
    }
    // `ok` even for a partial map: committing it is what was asked for.
    if (!quiet) Serial.println("ok");
    return true;
}

bool slotMapComplete(void) {
    if (!haveSlotReq) return true;         // nothing requested, nothing missing
    // A fenced slot satisfies `-` only: its node is not engaged.
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        if (slotFencedAt(i) ? slotReq[i] != SLOT_NONE : slotNodeAt(i) != slotReq[i])
            return false;
    }
    return true;
}

bool slotMapFromAxes(void) { return haveSlotReq && fromAxes; }

bool slotMapRetry(void) {
    if (!haveSlotReq) return true;
    RpcResult r;
    uint8_t fenced;
    applySlots(slotReq, /*skip=*/0, &r, &fenced);
    slotMapGate();
    return slotMapComplete();
}

void slotMapForget(void) {
    haveSlotReq = false;
    fromAxes    = false;
}

void slotMapGate(void) {
    // An unmet request would let motion ingest accept a job and stream to slots
    // no node is listening on. A probe's binding is a met slot request like any
    // other, so NODE_FAULT never needs to know about it.
    if (!slotMapComplete()) {
        // Reason before state, matching how Core 1 publishes the pair.
        alarmReason  = ALARM_NODE_FAULT;
        __dmb();
        machineState = STATE_ALARM;
    } else if (machineState == STATE_ALARM && alarmReason == ALARM_NODE_FAULT) {
        resumeOrHold();
    }
}
