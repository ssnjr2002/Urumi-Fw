// axis_map.cpp — the axis-map commit and the state it settles.

#include <Arduino.h>
#include <string.h>            // memcpy
#include "axis_map.h"
#include "position.h"
#include "state.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"
#include "hardware/sync.h"     // __dmb

// The map last passed to axisMapApply: what the bound map must equal to count
// as complete, and what `unalarm` retries.
static uint8_t requested[4];
static bool    haveRequest = false;

bool axisMapApply(const uint8_t* in, bool quiet) {
    // Copy first: `in` may be `requested` itself (axisMapRetry).
    uint8_t desired[4];
    memcpy(desired, in, sizeof desired);
    memcpy(requested, desired, sizeof requested);
    haveRequest = true;

    // NOT a diff — deliberately dumb. First disengage every previously-bound
    // node (best-effort: a since-removed/reset node that won't ACK is already
    // where we want it), then engage EVERY desired node to its slot,
    // unconditionally. Re-issuing the same axis_map therefore re-sends every
    // engage, so a node that silently lost its slot (reflash / power blip /
    // fresh Pico map) is always re-bound — the node state can never drift from
    // what the map claims, which a skip-if-unchanged diff allowed.
    // Park every bound node, recording the counter each reports (position.h,
    // the frozen-while-parked check).
    for (int i = 0; i < 4; i++) {
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
    // and enabled bit in the same transaction as the bind. This is the whole
    // point of the node-frame datum: a head parked through several rebinds
    // comes back with its position intact, and a slot that changed hands never
    // inherits the previous occupant's count.
    for (int i = 0; i < 4; i++) {
        if (desired[i] == SLOT_NONE) continue;
        NodeStatus st;
        RpcResult r = rpcNodeStatus(CMD_ENGAGE, desired[i], (uint8_t)i, &st);
        if (r != RPC_OK) {
            // Every previously-bound node was parked above, so this slot and
            // the ones after it hold nothing engaged. Unbind them rather than
            // leave stale ids that could make the map read as complete.
            for (int j = i; j < 4; j++) slotUnbind((uint8_t)j);
            axisMapGate();
            // `nak unsupported` here means a non-stepper node was mapped to a
            // motion slot — a config error, not a bus fault.
            if (!quiet) Serial.printf("err node %d %s\n", desired[i], rpcResultText(r));
            return false;
        }
        // Frozen-while-parked check (position.h). Silent by design: the
        // wire contract is exactly one line per command, so this cannot print.
        // Clearing the node's origin is the report — the axis comes back
        // un-homed, which getpos's mask and getstate both surface.
        if (st.hasStepperTail && parkMoved(desired[i], st.pos))
            originInvalidate(desired[i]);       // moved while parked
        slotBind((uint8_t)i, desired[i], &st);
    }
    // Slots left unbound hold no node, so they hold no position either.
    for (int i = 0; i < 4; i++) {
        if (desired[i] != SLOT_NONE) continue;
        slotUnbind((uint8_t)i);
    }

    axisMapGate();
    // `ok` even when the result is incomplete: committing the map is what was
    // asked for, and it succeeded. That the machine is now in ALARM_NODE_FAULT
    // is a state fact, and state facts travel as reason codes, not as errors.
    if (!quiet) Serial.println("ok");
    return true;
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

bool axisMapComplete(void) {
    if (!machineCfgValid() || !haveRequest) return false;
    for (uint8_t i = 0; i < MOTION_SLOTS; i++)
        if (slotNodeAt(i) != requested[i]) return false;
    return true;
}

bool axisMapRetry(void) {
    if (!machineCfgValid() || !haveRequest) return false;
    axisMapApply(requested, /*quiet=*/true);
    return axisMapComplete();
}

void axisMapGate(void) {
    // An incomplete map would let motion ingest (which gates on machineState
    // alone, data_plane.cpp) accept a job and stream to slots no node is
    // listening on. A probe session is the one legitimate incomplete binding;
    // its own exit path restores the map and then settles the state.
    if (machineState == STATE_PROBING) return;
    if (!axisMapComplete()) {
        // Reason before state, matching how Core 1 publishes the pair.
        alarmReason  = machineCfgValid() ? ALARM_NODE_FAULT : ALARM_CONFIG;
        __dmb();
        machineState = STATE_ALARM;
    } else if (machineState == STATE_ALARM && alarmReason == ALARM_NODE_FAULT) {
        resumeOrHold();
    }
}
