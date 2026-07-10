// Core 0: USB serial ingest — text commands + binary MicroSegment packets

#include <Arduino.h>
#include "../shared.h"
#include "hardware/sync.h"

// ─── Local State ──────────────────────────────────────────────────────────────

static char     serialRxBuf[128];
static uint8_t  serialRxLen   = 0;

// Binary ingest state machine
static uint8_t  pktBuf[MSEG_PACKET_SIZE];
static uint8_t  pktIdx        = 0;
static bool     inPacket      = false;
static uint16_t pktSeq        = 0;   // rolling counter for ACK echo
static uint8_t  expectedSeq   = 0;   // next wire seq (pktBuf[22]) we will execute

// ─── Helpers ──────────────────────────────────────────────────────────────────

static uint16_t getBufCount() {
    uint16_t h = mBufHead, t = mBufTail;
    if (t >= h) return t - h;
    return MASTER_BUF_SIZE - h + t;
}

static void sendAck() {
    Serial.write(MSEG_ACK);
    Serial.write((uint8_t)(pktSeq & 0xFF));
    Serial.write((uint8_t)(pktSeq >> 8));
    pktSeq++;
}

static void sendNack(uint8_t reason) {
    Serial.write(MSEG_NACK);
    Serial.write(reason);
    Serial.write((uint8_t)0x00);
}

// Binary mirror of `getstate` (docs/wire_protocol.md STATUS_REQ/STATUS_RSP).
// Accepted in every machine state; handled inline (no ring-buffer / Core 1
// interaction) so it never delays step timing.
static void sendStatusRsp() {
    uint8_t buf[STATUS_RSP_SIZE];
    uint16_t bufCount = getBufCount();
    buf[0] = STATUS_RSP;
    buf[1] = machineState;
    buf[2] = axes_enabled;
    buf[3] = axes_homed;
    buf[4] = alarmReason;
    buf[5] = runningReason;
    buf[6] = (uint8_t)(bufCount & 0xFF);
    buf[7] = (uint8_t)(bufCount >> 8);
    buf[8] = crc8(buf, STATUS_RSP_SIZE - 1);
    Serial.write(buf, STATUS_RSP_SIZE);
}

// ─── Command Handlers ─────────────────────────────────────────────────────────

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
static bool handleCommand(const String& input) {

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
        expectedSeq = 0;
        pktSeq      = 0;
        Serial.println("seq reset");
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
        pauseRequested = true;                 // Core 1 drains, then → PAUSED
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

// ─── Binary MicroSegment Ingest ───────────────────────────────────────────────
// Packet layout (MSEG_PACKET_SIZE = 26 bytes):
//   [0]      magic  0xAB
//   [1..24]  MicroSegment (24 bytes, little-endian; byte [22] = rolling seq
//            stamped by the host sender, used for the duplicate guard)
//   [25]     CRC8 over bytes [0..24]
//
// On success: push to ring buffer, send ACK (3 bytes).
// On failure: send NACK with reason byte, reset state machine.

static void processBinaryByte(uint8_t b) {
    if (!inPacket) {
        if (b == MSEG_MAGIC || b == JOG_MAGIC) {
            pktBuf[0] = b;            // remember which stream type for the state gate
            pktIdx    = 1;
            inPacket  = true;
        }
        // Any non-magic byte while idle is ignored (text commands handled separately)
        return;
    }

    pktBuf[pktIdx++] = b;

    if (pktIdx < MSEG_PACKET_SIZE) return; // Still accumulating

    // Full packet received — validate CRC
    inPacket = false;
    pktIdx   = 0;

    uint8_t expected = crc8(pktBuf, MSEG_PACKET_SIZE - 1);
    if (pktBuf[MSEG_PACKET_SIZE - 1] != expected) {
        sendNack(MSEG_NACK_CRC);
        return;
    }

    // State gate (wire_protocol.md allowed-state matrix). The magic byte selects
    // the stream type; we record it as the streamIsJog intent so Core 1 sets
    // runningReason together with the RUNNING transition it owns.
    //   MSEG job stream — IDLE/RUNNING; NACK_PAUSED while paused, else bad_state.
    //   JOG burst       — IDLE/PAUSED, or RUNNING if the in-progress burst is
    //                     itself a jog (packet 2+ of the same multi-packet
    //                     burst arrives after Core 1 has already flipped the
    //                     state to RUNNING to execute packet 1 — rejecting
    //                     those left every jog after the first packet
    //                     NACK_BAD_STATE'd forever, scrambling the motion).
    uint8_t st = machineState;
    if (pktBuf[0] == MSEG_MAGIC) {
        if (st == STATE_PAUSED)                          { sendNack(MSEG_NACK_PAUSED);    return; }
        if (st != STATE_IDLE && st != STATE_RUNNING)     { sendNack(MSEG_NACK_BAD_STATE); return; }
        streamIsJog = false;
    } else { // JOG_MAGIC
        bool continuingJog = (st == STATE_RUNNING && runningReason == RUNNING_JOG);
        if (st != STATE_IDLE && st != STATE_PAUSED && !continuingJog) {
            sendNack(MSEG_NACK_BAD_STATE); return;
        }
        streamIsJog = true;
    }

    // Duplicate guard: byte [22] carries the host's rolling 8-bit seq. After a
    // NACK the host rewinds (Go-Back-N) and may resend packets we already
    // accepted; executing them again would duplicate motion — a permanent
    // position offset. A seq we are not expecting is a stale retransmit: ACK it
    // (so the host's window advances) but do not execute. The host resets this
    // counter with the "seqreset" text command before each stream.
    if (pktBuf[22] != expectedSeq) {
        sendAck();
        return;
    }

    // Check buffer space
    uint16_t next = (mBufTail + 1) % MASTER_BUF_SIZE;
    if (next == mBufHead) {
        sendNack(MSEG_NACK_FULL);     // backpressure — windowed sender retries
        return;
    }

    // Deserialise MicroSegment from bytes [1..24] (little-endian)
    MicroSegment ms;
    const uint8_t* p = &pktBuf[1];
    memcpy(&ms.dx,       p,      4); p += 4;
    memcpy(&ms.dy,       p,      4); p += 4;
    memcpy(&ms.dz,       p,      4); p += 4;
    memcpy(&ms.da,       p,      4); p += 4;
    memcpy(&ms.interval, p,      4); p += 4;
    ms.flags  = *p++;
    ms.pad[0] = ms.pad[1] = ms.pad[2] = 0;

    masterBuf[mBufTail] = ms;
    __dmb();
    mBufTail = next;

    expectedSeq++;
    sendAck();
}

// ─── Serial Processing ────────────────────────────────────────────────────────
// Text lines and binary packets share the same USB CDC stream.
// Data-plane magics (MSEG/JOG 0xAB/0xAE, STATUS_REQ 0xA5) have bit 7 set and
// are dispatched here; any other byte is treated as the start of a text line.

void processSerial() {
    while (Serial.available() && !soft_reset_requested) {
        uint8_t b = (uint8_t)Serial.read();

        // If we're mid-packet, feed every byte to the binary state machine
        if (inPacket) {
            processBinaryByte(b);
            continue;
        }

        // A data-plane magic byte starts a binary packet
        if (b == MSEG_MAGIC || b == JOG_MAGIC) {
            processBinaryByte(b);
            continue;
        }

        // STATUS_REQ is a single byte, no payload/CRC — answer immediately.
        if (b == STATUS_REQ) {
            sendStatusRsp();
            continue;
        }

        // Otherwise treat as a control-plane text line
        char c = (char)b;
        if (c == '\n' || c == '\r') {
            if (serialRxLen > 0) {
                serialRxBuf[serialRxLen] = '\0';
                String input = String(serialRxBuf);
                serialRxLen = 0;

                if (!handleCommand(input)) Serial.println("err unknown");
            }
        } else if (serialRxLen < sizeof(serialRxBuf) - 1) {
            serialRxBuf[serialRxLen++] = c;
        }
    }
}

// ─── Core 0 Setup & Loop ──────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 10000) {}
}

