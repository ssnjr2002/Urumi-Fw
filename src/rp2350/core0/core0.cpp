// Core 0: USB serial dispatch + lifecycle.
//
// Owns the USB CDC ingest loop and the soft-reset sequence. The two planes that
// share the pipe live in their own translation units (docs/wire_protocol.md):
//   data_plane.*    — binary MicroSegment / jog packets (magic-dispatched)
//   control_plane.* — text command lines
//   status.*        — binary STATUS_RSP + buffer telemetry
// This file only routes bytes to them and manages Core 0's setup()/loop().

#include <Arduino.h>
#include "../shared.h"
#include "hardware/sync.h"
#include "control_plane.h"
#include "data_plane.h"
#include "status.h"

// ─── Text line assembly ───────────────────────────────────────────────────────

static char     serialRxBuf[128];
static uint8_t  serialRxLen = 0;

// ─── Serial Processing ────────────────────────────────────────────────────────
// Text lines and binary packets share the same USB CDC stream.
// Data-plane magics (MSEG/JOG 0xAB/0xAE, STATUS_REQ 0xA5) have bit 7 set and
// are dispatched here; any other byte is treated as the start of a text line.

void processSerial() {
    while (Serial.available() && !soft_reset_requested) {
        uint8_t b = (uint8_t)Serial.read();

        // Data plane (binary): consumes mid-packet bytes and packets started by a
        // magic byte. Returns false if the byte belongs to the control plane.
        if (dataPlaneConsume(b)) continue;

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
    // (Config cache is intentionally NOT wiped — it mirrors flash, which the soft
    // reset does not touch; re-zeroing it would blank a valid config from RAM.)
    serialRxLen = 0;
    dataPlaneReset();

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
