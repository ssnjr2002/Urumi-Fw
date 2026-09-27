// axes_map.cpp — the axes request, checked and applied through the slot map.
// See axes_map.h.

#include <Arduino.h>
#include <string.h>            // memcpy
#include "axes_map.h"
#include "slot_map.h"
#include "position.h"
#include "../config/machine_cfg.h"
#include "../../ipc/core1_rpc.h"

// Confirm the pending axes' types. Returns the first node that failed the check
// (SLOT_NONE if none), preferring one that answered as a non-stepper.
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
            if (!*wrongType) { bad = n; *wrongType = true; }
            continue;
        }
        axesReqClearPending(k);
    }
    return bad;
}

// Commit the axes request as the slot request and apply it, or park everything
// while an axis is pending. `bad` / `wrongType` are checkPending's report.
static AxesMapResult axesCommit(bool quiet, uint8_t bad, bool wrongType) {
    uint8_t req[MOTION_SLOTS];
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) req[k] = axesReqAt(k);

    RpcResult r = RPC_OK;
    if (axesReqPending()) {
        // Park, so a slot lent to another node (a probe's vacuum) is released
        // even when the request cannot be met yet.
        slotMapCommit(req, /*fromAxes=*/true, /*parkOnly=*/true, &r);
        if (!quiet) Serial.printf("err node %d %s\n", bad,
                                  wrongType ? "not_stepper" : "timeout");
        return AXES_PENDING;
    }
    const uint8_t failed = slotMapCommit(req, /*fromAxes=*/true, /*parkOnly=*/false, &r);
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
