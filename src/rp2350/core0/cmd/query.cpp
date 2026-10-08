// query.cpp — commands that read and report. None of these change machine
// state; the ones that touch the position model only read it.
//
// vac_switch lives here despite the vac_ prefix: it reads a switch level, it
// does not actuate anything.

#include <Arduino.h>
#include "table.h"
#include "parse.h"
#include "gate.h"
#include "../ops/position.h"
#include "../ops/frames.h"
#include "../ops/mesh.h"
#include "../ops/homing.h"
#include "../ops/bus.h"
#include "../status.h"                  // getBufCount (status alias)
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"
#include "../config/config_store.h"  // g_cfg (cfg)
#include "../config/machine_cfg.h"

bool cmdPing(const char*) { Serial.println("pong"); return true; }

// `cfg` — the committed config blob, whether it decoded, and the most recent
// rejection. Answers with or without a config, so it is a primitive.
bool cmdCfg(const char*) {
    if (!g_cfg.mounted) {
        Serial.print("cfg fs=unmounted");
    } else if (!g_cfg.valid) {
        Serial.print("cfg none");
    } else {
        Serial.printf("cfg seq=%lu len=%lu crc=0x%08lx",
                      (unsigned long)g_cfg.seq,
                      (unsigned long)g_cfg.length,
                      (unsigned long)g_cfg.crc32);
        if (machineCfgValid()) Serial.printf(" schema=%u", machineCfg().version);
        else                   Serial.print(" decoded=0");
    }
    if (machineCfgError() != CFG_DEC_OK)
        Serial.printf(" rejected=%s", configDecodeErrorName(machineCfgError()));
    if (machineCfgIgnored()) Serial.print(" ignored=1");
    Serial.println();
    return true;
}

// `status` / `?` — human-readable, not host-facing.
bool cmdStatus(const char*) {
    // mute / excluded / touched: bit n = bus id n (ops/bus.h).
    // buf= counts planner blocks, of planner::Planner::kSize, while planner motion runs or is held.
    Serial.printf("state=%s pos=%ld,%ld,%ld,%ld homed=0x%02x enabled=0x%02x buf=%u/%u"
                  " mute=0x%03x excluded=0x%03x touched=0x%03x",
                  stateName(machineState),
                  (long)machinePos[0], (long)machinePos[1],
                  (long)machinePos[2], (long)machinePos[3],
                  axes_homed, axes_enabled, getBufCount(),
                  plannerActive ? planner::Planner::kSize : MASTER_BUF_SIZE,
                  busMute(), busExcluded(), busTouched());
    // Frames, units; `-` where a slot has no value.
    const uint8_t sel = framesSelected();
    if (sel == FRAMES_NONE)        Serial.print(" head=-");
    else if (sel == FRAMES_ANCHOR) Serial.print(" head=anchor");
    else                           Serial.printf(" head=%u", sel);
    for (int w = 0; w < 2; w++) {
        Serial.print(w ? " wpos=" : " mpos=");
        for (uint8_t k = 0; k < 4; k++) {
            float v;
            const bool ok = w ? framesWPos(k, &v) : framesMPos(k, &v);
            if (k) Serial.print(',');
            if (ok) Serial.printf("%.3f", v);
            else    Serial.print('-');
        }
    }
    if (meshFile() == MESH_FILE_ABSENT)   Serial.print(" mesh=flat");
    else if (meshFile() == MESH_FILE_BAD) Serial.print(" mesh=bad");
    else if (!meshEnabled())              Serial.print(" mesh=off");
    else                                  Serial.printf(" mesh=%ux%u", meshNx(), meshNy());
    Serial.println();
    return true;
}

// The bus queries' gate: IDLE/PAUSED/ALARM, and a homing session between legs,
// when no leg holds the bus.
static bool busQueryDenies() {
    if (stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM) || homingWaiting()) return false;
    Serial.println("err bad_state");
    return true;
}

// ── pingnode [all|<id>] — relay an RS485 ping (IDLE/PAUSED/ALARM, HOMING_WAIT) ─
// Bare / `all` scans the whole bus 1..BUS_ADDR_MAX (one reply line, bring-up
// convenience — surfaces peripherals, not just axes); `pingnode <id>` is the
// single-line form the host pre-flight uses.
bool cmdPingNode(const char* args) {
    if (busQueryDenies()) return true;
    if (*args == '\0' || strcmp(args, "all") == 0) {
        // ONE line, not one per node. The text plane is strictly
        // request/response (D11) and the host reads exactly one line per
        // command, so a four-line reply left three orphans in its text sink
        // — which then answered the next three commands. A single CLI
        // `pingnode` desynced the control plane for the rest of the session.
        Serial.print("nodes");
        for (uint8_t n = 1; n <= BUS_ADDR_MAX; n++)
            Serial.printf(" %d=%s", n,
                          rpcResultText(rpcNodeCmd(CMD_PING, n, 0)));
        Serial.println();
    } else {
        uint8_t node = parseNode(args, nullptr);
        if (!node) { Serial.println("err bad_node"); return true; }
        Serial.printf("node %d %s\n", node,
                      rpcResultText(rpcNodeCmd(CMD_PING, node, 0)));
    }
    return true;
}