void loop() {
    // ══════════════════════════════════════════════════════════
    // ─── A: THE SOFT RESET SEQUENCE ─────────────────────
    // ══════════════════════════════════════════════════════════
    
    // 1. Tell Core 1 to stop working
    soft_reset_requested = true;

    // 2. Wait for Core 1 to safely finish its current operation and park
    while (!core1_is_parked) {
        delay(1);
    }

    // --- CORE 1 IS NOW LOCKED ---
    // It is 100% safe to wipe cross-core variables without mutexes.

    // 3. Flush USB Serial (discard any half-received junk)
    while (Serial.available()) {
        Serial.read();
    }

    // 4. Flush hardware FIFOs
    multicore_fifo_drain();

    // 5. WIPE ALL GLOBAL STATE (Clean Slate!)
    mBufHead = 0;
    mBufTail = 0;
    machineState = STATE_IDLE;
    alarmReason = ALARM_NONE;
    runningReason = RUNNING_JOB;
    machinePos[0] = machinePos[1] = machinePos[2] = machinePos[3] = 0;
    axes_homed = 0;
    axes_enabled = 0;
    jobActive = false;
    resumePos[0] = resumePos[1] = resumePos[2] = resumePos[3] = 0;
    pauseRequested = false;
    streamIsJog = false;
    __dmb();

    // 5b. WIPE LOCAL INGEST STATE
    serialRxLen = 0;
    inPacket    = false;
    pktIdx      = 0;
    expectedSeq = 0;
    pktSeq      = 0;
    
    // 6. Release Core 1 to start working again
    soft_reset_requested = false;

    // ══════════════════════════════════════════════════════════
    // ─── B: MAIN EXECUTION ──────────────────────────────
    // ══════════════════════════════════════════════════════════
    Serial.printf("RS485 MicroSegment Host Drive (%d baud)\n", RS485_BAUD);
    
    while (!soft_reset_requested) {
        processSerial();
        // Node-relay commands (pingnode/enable/disable) consume their Core 1 FIFO
        // responses synchronously inside handleCommand (relayNode), so there is no
        // async response stream to drain here. Backpressure is handled by the
        // windowed sender via NACK_FULL, not an out-of-band "ready" line.
    }
}
