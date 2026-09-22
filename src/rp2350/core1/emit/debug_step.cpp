// debug_step.cpp — the raw stream-byte burst emitter. See emit.h.
#include <Arduino.h>
#include "emit.h"
#include "common.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"   // STEP_DEBUG_SPS
#include "../bus/packet.h"

void emitDebugSteps(uint8_t slot, uint16_t sps, int32_t steps) {
    const bool     neg   = (steps < 0);
    const uint32_t count = (uint32_t)(neg ? -(int64_t)steps : (int64_t)steps);

    if (slot >= 4) return;                            // 4 stream slots (X/Y/Z/A)

    uint8_t bit = slot * 2;
    uint8_t streamByte = (1 << bit);                  // step bit
    if (!neg) streamByte |= (1 << (bit + 1));         // dir bit (positive = CW)

    if (sps == 0) sps = STEP_DEBUG_SPS;
    uint32_t interval = F_CPU / sps;

    while (!rs485.txEmpty());
    rs485.flushRX();
    rs485.writeStream(0);  // NOP to reset slave parsers

    uint32_t t0 = rp2040.getCycleCount();
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (machineState == STATE_ESTOP) break;
        while ((rp2040.getCycleCount() - t0) < interval) {
            if (machineState == STATE_ESTOP) break;
        }
        t0 += interval;
        rs485.writeStream(streamByte);
        emitted++;
    }

    // Debug stepping is TRACKED, not untracked: the target node is engaged (Core 0
    // refuses otherwise), so its RX ISR counts every one of these bytes into its
    // own absolutePosition exactly as it would during a job. The node-frame datum
    // therefore stays valid — nodePos - nodeOrigin still resolves correctly — and
    // clearing axes_homed here would throw away a datum that is still sound.
    //
    // Count what was actually emitted, not what was asked for: an estop can cut
    // the burst short (and invalidates the datum by its own path anyway).
    machinePos[slot] += neg ? -(int32_t)emitted : (int32_t)emitted;
}
