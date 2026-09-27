// axis_map.cpp — the slot map, the axes map on top of it, and the state they
// settle. See axis_map.h.

#include <Arduino.h>
#include <string.h>            // memcpy
#include "axis_map.h"
#include "position.h"
#include "state.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"
#include "hardware/sync.h"     // __dmb

// The slot request: what the binding must equal to count as complete, and what
// `unalarm` retries. `fromAxes` = it was written by the axes map, whose pending
// axes the retry re-checks first.
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

bool slotMapApply(const uint8_t* desired, bool quiet, uint8_t* failed) {
    memcpy(slotReq, desired, sizeof slotReq);
    haveSlotReq = true;
    fromAxes    = false;
    RpcResult r = RPC_OK;
    const uint8_t bad = applySlots(slotReq, &r);
    slotMapGate();
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

void slotMapForget(void) {
    haveSlotReq = false;
    fromAxes    = false;
    axesReqForget();
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

// Confirm the pending axes' types. Returns the first node that failed the check
// (SLOT_NONE if none) and whether it answered as a non-stepper.
static uint8_t checkPending(bool* wrongType) {
    uint8_t bad = SLOT_NONE;
    *wrongType = false;
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) {
        if (!(axesReqPending() & (1 << k))) continue;
        const uint8_t n = axesReqAt(k);
        NodeStatus st;
        if (rpcNodeStatus(CMD_NODE_STATUS, n, 0, &st) != RPC_OK) {
            if (bad == SLOT_NONE) bad = n;
            continue;
        }
        if (st.type != NODE_TYPE_STEPPER) {
            if (bad == SLOT_NONE || !*wrongType) { bad = n; *wrongType = true; }
            continue;
        }
        axesReqClearPending(k);
    }
    return bad;
}

// Commit the axes request as the slot request and apply it, or park everything
// while an axis is pending. `bad` / `wrongType` are checkPending's report.
static AxesMapResult axesCommit(bool quiet, uint8_t bad, bool wrongType) {
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) slotReq[k] = axesReqAt(k);
    haveSlotReq = true;
    fromAxes    = true;

    RpcResult r = RPC_OK;
    if (axesReqPending()) {
        // Park, so a slot lent to another node (a probe's vacuum) is released
        // even when the request cannot be met yet.
        const uint8_t none[MOTION_SLOTS] = { SLOT_NONE, SLOT_NONE, SLOT_NONE, SLOT_NONE };
        applySlots(none, &r);
        slotMapGate();
        if (!quiet) Serial.printf("err node %d %s\n", bad,
                                  wrongType ? "not_stepper" : "timeout");
        return AXES_PENDING;
    }
    const uint8_t failed = applySlots(slotReq, &r);
    slotMapGate();
    if (failed != SLOT_NONE) {
        if (!quiet) Serial.printf("err node %d %s\n", failed, rpcResultText(r));
        return AXES_ENGAGE;
    }
    if (!quiet) Serial.println("ok");
    return AXES_OK;
}

AxesMapResult axesMapApply(const uint8_t* in, bool quiet, bool keepWrongType) {
    uint8_t ids[MOTION_SLOTS];
    memcpy(ids, in, sizeof ids);

    // Stage: remember the current request, write the new one all pending, and
    // put the old one back if a wrong type refuses the map.
    uint8_t oldIds[MOTION_SLOTS];
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) oldIds[k] = axesReqAt(k);
    const uint8_t oldPending = axesReqPending();

    uint8_t pending = 0;
    for (uint8_t k = 0; k < MOTION_SLOTS; k++)
        if (ids[k] != SLOT_NONE) pending |= (1 << k);
    axesReqSet(ids, pending);

    bool wrongType;
    const uint8_t bad = checkPending(&wrongType);
    if (wrongType && !keepWrongType) {
        axesReqSet(oldIds, oldPending);
        if (!quiet) Serial.printf("err node %d not_stepper\n", bad);
        return AXES_NOT_STEPPER;
    }
    return axesCommit(quiet, bad, wrongType);
}

AxesMapResult axesMapRetry(bool quiet) {
    bool wrongType;
    const uint8_t bad = checkPending(&wrongType);
    return axesCommit(quiet, bad, wrongType);
}

bool slotMapRetry(void) {
    if (!haveSlotReq) return true;
    if (fromAxes) {
        axesMapRetry(/*quiet=*/true);
    } else {
        RpcResult r;
        applySlots(slotReq, &r);
        slotMapGate();
    }
    return slotMapComplete();
}

bool axisNodeInConfig(uint8_t node) {
    if (!machineCfgValid()) return false;
    const MachineCfg& c = machineCfg();
    const CfgAxis* axes[2] = { &c.x, &c.y };
    for (const CfgAxis* a : axes)
        if (a->node.present && a->node.id == node) return true;
    for (uint8_t h = 0; h < c.headCount; h++) {
        const CfgHead& hd = c.heads[h];
        if (hd.z.node.present && hd.z.node.id == node) return true;
        if (hd.a.node.present && hd.a.node.id == node) return true;
    }
    return false;
}
