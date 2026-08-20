// Core 0 control plane: text command line handling (docs/wire_protocol.md).
// One command per line; replies with exactly one text line.

#include <Arduino.h>
#include "../shared.h"
#include "control_plane.h"
#include "data_plane.h"   // dataPlaneResetSeq (seqreset)
#include "status.h"       // getBufCount (status alias)
#include "../config/config_store.h"  // g_cfg (status cfg)

// ─── State predicates ─────────────────────────────────────────────────────────

static inline bool stateIs(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t s = machineState;
    return s == a || s == b || s == c;
}

// ALARM is a fault state, and the peripheral gates admit it so an operator can
// park a machine that faulted with the pump running or the blade hot. That
// direction is recovery; the other is not. Turning a peripheral ON in ALARM
// energises a hot blade or a pump on a machine whose datum is already lost and
// whose estop sweep has just parked the whole bus — there is no workflow that
// wants it, and an operator reaching for `knife_osc N on` to test something has
// misread the state.
//
// Off stays permitted in every state the gate allows, so this can never trap a
// running peripheral. Prints its own error; callers return on true.
static inline bool alarmDeniesOn(bool turningOn) {
    if (turningOn && machineState == STATE_ALARM) {
        Serial.println("err bad_state");
        return true;
    }
    return false;
}

// Relay a command to Core 1 (which owns the RS485 bus) and block for its result,
// so the control-plane reply is synchronous. Returns true if the node responded
// (PONG/ACK) within the timeout. Not used for GET_POS (Core 1 pushes an extra
// word for that — host getpos reads machinePos directly instead).
//
// `node` may be BUS_ADDR_BROADCAST, in which case nothing answers and the return
// value degrades to "the frame was sent" — never "a node acted on it". Callers
// must not treat a broadcast's true as evidence of node state.
static bool relayNode(uint8_t cmd, uint8_t node) {
    multicore_fifo_push_blocking(((uint32_t)cmd << 8) | node);
    uint32_t resp = multicore_fifo_pop_blocking();
    return (resp & 0xFFFF) != 0;
}

// ─── Axis map (Core-0-local; docs/engage_and_axis_map.md §5) ─────────────────
// slotNode[i] = the bus id currently ENGAGE-bound to stream slot i (X/Y/Z/A), or
// SLOT_NONE if that slot is unbound. Core 0 owns this map and the abstraction;
// Core 1 only ever sees granular per-node CMD_ENGAGE. Boots all-unbound → the
// machine sits in ALARM_CONFIG until axis_map commits a binding.
#define SLOT_NONE 0xFF
static uint8_t slotNode[4] = { SLOT_NONE, SLOT_NONE, SLOT_NONE, SLOT_NONE };

void axisMapReset() {
    for (int i = 0; i < 4; i++) slotNode[i] = SLOT_NONE;
}

// The stream byte has this many motion slots (X/Y/Z/A); axes_enabled/homed are
// one bit PER SLOT. Which bus id occupies each slot is the runtime axis map
// (slotNode[], §5), so "is this id an axis, and which slot" is a map lookup —
// no longer the id==slot+1 assumption. An axis node can now be any bus id.
#define MOTION_SLOTS 4
static uint8_t nodeSlot(uint8_t n) {
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) if (slotNode[i] == n) return i;
    return SLOT_NONE;
}
static inline bool node_isAxis(uint8_t n) { return nodeSlot(n) != SLOT_NONE; }

static uint8_t popStatusPayload(uint8_t* buf, uint8_t cap);   // defined below

// Relay one CMD_ENGAGE to Core 1 (slot in the payload byte, like vac_servo packs
// its idx). slot 0..3 binds, SLOT_NONE unbinds. The ack is a full status payload,
// so this both performs the bind and reports the resulting node state in one
// transaction; `st` receives it and *stLen its length (0 = node timed out).
// Returns true if the node answered.
static bool relayEngage(uint8_t node, uint8_t slot, uint8_t* st, uint8_t* stLen) {
    multicore_fifo_push_blocking(((uint32_t)slot << 16) |
                                 ((uint32_t)CMD_ENGAGE << 8) | node);
    *stLen = popStatusPayload(st, 32);
    return *stLen != 0;
}

// ─── Position datum, in the NODE frame (docs/node_session_and_datum.md §2) ────
// machinePos[] is indexed by SLOT, so it goes stale the moment axis_map rebinds
// a slot to a different node. The datum therefore lives with the NODE instead:
// nodeOrigin[id] is that node's own step counter at the instant it was datumed,
// and machinePos[slot] = <node counter now> - nodeOrigin[node]. A parked node
// can neither move nor count (its RX ISR returns on slot == SLOT_NONE), so the
// offset stays valid across an arbitrary number of swaps.
//
// axes_homed (per SLOT) is now DERIVED from nodeHomed (per BUS ID) every time a
// slot is bound — see the axis_map handler.
static int32_t  nodeOrigin[BUS_ADDR_MAX + 1] = {0};
static uint16_t nodeHomed = 0;               // bit n = nodeOrigin[n] is valid

