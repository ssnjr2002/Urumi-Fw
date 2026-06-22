// Core 0: USB serial ingest — text commands + binary MicroSegment packets

#include <Arduino.h>
#include "shared.h"
#include "hardware/sync.h"

// ─── Local State ──────────────────────────────────────────────────────────────

static char     serialRxBuf[128];
static uint8_t  serialRxLen   = 0;
static bool     bufWasFull    = false;

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

// ─── Command Handlers ─────────────────────────────────────────────────────────

static const char* stateName(uint8_t s) {
    switch (s) {
        case STATE_IDLE:    return "IDLE";
        case STATE_RUNNING: return "RUNNING";
        case STATE_ESTOP:   return "ESTOP";
        case STATE_ALARM:   return "ALARM";
        default:            return "?";
    }
}

static bool handleControlCommand(const String& input) {
    if (input == "stop") {
        machineState = STATE_ESTOP;            // Core 1 flushes and drops to ALARM
        Serial.println("!!! STOP DETECTED !!!");
        return true;
    }
    if (input.startsWith("unalarm")) {
        // Acknowledge the alarm but leave position invalid — use setorigin to
        // re-establish a known origin before running again.
        if (machineState == STATE_ALARM || machineState == STATE_ESTOP)
            machineState = STATE_IDLE;
        Serial.println("Alarm cleared! (position still invalid — run setorigin)");
        return true;
    }
    if (input == "setorigin") {
        if (machineState == STATE_RUNNING) {
            Serial.println("Error: cannot set origin while RUNNING");
            return true;
        }
        machinePos[0] = machinePos[1] = machinePos[2] = machinePos[3] = 0;
        positionValid = true;
        if (machineState == STATE_ALARM) machineState = STATE_IDLE;
        Serial.println("Origin set");
        return true;
    }
    if (input == "seqreset") {
        // Host sends this before every stream so its packet index 0 lines up
        // with our duplicate-guard expectation. Also resets the ACK echo.
        expectedSeq = 0;
        pktSeq      = 0;
        Serial.println("seq reset");
        return true;
    }
    if (input == "status" || input == "?") {
        uint8_t s = machineState;
        Serial.printf("state=%s pos=%ld,%ld,%ld,%ld valid=%d buf=%u/%u",
                      stateName(s),
                      (long)machinePos[0], (long)machinePos[1],
                      (long)machinePos[2], (long)machinePos[3],
                      positionValid ? 1 : 0,
                      getBufCount(), MASTER_BUF_SIZE);
#ifdef DEBUG_TIMING
        Serial.printf(" texp=%lu tmeas=%lu twall=%lu",
                      (unsigned long)jobExpectedUs,
                      (unsigned long)jobMeasuredUs,
                      (unsigned long)jobWallUs);
#endif
        Serial.printf("\n");
        return true;
    }
    return false;
}