// A node's OWN step counter, read over RS485 — the independent check on
// `get pos`, which reports machinePos: what Core 1 believes it EMITTED. Only
// this can tell those apart. If the node never received the stream bytes
// (wrong baud, DE timing, streamEnabled unset) machinePos still advances by
// the full amount and reads perfectly correct, so `get pos` alone cannot
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
    RpcResult r = rpcNodeStatus(CMD_NODE_STATUS, node, 0, &st);
    if (r != RPC_OK) { Serial.printf("node %d %s\n", node, rpcResultText(r)); return true; }
    // Answered, but not with a stepper tail — a peripheral node has no position.
    // Distinct from the transport results above: the node is present and willing.
    if (!st.hasStepperTail) { Serial.printf("node %d bad_reply\n", node); return true; }
    Serial.printf("node %d pos %ld\n", node, (long)st.pos);
    return true;
}

// ── busstat <node> — a node's receive-error counters ─────────────────────────
// Raw, wrapping 16-bit counts since the node powered on; the reader diffs them.
bool cmdBusStat(const char* args) {
    if (busQueryDenies()) return true;
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err usage"); return true; }
    BusStats bs;
    RpcResult r = rpcBusStats(node, &bs);
    if (r != RPC_OK) {
        Serial.printf("node %d %s\n", node, rpcResultText(r)); return true;
    }
    Serial.printf("node %d ferr %u ovf %u crc %u\n", node,
                  (unsigned)bs.ferr, (unsigned)bs.ovf, (unsigned)bs.crc);
    return true;
}

// ── nodestat <node> — any node's generic + type-specific state ───────────────
// One round-trip (CMD_NODE_STATUS). The payload is [type][flags][tail]; the tail
// is decoded by type.
bool cmdNodeStat(const char* args) {
    if (busQueryDenies()) return true;
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err usage"); return true; }
    NodeStatus st;
    RpcResult r = rpcNodeStatus(CMD_NODE_STATUS, node, 0, &st);
    if (r != RPC_OK) {
        Serial.printf("node %d %s\n", node, rpcResultText(r)); return true;
    }

    uint8_t type = st.type;
    // limit/homing are the whole homing diagnostic: with no supervisor yet,
    // this print IS how a bench run is observed. limit is "pin asserted OR
    // gate latched" and homing is "the node's pulser is running" — see
    // docs/homing.md 1.5 for how the pair reads after each kind of move.
    Serial.printf("node %d type %d en %d datum %d", node, type,
                  (st.flags & NODE_FLAG_ENABLED) ? 1 : 0,
                  (st.flags & NODE_FLAG_DATUM)   ? 1 : 0);
    // `limit` is printed only where it can ever be non-zero. The BIT stays
    // reserved bus-wide (common.h) so the flags byte has one meaning for every
    // board -- but printing "limit 0" for a node with no switch states a fact
    // about a thing that does not exist, and on a rotary node that reads as a
    // switch that is fine rather than a switch that is absent.
    if (st.homingKind == HOMING_KIND_LIMIT)
        Serial.printf(" limit %d", (st.flags & NODE_FLAG_LIMIT) ? 1 : 0);
    Serial.printf(" homing %d", (st.flags & NODE_FLAG_LEG) ? 1 : 0);
    switch (type) {
        case NODE_TYPE_STEPPER: {
            if (st.slot == 0xFF) Serial.printf(" pos %ld slot none", (long)st.pos);
            else              Serial.printf(" pos %ld slot %d", (long)st.pos, st.slot);
            // How far this node's last homing leg ran, straight from the node
            // (stepper.cpp's "Leg span"). Absent on a board with no switch,
            // which has no homing and so nothing to measure -- printed only
            // when the node actually sent it, never defaulted to 0.
            if (st.hasHomeSpan) Serial.printf(" span %ld", (long)st.homeSpan);
            // Rotary only. The cause always prints when the node has an index,
            // including "none" before the first sweep -- the alternative is a
            // silent absence that reads identically to a linear node, which is
            // exactly the distinction this line exists to show.
            if (st.hasIndex) {
                Serial.printf(" idxcause %s", rotaryIdxCauseText(st.indexCause));
                if (st.indexCause == ROTARY_IDX_OK)
                    Serial.printf(" index %ld", (long)st.indexPos);
                Serial.printf(" hall %d base %d", st.hallRaw, st.hallBaseline);
            }
            // Always printed when the node sends it, INCLUDING on a failure --
            // `cross 0` against `idxcause notfound` is the whole diagnosis (the
            // magnet was never seen), and a short count says the budget ran out
            // before the sweep could prove the feature repeats.
            if (st.hasLap) {
                Serial.printf(" cross %u", st.crossings);
                if (st.stepsPerRev) Serial.printf(" steprev %ld", (long)st.stepsPerRev);
            }
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
    RpcResult r = rpcSwitchGet(node, &level);
    if (r != RPC_OK) {
        Serial.printf("node %d %s\n", node, rpcResultText(r));
        return true;
    }
    Serial.printf("node %d switch %s (level=%d)\n",
                  node, level ? "open" : "closed", level);
    return true;
}
