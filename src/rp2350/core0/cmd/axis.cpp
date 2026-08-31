// axis.cpp — the commands that bind, datum, energise, or step an axis.
//
// This is the ONLY file that writes the position model. Before the split,
// origin invalidation was scattered across disable, axes_enable, bus_enable,
// axis_map, setorigin and reconcileValidity inside one 1000-line file, and the
// recurring bug the source comments describe is exactly "updated one frame,
// forgot the other". Now there is one file to audit, and it reaches the model
// only through position.h's named operations.

#include <Arduino.h>
#include "table.h"
#include "parse.h"
#include "gate.h"
#include "../position.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"

// The gate shared by every command here that goes to the bus: Core 1 services
// channel 1 only after draining the ring, so a request issued mid-stream waits
// out the whole queue while Core 0 blocks and stops reading serial.
static inline bool busGateDenies() {
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state");
        return true;
    }
    return false;
}

// ── axes_enable <on|off> (IDLE/PAUSED/ALARM) ─────────────────────────────────
// Targets the axis map: every node currently bound to a motion slot, and no one
// else. This replaces the old `enable all` / `disable all`, whose name read
// bus-wide while the code always walked the axis map — a distinction that
// stopped being academic once vacuum and knife nodes joined the bus.
// Peripherals hold no slot, so they are addressed only by `enable <id>`.
bool cmdAxesEnable(const char* args) {
    if (busGateDenies()) return true;
    if (*args == '\0') { Serial.println("err usage"); return true; }
    // Deliberately NOT gated by alarmDeniesOn: ALARM is where axis recovery
    // happens. Boot sits in ALARM_CONFIG, and the post-estop flow is
    // axes_enable on → setorigin → unalarm. `enable <id>` is ungated for the
    // same reason. The peripheral commands gate because energising a pump
    // under alarm has no such recovery role.
    bool on = parseState(args);       // accepts "1"/"on" and "0"/"off"
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        uint8_t n = slotNodeAt(i);
        if (n == SLOT_NONE) continue;
        rpcNodeCmd(on ? CMD_ENABLE : CMD_DISABLE, n, 0);
        if (on) axes_enabled |=  (1 << i);
        else    axes_enabled &= ~(1 << i);
    }
    // De-energised → back-drivable → every bound origin is void. Keyed on the
    // node, not the slot, so a node that loses holding torque while PARKED
    // still loses its origin (see originInvalidate).
    if (!on)
        for (uint8_t i = 0; i < MOTION_SLOTS; i++)
            if (slotNodeAt(i) != SLOT_NONE) originInvalidate(slotNodeAt(i));
    Serial.println("ok");
    return true;
}

// ── bus_enable <on|off> (IDLE/PAUSED/ALARM) ──────────────────────────────────
// Whole-bus broadcast: ONE unacknowledged frame reaches every node at once,
// peripherals included. This is the genuinely bus-wide verb that the old
// `enable all` only claimed to be; `axes_enable` remains the axis-map form.
//
// Nobody answers a broadcast, so this cannot learn what actually happened.
// The bookkeeping is therefore deliberately ASYMMETRIC, in the direction that
// is safe to be wrong in:
//   off → clear axes_enabled and void every origin. If a node missed the
//         frame we under-claim (think it's off when it's live) — the operator
//         is told less is armed than is, and position is invalid regardless.
//   on  → touch NOTHING. Motion gates on axes_enabled, so believing a node
//         armed when it never heard us is the direction that moves a machine
//         that isn't ready. Use `axes_enable on` to actually arm the map; it
//         relays per node and gets an ACK for each.
// TODO(verify): once the CMD_NODE_STATUS poll lands, `on` can set the bits
// from what the nodes report rather than staying silent.
bool cmdBusEnable(const char* args) {
    if (busGateDenies()) return true;
    if (*args == '\0') { Serial.println("err usage"); return true; }
    bool on = parseState(args);
    rpcNodeCmd(on ? CMD_ENABLE : CMD_DISABLE, BUS_ADDR_BROADCAST, 0);
    if (!on) {
        axes_enabled = 0;
        originInvalidateAll();
    }
    Serial.println("ok");
    return true;
}

// ── enable / disable <id> (IDLE/PAUSED/ALARM) ────────────────────────────────
// Relays to any bus node — the generic CMD_ENABLE effect is delegated per type
// (motor energize / pump on …); the axis bookkeeping applies only when the id
// is an axis node (docs/engage_and_axis_map.md §9).
bool cmdEnable(const char* args) {
    if (busGateDenies()) return true;
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err bad_node"); return true; }
    rpcNodeCmd(CMD_ENABLE, node, 0);
    uint8_t s = nodeSlot(node);      // axis bookkeeping keyed on the slot
    if (s != SLOT_NONE) axes_enabled |= (1 << s);
    Serial.println("ok");
    return true;
}

