// axes_map.cpp — the axes request, checked and applied through the slot map.
// See axes_map.h.

#include <Arduino.h>
#include <string.h>            // memcpy
#include "axes_map.h"
#include "slot_map.h"
#include "position.h"
#include "../config/machine_cfg.h"
#include "../../ipc/core1_rpc.h"

// What checkPending found wrong, worst first.
enum PendingFault : uint8_t { PF_NONE, PF_TIMEOUT, PF_FENCED, PF_WRONG_TYPE };

// True when node `n` holds a fenced slot.
static bool nodeFenced(uint8_t n) {
    for (uint8_t i = 0; i < MOTION_SLOTS; i++)
        if (slotNodeAt(i) == n && slotFencedAt(i)) return true;
    return false;
}

// Confirm the pending axes' types. A fenced node's make-safe reply is its type
// check (it answers with status), since clearing the fence costs it the datum
// anyway; every other node gets the non-destructive CMD_NODE_STATUS. Returns
// the node behind the worst fault in `*fault`; bit i of `*fenced` = slot i's
// fence could not be cleared.
static uint8_t checkPending(PendingFault* fault, uint8_t* fenced) {
    uint8_t bad = SLOT_NONE;
    *fault  = PF_NONE;
    *fenced = 0;
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) {
        if (!(axesReqPending() & (1 << k))) continue;
        const uint8_t n = axesReqAt(k);
        NodeStatus st;
        PendingFault f = PF_NONE;
        if (nodeFenced(n)) {
            const uint8_t s = nodeSlot(n);
            if (slotMakeSafe(n, &st) != RPC_OK) { f = PF_FENCED; *fenced |= (1 << s); }
        } else if (rpcNodeStatus(CMD_NODE_STATUS, n, 0, &st) != RPC_OK) {
            f = PF_TIMEOUT;
        }
        if (f == PF_NONE && st.type != NODE_TYPE_STEPPER) f = PF_WRONG_TYPE;
        if (f == PF_NONE) { axesReqClearPending(k); continue; }
        if (f > *fault) { *fault = f; bad = n; }
    }
    return bad;
}

// Commit the axes request as the slot request and apply it. A pending axis's
// slot is parked, not engaged. `bad`, `fault` and `fenced` are checkPending's
// report.
static AxesMapResult axesCommit(bool quiet, uint8_t bad, PendingFault fault,
                                uint8_t fenced) {
    uint8_t req[MOTION_SLOTS];
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) req[k] = axesReqAt(k);

    RpcResult r = RPC_OK;
    uint8_t mapFenced;
    const uint8_t pending = axesReqPending();
    const uint8_t failed = slotMapCommit(req, /*fromAxes=*/true, pending, &r, &mapFenced);
    fenced |= mapFenced;
    if (fenced) {
        if (!quiet) slotMapPrintFenced(fenced);
        return pending ? AXES_PENDING : AXES_ENGAGE;
    }
    if (pending) {
        if (!quiet) Serial.printf("err node %d %s\n", bad,
                                  fault == PF_WRONG_TYPE ? "not_stepper" : "timeout");
        return AXES_PENDING;
    }
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

    PendingFault fault;
    uint8_t fenced;
    const uint8_t bad = checkPending(&fault, &fenced);
    if (fault == PF_WRONG_TYPE && !keepWrongType) {
        axesReqSet(oldIds, oldPending);
        if (!quiet) Serial.printf("err node %d not_stepper\n", bad);
        return AXES_NOT_STEPPER;
    }
    return axesCommit(quiet, bad, fault, fenced);
}

AxesMapResult axesMapRetry(bool quiet) {
    PendingFault fault;
    uint8_t fenced;
    const uint8_t bad = checkPending(&fault, &fenced);
    return axesCommit(quiet, bad, fault, fenced);
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
