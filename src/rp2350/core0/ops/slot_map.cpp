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

// Park every slot holder, then engage `in`. Returns SLOT_NONE, or the node that
// refused to engage with its result in `*res`.
static uint8_t applySlots(const uint8_t* in, RpcResult* res) {
    // Copy first: `in` may be slotReq itself.
    uint8_t desired[MOTION_SLOTS];
    memcpy(desired, in, sizeof desired);

    // NOT a diff. Re-issuing the same map re-sends every engage, so a node that
    // silently lost its slot (reflash / power blip / fresh Pico map) is always
    // re-bound. Park every bound node first (best-effort: a node that won't ACK
    // is already where we want it), recording the counter each reports
    // (position.h, the frozen-while-parked check).
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        uint8_t n = slotNodeAt(i);
        if (n == SLOT_NONE) continue;
        NodeStatus st;
        if (rpcNodeStatus(CMD_ENGAGE, n, SLOT_NONE, &st) == RPC_OK &&
            st.hasStepperTail)
            parkRecord(n, st.pos);
        else
            // No answer — we do not know where it stopped. Drop any earlier
            // entry rather than let a stale one produce a false match later.
            parkForget(n);
    }

    // Engage, and adopt each slot's state straight out of the ack — position
    // and enabled bit in the same transaction as the bind. A head parked
    // through several rebinds comes back with its position intact, and a slot
    // that changed hands never inherits the previous occupant's count.
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        if (desired[i] == SLOT_NONE) continue;
        NodeStatus st;
        RpcResult r = rpcNodeStatus(CMD_ENGAGE, desired[i], i, &st);
        if (r != RPC_OK) {
            // Every previously-bound node was parked above, so this slot and
            // the ones after it hold nothing engaged. Unbind them rather than
            // leave stale ids that could make the map read as complete.
            for (uint8_t j = i; j < MOTION_SLOTS; j++) slotUnbind(j);
            *res = r;
            return desired[i];
        }
        // Frozen-while-parked check (position.h). Silent by design: clearing
        // the node's origin is the report — the axis comes back un-homed.
        if (st.hasStepperTail && parkMoved(desired[i], st.pos))
            originInvalidate(desired[i]);       // moved while parked
        slotBind(i, desired[i], &st);
    }
    for (uint8_t i = 0; i < MOTION_SLOTS; i++)
        if (desired[i] == SLOT_NONE) slotUnbind(i);
    return SLOT_NONE;
}

uint8_t slotMapCommit(const uint8_t* req, bool axes, bool parkOnly, RpcResult* res) {
    memcpy(slotReq, req, sizeof slotReq);
    haveSlotReq = true;
    fromAxes    = axes;
    static const uint8_t none[MOTION_SLOTS] = { SLOT_NONE, SLOT_NONE, SLOT_NONE, SLOT_NONE };
    const uint8_t bad = applySlots(parkOnly ? none : slotReq, res);
    slotMapGate();
    return bad;
}

bool slotMapApply(const uint8_t* desired, bool quiet, uint8_t* failed) {
    RpcResult r = RPC_OK;
    const uint8_t bad = slotMapCommit(desired, /*fromAxes=*/false, /*parkOnly=*/false, &r);
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
    for (uint8_t i = 0; i < MOTION_SLOTS; i++)
        if (slotNodeAt(i) != slotReq[i]) return false;
    return true;
}

bool slotMapFromAxes(void) { return haveSlotReq && fromAxes; }

bool slotMapRetry(void) {
    if (!haveSlotReq) return true;
    RpcResult r;
    applySlots(slotReq, &r);
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