// Frozen-while-parked check. parkPos[n] is node n's counter as reported by the
// ack of the CMD_ENGAGE that DISENGAGED it; parkSeen marks which entries are
// live. A parked node can neither move nor count, so when it is engaged again
// its counter must read exactly the same — any difference means a reboot or lost
// steps. Free: it rides acks we already pay for.
//
// These MUST outlive one axis_map invocation: a park lasts until some later
// command re-engages the node, which is the entire point. As locals they only
// ever checked nodes that stayed bound across a single command — i.e. the ones
// that were never really parked. Every axis_map disengages all bound nodes before
// engaging any, so an entry is always refreshed before it is used.
static int32_t  parkPos[BUS_ADDR_MAX + 1] = {0};
static uint16_t parkSeen = 0;

// ─── The one place a position reference dies ──────────────────────────────────
// Every path that destroys a position went through its own open-coded pair of
// bit clears, and the recurring bug was updating one frame and forgetting the
// other: clear axes_homed but leave nodeOrigin, and the next axis_map cheerfully
// resurrects the datum. Both frames die together, here, or the two disagree.
//
// NAMING: this is the ORIGIN — Core 0's stored reference for a node, the thing
// machinePos is measured from. It is NOT the node-side datum (NODE_FLAG_DATUM /
// CMD_DATUM_SET), which is the node's own continuity witness. Core 0 never writes
// that; only the node sets or clears it. Two different facts, deliberately
// separate, and validity is the conjunction of them (see slotAdoptStatus).
static void originInvalidate(uint8_t node) {
    nodeHomed &= ~(1u << node);
    parkSeen  &= ~(1u << node);        // its parked counter means nothing now
    uint8_t s = nodeSlot(node);
    if (s != SLOT_NONE) axes_homed &= ~(1 << s);
}

// Whole-machine version — estop, soft limit, disable-all.
static void originInvalidateAll() {
    nodeHomed  = 0;
    parkSeen   = 0;
    axes_homed = 0;
}

// ─── Node status payload ──────────────────────────────────────────────────────
// Every command that reports node state answers with the SAME bytes, produced by
// one serializer on the node (buildNodeStatus): [type][flags][type tail…], where
// the stepper tail is [pos int32 BE][slot]. CMD_NODE_STATUS, CMD_GET_POS and the
// CMD_ENGAGE ack all use it, so there is one parser here rather than one per
// command. Field offsets:
#define NS_TYPE        0
#define NS_FLAGS       1
#define NS_STEP_POS    2   // …5, int32 big-endian
#define NS_STEP_SLOT   6
#define NS_STEP_LEN    7   // full stepper payload length
// Flag bits are NODE_FLAG_* from common.h — shared with the node, not redefined.

// Drain a status payload Core 1 pushed (length word, then 4 bytes per word).
// Returns the payload length, 0 = timeout. Always drains what was pushed.
static uint8_t popStatusPayload(uint8_t* buf, uint8_t cap) {
    uint8_t plen = multicore_fifo_pop_blocking() & 0xFF;
    if (plen == 0) return 0;
    for (uint8_t i = 0; i < plen; i += 4) {
        uint32_t w = multicore_fifo_pop_blocking();
        for (uint8_t j = 0; j < 4 && (i + j) < plen; j++)
            if (i + j < cap) buf[i + j] = (w >> (24 - j * 8)) & 0xFF;
    }
    return plen;
}

// One CMD_NODE_STATUS round trip. 0 = node timed out.
static uint8_t nodeStatusRead(uint8_t node, uint8_t* buf, uint8_t cap) {
    multicore_fifo_push_blocking(((uint32_t)CMD_NODE_STATUS << 8) | node);
    return popStatusPayload(buf, cap);
}

static inline int32_t nsPos(const uint8_t* p) {
    return ((int32_t)p[NS_STEP_POS]     << 24) | ((int32_t)p[NS_STEP_POS + 1] << 16) |
           ((int32_t)p[NS_STEP_POS + 2] <<  8) |  (int32_t)p[NS_STEP_POS + 3];
}

// Position only, for callers that do not need the rest. false = timeout.
static bool readNodePos(uint8_t node, int32_t* out) {
    uint8_t buf[32];
    if (nodeStatusRead(node, buf, sizeof buf) < NS_STEP_LEN) return false;
    *out = nsPos(buf);
    return true;
}

// Rebuild slot `s`'s position and enabled bit from the status payload the node
// returned with its ENGAGE ack — no second transaction, and no window in which
// the node could have rebooted between binding and reporting.
//
// `st` is the ack payload (NS_STEP_LEN bytes for a stepper), or nullptr if the
// node did not answer. Both flags are taken from the NODE's own report rather
// than from what Core 0 last assumed it commanded — that is the point: the slot
// view becomes derived from node truth at every bind.
//
// Validity is a CONJUNCTION of two things neither side can know alone:
//   nodeHomed[n]     — Core 0: "I took a datum for this node"
//   NODE_FLAG_DATUM  — the node: "nothing since has interrupted it"
// The node's half covers events Core 0 never observes (brownout, watchdog reset,
// a de-energise it did not issue). Core 0's half covers the case of a node that
// has simply never been datumed in this machine's frame.
static void slotAdoptStatus(uint8_t s, uint8_t n, const uint8_t* st, uint8_t stLen) {
    bool haveTail = (st != nullptr && stLen >= NS_STEP_LEN);
    uint8_t flags = haveTail ? st[NS_FLAGS] : 0;

    if (flags & NODE_FLAG_ENABLED) axes_enabled |=  (1 << s);
    else                           axes_enabled &= ~(1 << s);

    // The node's continuity witness is broken (reset, or de-energised at some
    // point) — whatever origin we hold for it no longer refers to anything.
    if (!(flags & NODE_FLAG_DATUM)) originInvalidate(n);

    if (haveTail && (nodeHomed & (1u << n))) {
        machinePos[s] = nsPos(st) - nodeOrigin[n];
        axes_homed   |= (1 << s);
    } else {
        machinePos[s] = 0;
        axes_homed   &= ~(1 << s);
    }
}

