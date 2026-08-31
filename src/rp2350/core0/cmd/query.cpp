// query.cpp — commands that read and report. None of these change machine
// state; the ones that touch the position model only read it.
//
// vac_switch lives here despite the vac_ prefix: it reads a switch level, it
// does not actuate anything.

#include <Arduino.h>
#include "table.h"
#include "parse.h"
#include "gate.h"
#include "../position.h"
#include "../status.h"                  // getBufCount (status alias)
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"
#include "../../config/config_store.h"  // g_cfg (status cfg)

bool cmdPing(const char*) { Serial.println("pong"); return true; }

bool cmdGetState(const char*) {
    Serial.printf("state=%d enabled=0x%02x homed=0x%02x alarm=%d running=%d",
                  machineState, axes_enabled, axes_homed, alarmReason, runningReason);
#ifdef DEBUG_TIMING
    // texp/tmeas = expected vs measured duration (us) of the last completed
    // burst, from the intervals actually commanded vs wall-clock execution
    // on Core 1; twall = end-to-end wall time including any pause/wait
    // inside the burst. tmeas > texp means Core 1 fell behind schedule.
    Serial.printf(" texp=%lu tmeas=%lu twall=%lu",
                  (unsigned long)jobExpectedUs, (unsigned long)jobMeasuredUs,
                  (unsigned long)jobWallUs);
#endif
    Serial.println();
    return true;
}

// Position AND its validity, in one reply. The four counts are always plain
// numbers — never a sentinel. An in-band "invalid" value cannot survive this
// system: Core 1 dead-reckons with `machinePos[slot] += steps`, so a magic
// number would be silently incremented into an ordinary-looking coordinate.
// Validity has to travel out of band, hence the trailing mask.
//
// A cleared bit means the count is untrustworthy, NOT that it is zero — most
// invalidation paths (estop, soft limit, disable, debug step) deliberately
// retain the last known value because it is approximately right for that same
// axis. Only a rebind to an un-datumed node zeroes, because there the leftover
// number describes the slot's PREVIOUS occupant — a different physical motor.
// Callers must gate on the mask; the number alone never says it is stale.
bool cmdGetPos(const char*) {
    Serial.printf("pos %ld %ld %ld %ld homed=0x%02x\n",
                  (long)machinePos[0], (long)machinePos[1],
                  (long)machinePos[2], (long)machinePos[3], axes_homed);
    return true;
}

// `status` / `?` — human-readable, not host-facing. `status cfg` reports the
// committed config slot. One handler because they share a command word: the
// table matches words, so the sub-verb has to be dispatched here.
bool cmdStatus(const char* args) {
    if (strcmp(args, "cfg") == 0) {
        if (g_cfg.slot < 0) {
            Serial.println("cfg slot=none");
        } else {
            Serial.printf("cfg slot=%d seq=%lu len=%lu addr=0x%08lx\n",
                          g_cfg.slot,
                          (unsigned long)g_cfg.seq,
                          (unsigned long)g_cfg.length,
                          (unsigned long)(uintptr_t)g_cfg.addr);
        }
        return true;
    }
    Serial.printf("state=%s pos=%ld,%ld,%ld,%ld homed=0x%02x enabled=0x%02x buf=%u/%u\n",
                  stateName(machineState),
                  (long)machinePos[0], (long)machinePos[1],
                  (long)machinePos[2], (long)machinePos[3],
                  axes_homed, axes_enabled, getBufCount(), MASTER_BUF_SIZE);
    return true;
}

// ── pingnode [all|<id>] — relay an RS485 ping (IDLE/PAUSED/ALARM) ─────────────
// Bare / `all` scans the whole bus 1..BUS_ADDR_MAX (one reply line, bring-up
// convenience — surfaces peripherals, not just axes); `pingnode <id>` is the
// single-line form the host pre-flight uses.
bool cmdPingNode(const char* args) {
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state"); return true;
    }
    if (*args == '\0' || strcmp(args, "all") == 0) {
        // ONE line, not one per node. The text plane is strictly
        // request/response (D11) and the host reads exactly one line per
        // command, so a four-line reply left three orphans in its text sink
        // — which then answered the next three commands. A single CLI
        // `pingnode` desynced the control plane for the rest of the session.
        Serial.print("nodes");
        for (uint8_t n = 1; n <= BUS_ADDR_MAX; n++)
            Serial.printf(" %d=%s", n,
                          rpcNodeCmd(CMD_PING, n, 0) == RPC_OK ? "ok" : "timeout");
        Serial.println();
    } else {
        uint8_t node = parseNode(args, nullptr);
        if (!node) { Serial.println("err bad_node"); return true; }
        Serial.printf("node %d %s\n", node,
                      rpcNodeCmd(CMD_PING, node, 0) == RPC_OK ? "ok" : "timeout");
    }
    return true;
}

