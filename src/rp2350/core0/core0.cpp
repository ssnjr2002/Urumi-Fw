// Core 0: USB serial dispatch + lifecycle.
//
// Owns the USB CDC ingest loop and the soft-reset sequence. The two planes that
// share the pipe live in their own translation units (docs/wire_protocol.md):
//   data_plane.*    — binary records, jog and config packets (magic-dispatched)
//   control_plane.* — text command lines
//   status.*        — binary STATUS_RSP + buffer telemetry
// This file only routes bytes to them and manages Core 0's setup()/loop().

#include <Arduino.h>
#include "../ipc/shared_state.h"
#include "../ipc/core1_rpc.h"
#include "../ipc/core1_facts.h"
#include "hardware/sync.h"
#include "control_plane.h"
#include "ops/position.h"
#include "ops/observe.h"
#include "controller/seq/toolchange.h"
#include "ops/mesh.h"
#include "ops/slot_map.h"
#include "ops/bus.h"
#include "ops/machine.h"
#include "ops/runner.h"
#include "ops/sequence.h"
#include "ops/probe.h"
#include "data_plane.h"
#include "status.h"
#include "config/config_store.h"
#include "config/machine_cfg.h"
#include "controller/seq/controller.h"

// ─── Text line assembly ───────────────────────────────────────────────────────

static char     serialRxBuf[256];
static uint16_t serialRxLen = 0;

// ─── Serial Processing ────────────────────────────────────────────────────────
// Text lines and binary packets share the same USB CDC stream.
// Data-plane magics (MSEG/JOG 0xAB/0xAE, STATUS_REQ 0xA5) have bit 7 set and
// are dispatched here; any other byte is treated as the start of a text line.

void processSerial() {
    while (Serial.available() && !soft_reset_requested) {
        uint8_t b = (uint8_t)Serial.read();

        // Data plane (binary): consumes mid-packet bytes and packets started by a
        // magic byte. Returns false if the byte belongs to the control plane.
        if (dataPlaneConsume(b)) { machinePublish(); continue; }

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
                machinePublish();   // the next command reads what this one left
            }
        } else if (serialRxLen < sizeof(serialRxBuf) - 1) {
            serialRxBuf[serialRxLen++] = c;
        }
    }
}

// ─── Core 0 Setup & Loop ──────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    rpcInit();           // channel-1 queues, before Core 1 can service them
    plannerLock = spin_lock_instance(spin_lock_claim_unused(true));
    while (!Serial && millis() < 10000) {}
    configStoreInit();   // mount LittleFS, verify /config.bin into g_cfg
    machineCfgLoad();    // decode it; the axis map follows once Core 1 runs
    meshInit();          // /mesh.bin into RAM, on per the config
}

void loop() {
    // ══════════════════════════════════════════════════════════
    // ─── A: THE SOFT RESET SEQUENCE ─────────────────────
    // ══════════════════════════════════════════════════════════

    // 1. Tell Core 1 to stop working
    soft_reset_requested = true;

    // 2. Wait for Core 1 to safely finish its current operation and park
    while (!core1_parked_for_reset) {
        delay(1);
    }

    // --- CORE 1 IS NOW LOCKED ---
    // It is 100% safe to wipe cross-core variables without mutexes.

    // 3. Flush USB Serial (discard any half-received junk)
    while (Serial.available()) {
        Serial.read();
    }

    // 4. Flush the channel-1 queues.
    //
    // This used to be multicore_fifo_drain(), which is now the wrong object:
    // channel 1 moved off the hardware FIFO onto queue_t (ipc/core1_rpc.h), so
    // draining the FIFO would leave a stale request or a stale reply sitting in
    // the queues across a soft reset. Core 1 is parked here, so nothing can be
    // added while we drain.
    rpcReset();
    core1FactsReset();

    // 5. WIPE ALL GLOBAL STATE (Clean Slate!)
    // Held in ALARM until section B settles it: with a valid config the
    // controller commits the defaultHead map as soon as Core 1 is released;
    // without one nothing is requested and the machine lands in ALARM with
    // F_CONFIG (IDLE, unmapped, after `uncfg`).
    // The slot table (bindings and fences) survives: only power-on clears it.
    slotMapReset();
    slotMapForget();
    machineReset();
    jobReason = JOB_BUFFERING;
    probingReason = PROBING_CLEAR;
    homingReason = HOMING_WAIT;
    machinePos[0] = machinePos[1] = machinePos[2] = machinePos[3] = 0;
    axes_homed = 0;
    // axes_enabled is DERIVED and deliberately not wiped here: projectEnabled
    // rebuilds it from nodeEnabled at the top of the loop below, before anything
    // host-observable runs. nodeEnabled itself is Core 1's — Core 0 must not
    // write it — and the boot sweep below clears it, per node, as each one
    // confirms its make-safe (ops/bus.h).
    // Zeroing the projection here would just assert a value one pass early, and
    // give a derived byte a second writer.
    pauseRequested = false;
    abortRequested = false;
    toolChangePending = false;
    plannerRing.reset({0, 0});
    plannerExec.reset({0, 0});
    plannerActive = false;
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
    Serial.printf("RS485 controller (%d baud)\n", RS485_BAUD);

    busSweep();
    if (busDegraded()) machinePublish();   // F_BUS, no map
    else               controllerApplyDefaultMap();
    // The boot sequence is done; the banner above is informational.
    Serial.println("ready");

    while (!soft_reset_requested) {
        // Fold in what Core 1 reported BEFORE serving the host.
        observeTick();
        processSerial();
        dataPlaneTick();   // abort a stalled CFG_SET transfer (inter-byte timeout)
        // Poll node-run legs to completion, then let the live sequence judge
        // their ends and arm its next step. Out here rather than inside the `leg` handler because the
        // control plane owes one reply line per command: blocking there would
        // hold `get` and `stop` shut for the whole seek, on the one command
        // that is driving an axis at a hard stop.
        runnerTick();
        sequenceTick();
        toolChangeTick();
        // And a probe leg, for the same reason and on the same terms: a leg runs
        // for seconds with a tool descending onto a bed, and holding `get`
        // and `stop` shut for the whole of it is exactly what a supervisor out
        // here avoids.
        probeTick();
        // Last: what the pass left.
        machinePublish();
        // Node-relay commands (pingnode/enable/disable) consume their Core 1 FIFO
        // responses synchronously inside handleCommand (relayNode), so there is no
        // async response stream to drain here. Backpressure is handled by the
        // windowed sender via NACK_FULL, not an out-of-band "ready" line.
    }
}