// ─── Validity reconciliation — Core 0 is the sole writer ──────────────────────
// axes_homed / axes_enabled are bitmasks Core 0 read-modify-writes (|= and &=).
// Core 1 used to whole-byte-write them on estop and soft limit, which raced those
// RMWs: Core 0 reading a mask, Core 1 zeroing it, Core 0 writing back its stale
// value — an axis left claiming a datum the estop had just destroyed. There is no
// atomic here and no critical section; instead Core 1 only ever SIGNALS, by
// entering ALARM with a reason, and this folds the signal into the masks.
//
// Level-triggered rather than edge-triggered: it re-asserts every pass, so it is
// idempotent and cannot miss a transition. It also reaches nodeOrigin/nodeHomed,
// which are Core-0 statics Core 1 could never have cleared — without that, the
// next axis_map would happily resurrect a datum an estop had destroyed.
//
// Called from Core 0's loop before anything the host can observe. Both the text
// plane and STATUS_RSP are answered from that loop, so the documented invariant
// still holds: once ALARM is visible, the datum is already gone and the bus is
// already parked.
void reconcileValidity() {
    uint8_t st = machineState, ar = alarmReason;

    // Position dies the instant motion stops abruptly — before the bus sweep.
    if (st == STATE_ESTOP || ar == ALARM_ESTOP || ar == ALARM_SOFT_LIMIT)
        originInvalidateAll();
    // Energisation, however, is only false once Core 1's busDisableAll() has
    // actually run. Core 1 sets ALARM_ESTOP *before* the sweep and STATE_ALARM
    // *after* it, so the conjunction is precisely "the sweep has completed".
    // Keying on STATE_ESTOP instead would report the machine disarmed while
    // every EN pin was still asserted.
    if (st == STATE_ALARM && ar == ALARM_ESTOP) axes_enabled = 0;
}

// Map an axes string ("xyza", "xy", …) to a bitmask. Empty/absent → all axes.
static uint8_t axisMask(const char* s) {
    if (!s || !*s) return 0x0F;
    uint8_t m = 0;
    for (; *s; s++) {
        switch (*s) {
            case 'x': case 'X': m |= 0x01; break;
            case 'y': case 'Y': m |= 0x02; break;
            case 'z': case 'Z': m |= 0x04; break;
            case 'a': case 'A': m |= 0x08; break;
        }
    }
    return m ? m : 0x0F;
}

static const char* stateName(uint8_t s) {
    switch (s) {
        case STATE_IDLE:    return "IDLE";
        case STATE_RUNNING: return "RUNNING";
        case STATE_ESTOP:   return "ESTOP";
        case STATE_ALARM:   return "ALARM";
        case STATE_PAUSED:  return "PAUSED";
        case STATE_HOMING:  return "HOMING";
        default:            return "?";
    }
}

// Skip the command word and any spaces, returning a pointer to the first arg.
static const char* argAfter(const String& input, int wordLen) {
    const char* p = input.c_str() + wordLen;
    while (*p == ' ') p++;
    return p;
}

// Parse an on/off token: "1" or "on" → true; anything else ("0"/"off") → false.
static bool parseState(const char* s) {
    if (*s == '1') return true;
    if ((s[0] == 'o' || s[0] == 'O') && (s[1] == 'n' || s[1] == 'N')) return true;
    return false;
}

// BUS_ADDR_MAX lives in shared.h — Core 1's safe-off sweep walks the same range.

