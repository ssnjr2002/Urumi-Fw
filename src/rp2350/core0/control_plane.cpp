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

// Relay a single-node command to Core 1 (which owns the RS485 bus) and block for
// its result, so the control-plane reply is synchronous. Returns true if the node
// responded (PONG/ACK) within the timeout. Not used for GET_POS (Core 1 pushes an
// extra word for that — host getpos reads machinePos directly instead).
static bool relayNode(uint8_t cmd, uint8_t node) {
    multicore_fifo_push_blocking(((uint32_t)cmd << 8) | node);
    uint32_t resp = multicore_fifo_pop_blocking();
    return (resp & 0xFFFF) != 0;
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

// Provisional bus-address ceiling for command relays. A real node registry
// replaces this range check when the axis-map/ENGAGE work lands
// (docs/engage_and_axis_map.md §9); until then a wrong id simply relays and
// times out.
#define BUS_ADDR_MAX 8

// Axis nodes are the ones that occupy a stream slot and carry the
// axes_enabled/homed bookkeeping. Until axis_map/ENGAGE makes the axis→node
// binding runtime, the axis nodes are statically ids 1..AXIS_NODE_MAX, matching
// the Core 1 stream packer (axis i → node i+1). node_isAxis() is the current
// stand-in for "is this id in the axis map"; it becomes a real map lookup then.
#define AXIS_NODE_MAX 4
static inline bool node_isAxis(uint8_t n) { return n >= 1 && n <= AXIS_NODE_MAX; }

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
    if (input == "getpos") {
        Serial.printf("pos %ld %ld %ld %ld\n",
                      (long)machinePos[0], (long)machinePos[1],
                      (long)machinePos[2], (long)machinePos[3]);
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
        multicore_fifo_push_blocking(((uint32_t)CMD_GET_POS << 8) | node);
        if ((multicore_fifo_pop_blocking() & 0xFFFF) == 0) {
            Serial.printf("node %d timeout\n", node);
            return true;
        }
        Serial.printf("node %d pos %ld\n", node,
                      (long)(int32_t)multicore_fifo_pop_blocking());
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

    // ── enable / disable [all|<id>] (IDLE/PAUSED/ALARM) ───────────────────────
    // `all` targets the axes only (energizing a peripheral pump via "all" is not
    // wanted). An explicit <id> relays to any bus node — the generic CMD_ENABLE
    // effect is delegated per type (motor energize / pump on …); the axis
    // bookkeeping applies only when the id is an axis node (docs/engage_and_axis_map.md §9).
    if (input.startsWith("enable")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 6);
        if (*a == '\0' || strcmp(a, "all") == 0) {
            for (uint8_t n = 1; n <= AXIS_NODE_MAX; n++) relayNode(CMD_ENABLE, n);
            axes_enabled = (1 << AXIS_NODE_MAX) - 1;
        } else {
            uint8_t node = (uint8_t)strtoul(a, NULL, 10);
            if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err bad_node"); return true; }
            relayNode(CMD_ENABLE, node);
            if (node_isAxis(node)) axes_enabled |= (1 << (node - 1));
        }
        Serial.println("ok");
        return true;
    }
    if (input.startsWith("disable")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 7);
        if (*a == '\0' || strcmp(a, "all") == 0) {
            for (uint8_t n = 1; n <= AXIS_NODE_MAX; n++) relayNode(CMD_DISABLE, n);
            axes_enabled = 0;
            axes_homed   = 0;          // de-energised → datum lost on every axis
        } else {
            uint8_t node = (uint8_t)strtoul(a, NULL, 10);
            if (node < 1 || node > BUS_ADDR_MAX) { Serial.println("err bad_node"); return true; }
            relayNode(CMD_DISABLE, node);
            if (node_isAxis(node)) {
                axes_enabled &= ~(1 << (node - 1));
                axes_homed   &= ~(1 << (node - 1));   // de-energised → datum lost
            }
        }
        Serial.println("ok");
        return true;
    }

    // ── vac_servo <node> <idx> <on|off> — vacuum-node servo channel ───────────
    // Relays CMD_SERVO_SET to a peripheral node. idx 0 = all servos, 1..6 = one.
    // The arg is packed into the FIFO word's payload byte (high nibble = idx, low
    // bit = on/off) for Core 1, which expands on→SERVO_ON_ANGLE before the wire.
    if (input.startsWith("vac_servo")) {
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
        uint8_t payload = (uint8_t)((idx << 4) | (parseState(endPtr) ? 1u : 0u));
        multicore_fifo_push_blocking(((uint32_t)payload << 16) |
                                     ((uint32_t)CMD_SERVO_SET << 8) | node);
        bool ok = (multicore_fifo_pop_blocking() & 0xFFFF) != 0;
        Serial.printf("node %d %s\n", node, ok ? "ok" : "timeout");
        return true;
    }

    // ── vac_pump <node> <on|off> — vacuum-node SSR pump (soft-started) ─────────
    if (input.startsWith("vac_pump")) {
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
        multicore_fifo_push_blocking(((uint32_t)state << 16) |
                                     ((uint32_t)CMD_KNIFE_OSC << 8) | node);
        bool ok = (multicore_fifo_pop_blocking() & 0xFFFF) != 0;
        Serial.printf("node %d %s\n", node, ok ? "ok" : "timeout");
        return true;
    }

    // ── knife_blower <node> <0..100> — oscillating-knife blower PWM duty ───────
    // Relays CMD_KNIFE_BLOWER to a knife node. Duty (0..100 %) packed into the
    // FIFO word's payload byte for Core 1.
    if (input.startsWith("knife_blower")) {
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

    // ── setorigin [axes] (IDLE/PAUSED/ALARM) ──────────────────────────────────
    if (input.startsWith("setorigin")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        uint8_t m = axisMask(argAfter(input, 9));
        for (int i = 0; i < 4; i++) if (m & (1 << i)) machinePos[i] = 0;
        axes_homed |= m;
        if (machineState == STATE_ALARM) {     // setorigin recovers from ALARM
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
        machineState = STATE_IDLE;
        alarmReason  = ALARM_NONE;
        Serial.println("ok");                  // position still invalid — run setorigin
        return true;
    }

    // ── step <node> <count> — debug stepping (bring-up only) ───────────────────
    if (input.startsWith("step")) {
        const char* p = argAfter(input, 4);
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(p, &endPtr, 10);
        long count = strtol(endPtr, &endPtr, 10);
        // node here addresses a stream SLOT ((node-1)*2), not a bus id, so it is
        // bounded by the axis/slot count, not BUS_ADDR_MAX.
        // if (node_isAxis(node) && count != 0) {
        if ((node >= 1 || node <= 6) && count != 0) {
            uint16_t mag = (uint16_t)labs(count) & 0x7FFF;
            if (count < 0) mag |= 0x8000;
            uint32_t word = ((uint32_t)FIFO_STEP_DEBUG << 24) | ((uint32_t)node << 16) | mag;
            multicore_fifo_push_blocking(word);
            Serial.println("ok");
        } else {
            Serial.println("err usage");
        }
        return true;
    }

    return false;
}