bool cmdDisable(const char* args) {
    if (busGateDenies()) return true;
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err bad_node"); return true; }
    rpcNodeCmd(CMD_DISABLE, node, 0);
    // Unconditional: a de-energised node is back-drivable whether or not
    // it currently holds a slot, so its origin is void either way. This
    // is exactly the case slot-indexed bookkeeping could not express —
    // a PARKED head losing holding torque and sagging under gravity.
    originInvalidate(node);
    uint8_t s = nodeSlot(node);
    if (s != SLOT_NONE) axes_enabled &= ~(1 << s);
    Serial.println("ok");
    return true;
}

// ── axis_map [<x> <y> <z> <a>] — bind bus nodes to stream slots ──────────────
// No-arg: read back the committed map in setter syntax ('-' = unbound slot).
// Four tokens (a bus id, or '-'/'0' = unbound). A successful commit clears the
// ALARM_CONFIG boot gate. Valid IDLE/PAUSED/ALARM; rebinding mid-RUNNING
// corrupts motion (§6.2).
bool cmdAxisMap(const char* args) {
    if (*args == '\0') {                          // read-back form
        Serial.print("axis_map");
        for (int i = 0; i < 4; i++) {
            if (slotNodeAt(i) == SLOT_NONE) Serial.print(" -");
            else                            Serial.printf(" %d", slotNodeAt(i));
        }
        Serial.println();
        return true;
    }

    if (busGateDenies()) return true;

    // Parse exactly four tokens into desired[]: a bus id, or '-'/'0' = unbound.
    uint8_t desired[4];
    const char* p = args;
    for (int i = 0; i < 4; i++) {
        while (*p == ' ') p++;
        if (*p == '\0') { Serial.println("err usage"); return true; }
        if (*p == '-') { desired[i] = SLOT_NONE; p++; continue; }
        char* endPtr;
        unsigned long v = strtoul(p, &endPtr, 10);
        if (endPtr == p) { Serial.println("err usage"); return true; }
        p = endPtr;
        if (v == 0)                 desired[i] = SLOT_NONE;
        else if (v <= BUS_ADDR_MAX) desired[i] = (uint8_t)v;
        else { Serial.println("err bad_node"); return true; }
    }
    // A bus id can occupy only one slot — reject a node bound twice.
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (desired[i] != SLOT_NONE && desired[i] == desired[j]) {
                Serial.println("err dup"); return true;
            }

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
        if (rpcNodeStatus(CMD_ENGAGE, desired[i], (uint8_t)i, &st) != RPC_OK) {
            Serial.printf("err node %d timeout\n", desired[i]);
            return true;              // leave the map as-is; a retry redoes all
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

    // Committed — clear the config gate if that is what was holding us.
    if (machineState == STATE_ALARM && alarmReason == ALARM_CONFIG) {
        machineState = STATE_IDLE;
        alarmReason  = ALARM_NONE;
    }
    Serial.println("ok");
    return true;
}

// ── setorigin [axes] (IDLE/PAUSED/ALARM) ─────────────────────────────────────
bool cmdSetOrigin(const char* args) {
    if (busGateDenies()) return true;
    uint8_t m = axisMask(args);
    // setorigin does bus I/O below — up to four round trips, so it can be in
    // flight for tens of milliseconds. An estop landing inside that window
    // would otherwise be ERASED by the alarm-clearing block at the end, which
    // cannot tell "the fault I was invoked to recover from" apart from "a
    // fault that arrived while I was working". Snapshot the reason on entry
    // and only clear what we came in with.
    uint8_t alarmAtEntry = alarmReason;
    // The datum is recorded in the NODE's frame: originRecord captures that
    // node's own counter here, so machinePos is a derived offset from now on
    // and survives any later rebinding. A masked slot with no node bound
    // cannot be datumed — there is nothing to record against — so it is
    // skipped and left un-homed rather than silently claiming an origin.
    for (int i = 0; i < 4; i++) {
        if (!(m & (1 << i))) continue;
        uint8_t n = slotNodeAt(i);
        if (n == SLOT_NONE) { axes_homed &= ~(1 << i); continue; }

        // CMD_DATUM_SET arms the node's continuity witness AND returns the
        // counter it refers to. One transaction, so the origin recorded here
        // and the witness armed there describe the same instant — a separate
        // read could straddle a reset and pair a witness with a stale count.
        NodeStatus st;
        if (rpcNodeStatus(CMD_DATUM_SET, n, 0, &st) != RPC_OK ||
            !st.hasStepperTail || !(st.flags & NODE_FLAG_DATUM)) {
            originInvalidate(n);           // no answer, or witness not armed
            continue;
        }
        originRecord(n, st.pos);
    }
    // A fault that arrived while we were on the bus outranks this command. The
    // datum we just recorded describes a machine that has since stopped hard,
    // so refuse rather than clear it — reconcileValidity() drops the masks on
    // the next pass, and the operator retries after unalarm.
    if (alarmReason != alarmAtEntry || machineState == STATE_ESTOP) {
        Serial.println("err estop"); return true;
    }
    // setorigin recovers from an ESTOP-alarm, but NOT the config gate — only a
    // committed axis_map clears ALARM_CONFIG (docs/engage_and_axis_map.md §6.1).
    if (machineState == STATE_ALARM && alarmReason != ALARM_CONFIG) {
        machineState = STATE_IDLE;
        alarmReason  = ALARM_NONE;
    }
    Serial.println("ok");
    return true;
}

// ── home <node> <dir> <start_us> <floor_us> <ramp_steps> <max_steps> ─────────
//
// Bench bring-up only. Deliberately raw and positional: no mm, no steps/mm, no
// config lookup, no `invert`, and NO seek/retract argument. Composing those
// belongs to the host (docs/homing.md 3), and a temporary Pico-side version of
// them is exactly how they end up living here permanently. The Pico relays; it
// does not plan.
//
// No state gate either, on purpose — this has to be usable from ALARM while the
// machine is being commissioned, which is when homing matters most. The real
// `home` (2.2) will gate; this one is a bench tool.
bool cmdHome(const char* args) {
    const char* a = args;
    char* end;
    unsigned long v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = strtoul(a, &end, 10);
        if (end == a) { Serial.println("err usage"); return true; }
        a = end;
    }
    const uint8_t node = (uint8_t)v[0];
    if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err usage"); return true; }
    if (v[1] > 1 || v[2] > 0xFFFF || v[3] > 0xFFFF || v[4] > 0xFFFF) {
        Serial.println("err range"); return true;
    }

    NodeStatus st;
    // A node that NAKs (bad parameters) still simply does not answer -- it has
    // no NAK opcode yet (plan section 8.1) -- so RPC_TIMEOUT still covers both.
    // The string keeps saying so rather than claiming a certainty we lack.
    if (rpcHome(node, (uint8_t)(v[1] & 1), (uint16_t)v[2], (uint16_t)v[3],
                (uint16_t)v[4], (uint32_t)v[5], &st) != RPC_OK) {
        Serial.printf("node %d nak_or_timeout\n", node); return true;
    }
    Serial.printf("node %d armed limit %d homing %d pos %ld\n", node,
                  (st.flags & NODE_FLAG_LIMIT)  ? 1 : 0,
                  (st.flags & NODE_FLAG_HOMING) ? 1 : 0,
                  (long)st.pos);
    return true;
}