// Handle one control-plane text line. Replies with exactly one line per the wire
// contract (docs/wire_protocol.md): `ok` / `err <reason>` / a typed read.
bool handleCommand(const String& input) {

    // ── always available ──────────────────────────────────────────────────────
    if (input == "ping") { Serial.println("pong"); return true; }

    if (input == "getstate") {
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
    if (input == "getpos") {
        Serial.printf("pos %ld %ld %ld %ld homed=0x%02x\n",
                      (long)machinePos[0], (long)machinePos[1],
                      (long)machinePos[2], (long)machinePos[3], axes_homed);
        return true;
    }
    // A node's OWN step counter, read over RS485 — the independent check on
    // `getpos`, which reports machinePos: what Core 1 believes it EMITTED. Only
    // this can tell those apart. If the node never received the stream bytes
    // (wrong baud, DE timing, streamEnabled unset) machinePos still advances by
    // the full amount and reads perfectly correct, so `getpos` alone cannot
    // detect lost steps. A divergence localises the loss to the bus or the node.
    //
    // Core 1 already implements the exchange; this only surfaces it. Note the
    // GET_POS reply is TWO FIFO words (status, then position) where every other
    // relayed command pushes one — hence not going through relayNode().
    if (input.startsWith("nodepos")) {
        // Same gate as pingnode/enable/disable, and for the same reason: Core 1
        // only services the FIFO after draining the ring (processBus step 2
        // before step 3), so a relayed command issued mid-stream waits out the
        // whole queue. Core 0 blocks in pop_blocking meanwhile and stops reading
        // serial entirely — which would put `stop` behind it. Measured at 4 s of
        // queued motion before this gate existed.
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 7);
        uint8_t node = (uint8_t)strtoul(a, nullptr, 10);
        if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err usage"); return true; }
        int32_t pos;
        if (!readNodePos(node, &pos)) {
            Serial.printf("node %d timeout\n", node);
            return true;
        }
        Serial.printf("node %d pos %ld\n", node, (long)pos);
        return true;
    }
    // ── nodestat <node> — any node's generic + type-specific state ────────────
    // One round-trip (CMD_NODE_STATUS). Core 1 pushes a status word (payload len,
    // 0 = timeout) then the payload packed 4 bytes/word. Payload is
    // [type][flags][type-specific tail]; we decode the tail by type.
    // home <node> <dir> <start_us> <floor_us> <ramp_steps> <max_steps>
    //
    // Bench bring-up only. Deliberately raw and positional: no mm, no
    // steps/mm, no config lookup, no `invert`, and NO seek/retract argument.
    // Composing those belongs to the host (docs/homing.md 3), and a temporary
    // Pico-side version of them is exactly how they end up living here
    // permanently. The Pico relays; it does not plan.
    //
    // No state gate either, on purpose — this has to be usable from ALARM while
    // the machine is being commissioned, which is when homing matters most.
    // The real `home` (2.2) will gate; this one is a bench tool.
    if (input.startsWith("home")) {
        const char* a = argAfter(input, 4);
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

        multicore_fifo_push_blocking(((uint32_t)FIFO_HOME << 24) |
                                     ((uint32_t)(v[1] & 1) << 16) | node);
        multicore_fifo_push_blocking(((uint32_t)v[2] << 16) | (uint32_t)v[3]);
        multicore_fifo_push_blocking( (uint32_t)v[4] << 16);
        multicore_fifo_push_blocking( (uint32_t)v[5]);

        uint8_t buf[32] = {0};
        uint8_t plen = popStatusPayload(buf, sizeof buf);
        // A node that NAKs (bad parameters) simply does not answer, which on
        // this bus is indistinguishable from a node that is not there. Both mean
        // the same thing to the operator, though: nothing armed.
        if (plen == 0) { Serial.printf("node %d nak_or_timeout\n", node); return true; }
        Serial.printf("node %d armed limit %d homing %d pos %ld\n", node,
                      (buf[NS_FLAGS] & NODE_FLAG_LIMIT)  ? 1 : 0,
                      (buf[NS_FLAGS] & NODE_FLAG_HOMING) ? 1 : 0,
                      (long)nsPos(buf));
        return true;
    }

    if (input.startsWith("nodestat")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 8);
        uint8_t node = (uint8_t)strtoul(a, nullptr, 10);
        if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err usage"); return true; }
        uint8_t buf[32] = {0};             // max node reply payload
        uint8_t plen = nodeStatusRead(node, buf, sizeof buf);
        if (plen == 0) { Serial.printf("node %d timeout\n", node); return true; }

        uint8_t type = buf[NS_TYPE];
        // limit/homing are the whole homing diagnostic: with no supervisor yet,
        // this print IS how a bench run is observed. limit is "pin asserted OR
        // gate latched" and homing is "the node's pulser is running" — see
        // docs/homing.md 1.5 for how the pair reads after each kind of move.
        Serial.printf("node %d type %d en %d datum %d limit %d homing %d", node, type,
                      (buf[NS_FLAGS] & NODE_FLAG_ENABLED) ? 1 : 0,
                      (buf[NS_FLAGS] & NODE_FLAG_DATUM)   ? 1 : 0,
                      (buf[NS_FLAGS] & NODE_FLAG_LIMIT)   ? 1 : 0,
                      (buf[NS_FLAGS] & NODE_FLAG_HOMING)  ? 1 : 0);
        switch (type) {
            case NODE_TYPE_STEPPER: {
                uint8_t slot = buf[NS_STEP_SLOT];
                if (slot == 0xFF) Serial.printf(" pos %ld slot none", (long)nsPos(buf));
                else              Serial.printf(" pos %ld slot %d", (long)nsPos(buf), slot);
                break;
            }
            case NODE_TYPE_VACUUM:
                Serial.printf(" servos 0x%02X ssr %d", buf[2], buf[3]);
                break;
            case NODE_TYPE_KNIFE_OSC:
                Serial.printf(" osc %d blower %d", buf[2], buf[3]);
                break;
            default:                         // unknown type — dump the raw tail
                Serial.print(" tail");
                for (uint8_t i = 2; i < plen; i++) Serial.printf(" %02X", buf[i]);
                break;
        }
        Serial.println();
        return true;
    }
    if (input == "stop") {
        machineState = STATE_ESTOP;            // Core 1 flushes, clears axes, → ALARM
        Serial.println("ok");
        return true;
    }
    if (input == "reset" || input == "rst") {
        if (machineState != STATE_IDLE && machineState != STATE_ALARM) {
            Serial.println("err bad_state");
            return true;
        }
        soft_reset_requested = true;           // Exits loop(), triggers soft reset
        Serial.println("ok");
        return true;
    }
    if (input == "seqreset") {                 // data-plane support (see wire doc)
        dataPlaneResetSeq();
        Serial.println("seq reset");
        return true;
    }
    if (input == "status cfg") {
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
    if (input == "status" || input == "?") {   // human-readable alias (not host-facing)
        Serial.printf("state=%s pos=%ld,%ld,%ld,%ld homed=0x%02x enabled=0x%02x buf=%u/%u\n",
                      stateName(machineState),
                      (long)machinePos[0], (long)machinePos[1],
                      (long)machinePos[2], (long)machinePos[3],
                      axes_homed, axes_enabled, getBufCount(), MASTER_BUF_SIZE);
        return true;
    }

    // ── pingnode [all|<id>] — relay an RS485 ping (IDLE/PAUSED/ALARM) ──────────
    // Bare / `all` scans the whole bus 1..BUS_ADDR_MAX (one reply line, bring-up
    // convenience — surfaces peripherals, not just axes); `pingnode <id>` is the
    // single-line form the host pre-flight uses.
    if (input.startsWith("pingnode")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 8);
        if (*a == '\0' || strcmp(a, "all") == 0) {
            // ONE line, not one per node. The text plane is strictly
            // request/response (D11) and the host reads exactly one line per
            // command, so a four-line reply left three orphans in its text sink
            // — which then answered the next three commands. A single CLI
            // `pingnode` desynced the control plane for the rest of the session.
            Serial.print("nodes");
            for (uint8_t n = 1; n <= BUS_ADDR_MAX; n++)
                Serial.printf(" %d=%s", n, relayNode(CMD_PING, n) ? "ok" : "timeout");
            Serial.println();
        } else {
            uint8_t node = (uint8_t)strtoul(a, NULL, 10);
            if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err bad_node"); return true; }
            Serial.printf("node %d %s\n", node, relayNode(CMD_PING, node) ? "ok" : "timeout");
        }
        return true;
    }

    // ── axes_enable <on|off> (IDLE/PAUSED/ALARM) ──────────────────────────────
    // Targets the axis map: every node currently bound to a motion slot, and no
    // one else. This replaces the old `enable all` / `disable all`, whose name
    // read bus-wide while the code always walked slotNode[] — a distinction that
    // stopped being academic once vacuum and knife nodes joined the bus.
    // Peripherals hold no slot, so they are addressed only by `enable <id>`.
    if (input.startsWith("axes_enable")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 11);
        if (*a == '\0') { Serial.println("err usage"); return true; }
        // Deliberately NOT gated by alarmDeniesOn: ALARM is where axis recovery
        // happens. Boot sits in ALARM_CONFIG, and the post-estop flow is
        // axes_enable on → setorigin → unalarm. `enable <id>` is ungated for the
        // same reason. The peripheral commands gate because energising a pump
        // under alarm has no such recovery role.
        bool on = parseState(a);          // accepts "1"/"on" and "0"/"off"
        for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
            if (slotNode[i] == SLOT_NONE) continue;
            relayNode(on ? CMD_ENABLE : CMD_DISABLE, slotNode[i]);
            if (on) axes_enabled |=  (1 << i);
            else    axes_enabled &= ~(1 << i);
        }
        // De-energised → back-drivable → every bound origin is void. Keyed on the
        // node, not the slot, so a node that loses holding torque while PARKED
        // still loses its origin (see originInvalidate).
        if (!on)
            for (uint8_t i = 0; i < MOTION_SLOTS; i++)
                if (slotNode[i] != SLOT_NONE) originInvalidate(slotNode[i]);
        Serial.println("ok");
        return true;
    }

    // ── bus_enable <on|off> (IDLE/PAUSED/ALARM) ───────────────────────────────
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
    if (input.startsWith("bus_enable")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 10);
        if (*a == '\0') { Serial.println("err usage"); return true; }
        bool on = parseState(a);
        relayNode(on ? CMD_ENABLE : CMD_DISABLE, BUS_ADDR_BROADCAST);
        if (!on) {
            axes_enabled = 0;
            originInvalidateAll();
        }
        Serial.println("ok");
        return true;
    }

    // ── enable / disable <id> (IDLE/PAUSED/ALARM) ─────────────────────────────
    // Relays to any bus node — the generic CMD_ENABLE effect is delegated per type
    // (motor energize / pump on …); the axis bookkeeping applies only when the id
    // is an axis node (docs/engage_and_axis_map.md §9).
    if (input.startsWith("enable")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 6);
        uint8_t node = (uint8_t)strtoul(a, NULL, 10);
        if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err bad_node"); return true; }
        relayNode(CMD_ENABLE, node);
        uint8_t s = nodeSlot(node);      // axis bookkeeping keyed on the slot
        if (s != SLOT_NONE) axes_enabled |= (1 << s);
        Serial.println("ok");
        return true;
    }
    if (input.startsWith("disable")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 7);
        uint8_t node = (uint8_t)strtoul(a, NULL, 10);
        if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err bad_node"); return true; }
        relayNode(CMD_DISABLE, node);
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

    // ── vac_servo <node> <idx> <on|off> — vacuum-node servo channel ───────────
    // Relays CMD_SERVO_SET to a peripheral node. idx 0 = all servos, 1..6 = one.
    // The arg is packed into the FIFO word's payload byte (high nibble = idx, low
    // bit = on/off) for Core 1, which expands on→SERVO_ON_ANGLE before the wire.
    if (input.startsWith("vac_servo")) {
        // Gated: the relay blocks Core 0 on a Core 1 round trip (push + pop,
        // up to RESPONSE_TIMEOUT_MS), which Core 1 services between
        // microsegments — mid-stream it stretches a step interval and marks
        // the cut. Mid-job peripheral changes belong at a PAUSED boundary,
        // which is where the host orchestrator issues them.
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* p = argAfter(input, 9);
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(p,      &endPtr, 10);
        uint8_t idx  = (uint8_t)strtoul(endPtr, &endPtr, 10);
        while (*endPtr == ' ') endPtr++;
        if (node < 1 || node > BUS_ADDR_MAX || idx > 6 || *endPtr == '\0') {
            Serial.println("err usage"); return true;
        }
        bool on = parseState(endPtr);
        if (alarmDeniesOn(on)) return true;
        uint8_t payload = (uint8_t)((idx << 4) | (on ? 1u : 0u));
        multicore_fifo_push_blocking(((uint32_t)payload << 16) |
                                     ((uint32_t)CMD_SERVO_SET << 8) | node);
        bool ok = (multicore_fifo_pop_blocking() & 0xFFFF) != 0;
        Serial.printf("node %d %s\n", node, ok ? "ok" : "timeout");
        return true;
    }

    // ── vac_pump <node> <on|off> — vacuum-node SSR pump (soft-started) ─────────
    if (input.startsWith("vac_pump")) {
        // Gated: the relay blocks Core 0 on a Core 1 round trip (push + pop,
        // up to RESPONSE_TIMEOUT_MS), which Core 1 services between
        // microsegments — mid-stream it stretches a step interval and marks
        // the cut. Mid-job peripheral changes belong at a PAUSED boundary,
        // which is where the host orchestrator issues them.
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* p = argAfter(input, 8);
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(p, &endPtr, 10);
        while (*endPtr == ' ') endPtr++;
        if (node < 1 || node > BUS_ADDR_MAX || *endPtr == '\0') {
            Serial.println("err usage"); return true;
        }
        uint8_t state = parseState(endPtr) ? 1u : 0u;
        if (alarmDeniesOn(state != 0)) return true;
        multicore_fifo_push_blocking(((uint32_t)state << 16) |
                                     ((uint32_t)CMD_SSR_SET << 8) | node);
        bool ok = (multicore_fifo_pop_blocking() & 0xFFFF) != 0;
        Serial.printf("node %d %s\n", node, ok ? "ok" : "timeout");
        return true;
    }

    // ── knife_osc <node> <on|off> — oscillating-knife oscillator toggle ───────
    // Relays CMD_KNIFE_OSC to a knife node. State packed into the FIFO word's
    // payload byte (low bit) for Core 1.
    if (input.startsWith("knife_osc")) {
        // Gated: the relay blocks Core 0 on a Core 1 round trip (push + pop,
        // up to RESPONSE_TIMEOUT_MS), which Core 1 services between
        // microsegments — mid-stream it stretches a step interval and marks
        // the cut. Mid-job peripheral changes belong at a PAUSED boundary,
        // which is where the host orchestrator issues them.
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* p = argAfter(input, 9);
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(p, &endPtr, 10);
        while (*endPtr == ' ') endPtr++;
        if (node < 1 || node > BUS_ADDR_MAX || *endPtr == '\0') {
            Serial.println("err usage"); return true;
        }
        uint8_t state = parseState(endPtr) ? 1u : 0u;
        if (alarmDeniesOn(state != 0)) return true;
        multicore_fifo_push_blocking(((uint32_t)state << 16) |
                                     ((uint32_t)CMD_KNIFE_OSC << 8) | node);
        bool ok = (multicore_fifo_pop_blocking() & 0xFFFF) != 0;
        Serial.printf("node %d %s\n", node, ok ? "ok" : "timeout");
        return true;
    }

    // ── laser <node> <on|off> — stepper-node laser gate ──────────────────────
    // Relays CMD_LASER to a stepper node (only the one built -DNODE_HAS_LASER
    // handles it; others NAK → "timeout"). State in the FIFO payload byte.
    if (input.startsWith("laser")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* p = argAfter(input, 5);
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(p, &endPtr, 10);
        while (*endPtr == ' ') endPtr++;
        if (node < 1 || node > BUS_ADDR_MAX || *endPtr == '\0') {
            Serial.println("err usage"); return true;
        }
        uint8_t state = parseState(endPtr) ? 1u : 0u;
        if (alarmDeniesOn(state != 0)) return true;
        multicore_fifo_push_blocking(((uint32_t)state << 16) |
                                     ((uint32_t)CMD_LASER << 8) | node);
        bool ok = (multicore_fifo_pop_blocking() & 0xFFFF) != 0;
        Serial.printf("node %d %s\n", node, ok ? "ok" : "timeout");
        return true;
    }

    // ── knife_blower <node> <0..100> — oscillating-knife blower PWM duty ───────
    // Relays CMD_KNIFE_BLOWER to a knife node. Duty (0..100 %) packed into the
    // FIFO word's payload byte for Core 1.
    if (input.startsWith("knife_blower")) {
        // Gated: the relay blocks Core 0 on a Core 1 round trip (push + pop,
        // up to RESPONSE_TIMEOUT_MS), which Core 1 services between
        // microsegments — mid-stream it stretches a step interval and marks
        // the cut. Mid-job peripheral changes belong at a PAUSED boundary,
        // which is where the host orchestrator issues them.
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* p = argAfter(input, 12);
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(p,      &endPtr, 10);
        long    duty = strtol(endPtr, &endPtr, 10);
        if (node < 1 || node > BUS_ADDR_MAX || duty < 0 || duty > 100) {
            Serial.println("err usage"); return true;
        }
        if (alarmDeniesOn(duty > 0)) return true;
        multicore_fifo_push_blocking(((uint32_t)(uint8_t)duty << 16) |
                                     ((uint32_t)CMD_KNIFE_BLOWER << 8) | node);
        bool ok = (multicore_fifo_pop_blocking() & 0xFFFF) != 0;
        Serial.printf("node %d %s\n", node, ok ? "ok" : "timeout");
        return true;
    }

    // ── vac_switch <node> — read the vacuum node's NC switch (PA3) ────────────
    // Query: pushes CMD_SWITCH_GET; Core 1 returns two words (status, level) like
    // nodepos. NC switch wired to GND w/ pull-up: level 0 = closed (rest),
    // level 1 = open (actuated).
    if (input.startsWith("vac_switch")) {
        // Gated: the relay blocks Core 0 on a Core 1 round trip (push + pop,
        // up to RESPONSE_TIMEOUT_MS), which Core 1 services between
        // microsegments — mid-stream it stretches a step interval and marks
        // the cut. Mid-job peripheral changes belong at a PAUSED boundary,
        // which is where the host orchestrator issues them.
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 10);
        uint8_t node = (uint8_t)strtoul(a, nullptr, 10);
        if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err usage"); return true; }
        multicore_fifo_push_blocking(((uint32_t)CMD_SWITCH_GET << 8) | node);
        if ((multicore_fifo_pop_blocking() & 0xFFFF) == 0) {
            Serial.printf("node %d timeout\n", node);
            return true;
        }
        uint8_t level = (uint8_t)multicore_fifo_pop_blocking();
        Serial.printf("node %d switch %s (level=%d)\n",
                      node, level ? "open" : "closed", level);
        return true;
    }

    // ── axis_map [<x> <y> <z> <a>] — bind bus nodes to stream slots ───────────
    // No-arg: read back the committed map in setter syntax ('-' = unbound slot).
    // Four tokens (a bus id, or '-'/'0' = unbound): diff against the committed map,
    // emit the minimal engage/disengage packets, and commit each slot only once
    // its packets ACK. A successful commit clears the ALARM_CONFIG boot gate.
    // Valid IDLE/PAUSED/ALARM; rebinding mid-RUNNING corrupts motion (§6.2).
    if (input.startsWith("axis_map")) {
        const char* a = argAfter(input, 8);

        if (*a == '\0') {                          // read-back form
            Serial.print("axis_map");
            for (int i = 0; i < 4; i++) {
                if (slotNode[i] == SLOT_NONE) Serial.print(" -");
                else                          Serial.printf(" %d", slotNode[i]);
            }
            Serial.println();
            return true;
        }

        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }

        // Parse exactly four tokens into desired[]: a bus id, or '-'/'0' = unbound.
        uint8_t desired[4];
        const char* p = a;
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
        // Park every bound node, recording the counter each reports (see parkPos).
        for (int i = 0; i < 4; i++) {
            uint8_t n = slotNode[i];
            if (n == SLOT_NONE) continue;
            uint8_t st[32], stLen;
            if (relayEngage(n, SLOT_NONE, st, &stLen) && stLen >= NS_STEP_LEN) {
                parkPos[n] = nsPos(st);
                parkSeen  |= (1u << n);
            } else {
                // No answer — we do not know where it stopped. Drop any earlier
                // entry rather than let a stale one produce a false match later.
                parkSeen &= ~(1u << n);
            }
        }

        // Engage, and adopt each slot's state straight out of the ack — position
        // and enabled bit in the same transaction as the bind. This is the whole
        // point of the node-frame datum: a head parked through several rebinds
        // comes back with its position intact, and a slot that changed hands never
        // inherits the previous occupant's count.
        for (int i = 0; i < 4; i++) {
            if (desired[i] == SLOT_NONE) continue;
            uint8_t st[32], stLen;
            if (!relayEngage(desired[i], (uint8_t)i, st, &stLen)) {
                Serial.printf("err node %d timeout\n", desired[i]);
                return true;              // leave the map as-is; a retry redoes all
            }
            // Frozen-while-parked check (see parkPos above). Silent by design: the
            // wire contract is exactly one line per command, so this cannot print.
            // Clearing the node's origin is the report — the axis comes back
            // un-homed, which getpos's mask and getstate both surface.
            if ((parkSeen & (1u << desired[i])) && stLen >= NS_STEP_LEN &&
                nsPos(st) != parkPos[desired[i]])
                originInvalidate(desired[i]);       // moved while parked
            slotNode[i] = desired[i];
            slotAdoptStatus((uint8_t)i, desired[i], st, stLen);
        }
        // Slots left unbound hold no node, so they hold no position either.
        for (int i = 0; i < 4; i++) {
            if (desired[i] != SLOT_NONE) continue;
            slotNode[i]   = SLOT_NONE;
            machinePos[i] = 0;
            axes_homed   &= ~(1 << i);
            axes_enabled &= ~(1 << i);
        }

        // Committed — clear the config gate if that is what was holding us.
        if (machineState == STATE_ALARM && alarmReason == ALARM_CONFIG) {
            machineState = STATE_IDLE;
            alarmReason  = ALARM_NONE;
        }
        Serial.println("ok");
        return true;
    }

    // ── setorigin [axes] (IDLE/PAUSED/ALARM) ──────────────────────────────────
    if (input.startsWith("setorigin")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        uint8_t m = axisMask(argAfter(input, 9));
        // setorigin does bus I/O below — up to four round trips, so it can be in
        // flight for tens of milliseconds. An estop landing inside that window
        // would otherwise be ERASED by the alarm-clearing block at the end, which
        // cannot tell "the fault I was invoked to recover from" apart from "a
        // fault that arrived while I was working". Snapshot the reason on entry
        // and only clear what we came in with.
        uint8_t alarmAtEntry = alarmReason;
        // The datum is recorded in the NODE's frame: nodeOrigin[id] captures that
        // node's own counter here, so machinePos is a derived offset from now on
        // and survives any later rebinding. A masked slot with no node bound
        // cannot be datumed — there is nothing to record against — so it is
        // skipped and left un-homed rather than silently claiming an origin.
        for (int i = 0; i < 4; i++) {
            if (!(m & (1 << i))) continue;
            uint8_t n = slotNode[i];
            if (n == SLOT_NONE) { axes_homed &= ~(1 << i); continue; }

            // CMD_DATUM_SET arms the node's continuity witness AND returns the
            // counter it refers to. One transaction, so the origin recorded here
            // and the witness armed there describe the same instant — a separate
            // read could straddle a reset and pair a witness with a stale count.
            uint8_t st[32], stLen;
            multicore_fifo_push_blocking(((uint32_t)CMD_DATUM_SET << 8) | n);
            stLen = popStatusPayload(st, sizeof st);
            if (stLen < NS_STEP_LEN || !(st[NS_FLAGS] & NODE_FLAG_DATUM)) {
                originInvalidate(n);           // no answer, or witness not armed
                continue;
            }
            nodeOrigin[n]  = nsPos(st);
            nodeHomed     |= (1u << n);
            machinePos[i]  = 0;
            axes_homed    |= (1 << i);
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

    // ── pause / resume / cancel (job lifecycle) ───────────────────────────────
    if (input == "pause") {
        if (machineState != STATE_RUNNING) { Serial.println("err bad_state"); return true; }
        // Core 1 ramps the current segment to rest, flushes the ring, snapshots
        // resumePos and enters PAUSED (§4.5). It no longer finishes the segment
        // and drains — resume re-plans from position, so stopping early is safe.
        // Gated on RUNNING, so unlike abort this flag can never strand.
        pauseRequested = true;
        Serial.println("ok");
        return true;
    }
    if (input == "resume") {
        if (machineState != STATE_PAUSED) { Serial.println("err bad_state"); return true; }
        // Phase 1: the host has already pre-positioned the head, so resume simply
        // leaves PAUSED. The next operation streams in fresh (IDLE accepts MSEG).
        jobActive    = false;
        machineState = STATE_IDLE;
        Serial.println("ok");
        return true;
    }
    if (input == "cancel") {
        if (machineState != STATE_PAUSED) { Serial.println("err bad_state"); return true; }
        mBufHead = mBufTail;                   // buffer already drained at pause; defensive
        queuedUsOut = queuedUsIn;              // …and its queued time with it (§4.6)
        jobActive    = false;
        machineState = STATE_IDLE;
        Serial.println("ok");
        return true;
    }
    if (input == "unalarm") {
        if (machineState != STATE_ALARM) { Serial.println("err bad_state"); return true; }
        // The config gate is not a clearable fault — only a committed axis_map
        // leaves it (docs/engage_and_axis_map.md §6.1).
        if (alarmReason == ALARM_CONFIG) { Serial.println("err unconfigured"); return true; }
        machineState = STATE_IDLE;
        alarmReason  = ALARM_NONE;
        Serial.println("ok");                  // position still invalid — run setorigin
        return true;
    }

    // ── step <node> <count> [sps] — debug stepping (bring-up only) ─────────────
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
    if (input.startsWith("step")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* p = argAfter(input, 4);
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(p, &endPtr, 10);
        long count = strtol(endPtr, &endPtr, 10);
        if (count == 0) { Serial.println("err usage"); return true; }
        if (labs(count) > STEP_DEBUG_MAX) { Serial.println("err too_many"); return true; }
        uint32_t sps = strtoul(endPtr, &endPtr, 10);   // optional — 0 if absent
        if (sps == 0) sps = STEP_DEBUG_SPS;
        if (sps > STEP_DEBUG_SPS_MAX) sps = STEP_DEBUG_SPS_MAX;
        uint8_t slot = nodeSlot(node);
        if (slot == SLOT_NONE) { Serial.println("err not_engaged"); return true; }
        if (!(axes_enabled & (1 << slot))) {
            Serial.println("err not_enabled"); return true;
        }
        // Two words: tag|slot|sps, then the plain int32 count (sign = direction).
        // Both parameters ride the request so back-to-back `step`s cannot steal
        // each other's rate — see the FIFO encoding note in shared.h.
        multicore_fifo_push_blocking(((uint32_t)FIFO_STEP_DEBUG << 24) |
                                     ((uint32_t)slot << 16) | (sps & 0xFFFF));
        multicore_fifo_push_blocking((uint32_t)(int32_t)count);
        Serial.printf("ok %ld steps %lu sps\n", count, (unsigned long)sps);
        return true;
    }

    return false;
}
