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
    // Bare / `all` pings nodes 1-4 (one reply line each, bring-up convenience);
    // `pingnode <id>` is the single-line form the host pre-flight uses.
    if (input.startsWith("pingnode")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 8);
        if (*a == '\0' || strcmp(a, "all") == 0) {
            for (uint8_t n = 1; n <= 4; n++)
                Serial.printf("node %d %s\n", n, relayNode(CMD_PING, n) ? "ok" : "timeout");
        } else {
            uint8_t node = (uint8_t)strtoul(a, NULL, 10);
            if (node < 1 || node > 4) { Serial.println("err bad_node"); return true; }
            Serial.printf("node %d %s\n", node, relayNode(CMD_PING, node) ? "ok" : "timeout");
        }
        return true;
    }

    // ── enable / disable [all|<id>] (IDLE/PAUSED/ALARM) ───────────────────────
    if (input.startsWith("enable")) {
        if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
            Serial.println("err bad_state"); return true;
        }
        const char* a = argAfter(input, 6);
        if (*a == '\0' || strcmp(a, "all") == 0) {
            for (uint8_t n = 1; n <= 4; n++) relayNode(CMD_ENABLE, n);
            axes_enabled = 0x0F;
        } else {
            uint8_t node = (uint8_t)strtoul(a, NULL, 10);
            if (node < 1 || node > 4) { Serial.println("err bad_node"); return true; }
            relayNode(CMD_ENABLE, node);
            axes_enabled |= (1 << (node - 1));
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
            for (uint8_t n = 1; n <= 4; n++) relayNode(CMD_DISABLE, n);
            axes_enabled = 0;
            axes_homed   = 0;          // de-energised → datum lost on every axis
        } else {
            uint8_t node = (uint8_t)strtoul(a, NULL, 10);
            if (node < 1 || node > 4) { Serial.println("err bad_node"); return true; }
            relayNode(CMD_DISABLE, node);
            axes_enabled &= ~(1 << (node - 1));
            axes_homed   &= ~(1 << (node - 1));
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
        if (node >= 1 && node <= 4 && count != 0) {
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