// ── step <node> <count> [sps] — debug stepping (bring-up only) ───────────────
// <node> is a BUS id resolved to its ENGAGE-bound stream slot via the axis map,
// so the node must be in a committed axis_map first. count is a full int32, its
// sign the direction, clamped to STEP_DEBUG_MAX. [sps] defaults to
// STEP_DEBUG_SPS and is clamped to STEP_DEBUG_SPS_MAX.
//
// The datum SURVIVES a debug burst. The node is engaged, so it counts these
// bytes into its own position exactly as during a job, and Core 1 adds the
// same steps to machinePos — both frames stay consistent. This used to clear
// axes_homed, which made sense only while position was slot-framed.
//
// Hence the enabled requirement: a node counts stream bytes whether or not its
// motor is energised, so stepping a de-energised axis would advance both
// counters while the shaft stayed put — the one case where the two agree and
// are both wrong. Refuse it rather than record a fiction.
bool cmdStep(const char* args) {
    if (busGateDenies()) return true;
    char* end;
    uint8_t node = (uint8_t)strtoul(args, &end, 10);
    long count = strtol(end, &end, 10);
    if (count == 0) { Serial.println("err usage"); return true; }
    if (labs(count) > STEP_DEBUG_MAX) { Serial.println("err too_many"); return true; }
    uint32_t sps = strtoul(end, &end, 10);   // optional — 0 if absent
    if (sps == 0) sps = STEP_DEBUG_SPS;
    if (sps > STEP_DEBUG_SPS_MAX) sps = STEP_DEBUG_SPS_MAX;
    uint8_t slot = nodeSlot(node);
    if (slot == SLOT_NONE) { Serial.println("err not_engaged"); return true; }
    if (!(axes_enabled & (1 << slot))) {
        Serial.println("err not_enabled"); return true;
    }
    // Both parameters ride the request so back-to-back `step`s cannot steal
    // each other's rate — see ipc/core1_rpc.h.
    rpcStepDebug(slot, (uint16_t)sps, (int32_t)count);
    Serial.printf("ok %ld steps %lu sps\n", count, (unsigned long)sps);
    return true;
}