// A node's OWN step counter, read over RS485 — the independent check on
// `getpos`, which reports machinePos: what Core 1 believes it EMITTED. Only
// this can tell those apart. If the node never received the stream bytes
// (wrong baud, DE timing, streamEnabled unset) machinePos still advances by
// the full amount and reads perfectly correct, so `getpos` alone cannot
// detect lost steps. A divergence localises the loss to the bus or the node.
bool cmdNodePos(const char* args) {
    // Same gate as pingnode/enable/disable and the peripherals, for the reason
    // spelled out in cmd/periph.cpp: the exchange costs Core 1 up to
    // RESPONSE_TIMEOUT_MS inside its step budget. A read is not cheaper than an
    // actuation here -- what costs is the transaction, not the node's answer.
    //
    // The original reason was Core 0 blocking in pop_blocking, which put `stop`
    // behind a relay -- measured at 4 s of queued motion. That half is gone
    // (ipc/core1_rpc.h); the gate stays for Core 1's half.
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state"); return true;
    }
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err usage"); return true; }
    NodeStatus st;
    if (rpcNodeStatus(CMD_NODE_STATUS, node, 0, &st) != RPC_OK ||
        !st.hasStepperTail) {
        Serial.printf("node %d timeout\n", node);
        return true;
    }
    Serial.printf("node %d pos %ld\n", node, (long)st.pos);
    return true;
}

// ── nodestat <node> — any node's generic + type-specific state ───────────────
// One round-trip (CMD_NODE_STATUS). The payload is [type][flags][tail]; the tail
// is decoded by type.
bool cmdNodeStat(const char* args) {
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state"); return true;
    }
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err usage"); return true; }
    NodeStatus st;
    if (rpcNodeStatus(CMD_NODE_STATUS, node, 0, &st) != RPC_OK) {
        Serial.printf("node %d timeout\n", node); return true;
    }

    uint8_t type = st.type;
    // limit/homing are the whole homing diagnostic: with no supervisor yet,
    // this print IS how a bench run is observed. limit is "pin asserted OR
    // gate latched" and homing is "the node's pulser is running" — see
    // docs/homing.md 1.5 for how the pair reads after each kind of move.
    Serial.printf("node %d type %d en %d datum %d limit %d homing %d", node, type,
                  (st.flags & NODE_FLAG_ENABLED) ? 1 : 0,
                  (st.flags & NODE_FLAG_DATUM)   ? 1 : 0,
                  (st.flags & NODE_FLAG_LIMIT)   ? 1 : 0,
                  (st.flags & NODE_FLAG_HOMING)  ? 1 : 0);
    switch (type) {
        case NODE_TYPE_STEPPER: {
            if (st.slot == 0xFF) Serial.printf(" pos %ld slot none", (long)st.pos);
            else              Serial.printf(" pos %ld slot %d", (long)st.pos, st.slot);
            break;
        }
        case NODE_TYPE_VACUUM:
            Serial.printf(" servos 0x%02X ssr %d", st.tail[0], st.tail[1]);
            break;
        case NODE_TYPE_KNIFE_OSC:
            Serial.printf(" osc %d blower %d", st.tail[0], st.tail[1]);
            break;
        default:                         // unknown type — dump the raw tail
            Serial.print(" tail");
            for (uint8_t i = 0; i < st.tailLen; i++) Serial.printf(" %02X", st.tail[i]);
            break;
    }
    Serial.println();
    return true;
}

// ── vac_switch <node> — read the vacuum node's NC switch (PA3) ───────────────
// NC switch wired to GND w/ pull-up: level 0 = closed (rest), 1 = open.
bool cmdVacSwitch(const char* args) {
    // Gated like every other transaction that blocks Core 0 on the bus — see
    // cmdNodePos.
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state"); return true;
    }
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err usage"); return true; }
    uint8_t level;
    if (rpcSwitchGet(node, &level) != RPC_OK) {
        Serial.printf("node %d timeout\n", node);
        return true;
    }
    Serial.printf("node %d switch %s (level=%d)\n",
                  node, level ? "open" : "closed", level);
    return true;
}