static bool handleNonStreamingCommand(const String& input) {
    bool isCommand = input.startsWith("ping")    ||
                     input.startsWith("enable")  ||
                     input.startsWith("disable") ||
                     input.startsWith("getpos")  ||
                     input.startsWith("step")    ||
                     input.startsWith("suction");

    if (!isCommand) return false;

    if (!multicore_fifo_wready()) {
        Serial.println("Error: Command queue full.");
        return true;
    }

    if (input.startsWith("ping all")) {
        Serial.println("Queued PING ALL sequence...");
        for (uint8_t i = 1; i <= 4; i++) multicore_fifo_push_blocking((CMD_PING << 8) | i);
    }
    else if (input.startsWith("enable all")) {
        Serial.println("Queued ENABLE ALL sequence...");
        for (uint8_t i = 1; i <= 4; i++) multicore_fifo_push_blocking((CMD_ENABLE << 8) | i);
    }
    else if (input.startsWith("disable all")) {
        Serial.println("Queued DISABLE ALL sequence...");
        for (uint8_t i = 1; i <= 4; i++) multicore_fifo_push_blocking((CMD_DISABLE << 8) | i);
    }
    else if (input.startsWith("ping")) {
        char* ptr = (char*)input.c_str() + 4;
        while (*ptr == ' ') ptr++;
        uint8_t node = (uint8_t)strtoul(ptr, NULL, 10);
        if (node >= 1 && node <= 4) { Serial.printf("Queued PING for Node %d...\n", node); multicore_fifo_push_blocking((CMD_PING << 8) | node); }
        else Serial.println("Error: Invalid Node ID for ping.");
    }
    else if (input.startsWith("enable")) {
        char* ptr = (char*)input.c_str() + 6;
        while (*ptr == ' ') ptr++;
        uint8_t node = (uint8_t)strtoul(ptr, NULL, 10);
        if (node >= 1 && node <= 4) { Serial.printf("Queued ENABLE for Node %d...\n", node); multicore_fifo_push_blocking((CMD_ENABLE << 8) | node); }
        else Serial.println("Error: Invalid Node ID for enable.");
    }
    else if (input.startsWith("disable")) {
        char* ptr = (char*)input.c_str() + 7;
        while (*ptr == ' ') ptr++;
        uint8_t node = (uint8_t)strtoul(ptr, NULL, 10);
        if (node >= 1 && node <= 4) { Serial.printf("Queued DISABLE for Node %d...\n", node); multicore_fifo_push_blocking((CMD_DISABLE << 8) | node); }
        else Serial.println("Error: Invalid Node ID for disable.");
    }
    else if (input.startsWith("getpos")) {
        char* ptr = (char*)input.c_str() + 6;
        while (*ptr == ' ') ptr++;
        uint8_t node = (uint8_t)strtoul(ptr, NULL, 10);
        if (node >= 1 && node <= 4) { Serial.printf("Queued Position Query for Node %d...\n", node); multicore_fifo_push_blocking((CMD_GET_POS << 8) | node); }
        else Serial.println("Error: Invalid Node ID for getpos.");
    }
    else if (input.startsWith("step")) {
        // step <node> <count>  — direct debug stepping, bypasses MicroSegment path.
        // Negative count steps in the reverse direction. Node must be enabled first.
        char* ptr = (char*)input.c_str() + 4;
        while (*ptr == ' ') ptr++;
        char* endPtr;
        uint8_t node = (uint8_t)strtoul(ptr, &endPtr, 10);
        ptr = endPtr;
        long count = strtol(ptr, &endPtr, 10);
        if (node >= 1 && node <= 4 && count != 0) {
            uint16_t mag = (uint16_t)labs(count) & 0x7FFF;
            if (count < 0) mag |= 0x8000;
            uint32_t word = ((uint32_t)FIFO_STEP_DEBUG << 24) | ((uint32_t)node << 16) | mag;
            Serial.printf("Queued STEP DEBUG node %d count %ld...\n", node, count);
            multicore_fifo_push_blocking(word);
        } else {
            Serial.println("Error: usage: step <node 1-4> <count != 0>");
        }
    }
    else if (input.startsWith("suction")) {
        Serial.println("ok");
    }

    return true;
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
        if (b == MSEG_MAGIC) {
            pktBuf[0] = b;
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
        bufWasFull = true;
        sendNack(MSEG_NACK_FULL);
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
// Binary packets start with 0xAB — any byte >= 0x80 that isn't mid-packet
// is treated as a potential packet start. Printable ASCII goes to the text parser.

void processSerial() {
    while (Serial.available()) {
        uint8_t b = (uint8_t)Serial.read();

        // If we're mid-packet, feed every byte to the binary state machine
        if (inPacket) {
            processBinaryByte(b);
            continue;
        }

        // Magic byte starts a binary packet
        if (b == MSEG_MAGIC) {
            processBinaryByte(b);
            continue;
        }

        // Otherwise treat as text
        char c = (char)b;
        if (c == '\n' || c == '\r') {
            if (serialRxLen > 0) {
                serialRxBuf[serialRxLen] = '\0';
                String input = String(serialRxBuf);
                serialRxLen = 0;

                if      (handleControlCommand(input))    { /* handled */ }
                else if (handleNonStreamingCommand(input)) { /* handled */ }
                else    Serial.println("Error: Unknown command.");
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
    Serial.printf("RS485 MicroSegment Host Drive (%d baud)\n", RS485_BAUD);
}

void loop() {
    processSerial();

    // Relay Core 1 FIFO responses to USB
    while (multicore_fifo_rvalid()) {
        uint32_t resp = multicore_fifo_pop_blocking();
        uint8_t  cmd     = (resp >> 24) & 0xFF;
        uint8_t  node    = (resp >> 16) & 0xFF;
        uint16_t success =  resp & 0xFFFF;

        switch (cmd) {
            case CMD_PING:
                if (success) Serial.printf("Node %d: PONG\n", node);
                else         Serial.printf("Node %d: Timeout (PING)\n", node);
                break;
            case CMD_GET_POS:
                if (success) {
                    int32_t pos = (int32_t)multicore_fifo_pop_blocking();
                    Serial.printf("Node %d pos=%ld\n", node, pos);
                } else {
                    Serial.printf("Node %d: Timeout (GET_POS)\n", node);
                }
                break;
            case CMD_ENABLE:
                if (success) Serial.printf("Node %d: Enabled\nok\n", node);
                else         Serial.printf("Node %d: Timeout (ENABLE)\n", node);
                break;
            case CMD_DISABLE:
                if (success) Serial.printf("Node %d: Disabled\nok\n", node);
                else         Serial.printf("Node %d: Timeout (DISABLE)\n", node);
                break;
        }
    }

    // Signal host when buffer drains below watermark after a backpressure event
    if (bufWasFull) {
        bufWasFull = (getBufCount() > MASTER_BUF_LOW_WATERMARK);
        if (!bufWasFull) Serial.println("ready");
    }
}
