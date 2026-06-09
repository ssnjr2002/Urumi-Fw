// Core 1: Real-time RS485 step emitter
//
// Consumes MicroSegments produced by the host PC (via Core 0 ingest).
// All kinematics are pre-computed on the PC — Core 1 just packs stream bytes
// and waits the prescribed interval between steps.
//
// Stream byte format (9th bit = 0):
//   Bits 1-0 : Node 1 (dir | step)
//   Bits 3-2 : Node 2
//   Bits 5-4 : Node 3
//   Bits 7-6 : Node 4

#include <Arduino.h>
#include "shared.h"
#include "RS485Bus.h"
#include "hardware/gpio.h"

RS485Bus rs485;

// ─── Local Helpers ────────────────────────────────────────────────────────────

static void sendPacket(uint8_t* packet, uint8_t len) {
    packet[len - 1] = crc8(packet, len - 1);
    for (int i = 0; i < len; i++) rs485.writeCommand(packet[i]);
}

static uint8_t receivePacket(uint8_t expectedNode, uint8_t expectedCmd,
                              uint8_t* outPayload, uint32_t timeoutMs) {
    uint32_t start = millis();
    uint8_t  rxBuf[32];
    int      rxIdx = 0;

    while (millis() - start < timeoutMs) {
        if (!rs485.available()) continue;

        uint16_t rcv = rs485.read();
        if (!(rcv & (1 << 8))) { rxIdx = 0; continue; } // stream byte — discard

        rxBuf[rxIdx++] = (uint8_t)(rcv & 0xFF);
        if (rxIdx < 4) continue;

        uint8_t payloadLen      = rxBuf[2];
        int     expectedTotalLen = 3 + payloadLen + 1;
        if (rxIdx < expectedTotalLen) continue;

        bool ok = (rxBuf[0] == expectedNode) &&
                  (rxBuf[1] == expectedCmd)  &&
                  (rxBuf[rxIdx - 1] == crc8(rxBuf, rxIdx - 1));

        if (ok) {
            if (outPayload && payloadLen > 0) memcpy(outPayload, &rxBuf[3], payloadLen);
            return payloadLen;
        }
        rxIdx = 0; // bad packet — restart
    }
    return 0xFF; // timeout
}

// ─── MicroSegment Step Emitter ────────────────────────────────────────────────
// Pack a MicroSegment's axis deltas into a single RS485 stream byte and wait
// the pre-computed interval before sending. Supports up to 4 axes (nodes 1-4).
//
// Node assignment (matches host PC convention):
//   Node 1 = X,  Node 2 = Y,  Node 3 = Z,  Node 4 = A

static void __time_critical_func(emitMicroSegment)(const MicroSegment& ms) {
    // A MicroSegment describes a block of steps: the major axis takes
    // max(|dx|,|dy|,|dz|,|da|) steps, minor axes are Bresenham-distributed
    // against it. `interval` is the time (CPU cycles) per major-axis step.
    // The host has already resolved velocity — the Pico just executes.
    //
    // Node bit layout: bit(2n) = step, bit(2n+1) = dir (1 = CW / positive).

    int32_t  delta[4] = { ms.dx, ms.dy, ms.dz, ms.da };
    uint32_t absSteps[4];
    uint8_t  dirBits = 0;
    uint32_t maxSteps = 0;

    for (int i = 0; i < 4; i++) {
        absSteps[i] = (delta[i] < 0) ? (uint32_t)(-delta[i]) : (uint32_t)delta[i];
        if (absSteps[i] > maxSteps) maxSteps = absSteps[i];
        if (delta[i] > 0) dirBits |= (1 << (i * 2 + 1)); // positive = CW
    }

    if (maxSteps == 0) return; // no motion this segment

    // Bresenham error accumulators — symmetric init for centred distribution
    uint32_t err[4] = { maxSteps / 2, maxSteps / 2, maxSteps / 2, maxSteps / 2 };

    uint32_t t0 = rp2040.getCycleCount();
    for (uint32_t s = 0; s < maxSteps; s++) {
        if (emergencyStop) return;

        uint8_t streamByte = dirBits;
        for (int i = 0; i < 4; i++) {
            if (absSteps[i] == 0) continue;
            err[i] += absSteps[i];
            if (err[i] >= maxSteps) {
                err[i] -= maxSteps;
                streamByte |= (1 << (i * 2));   // step bit
            }
        }

        // Wait the prescribed per-step interval, then emit
        while ((rp2040.getCycleCount() - t0) < ms.interval) {
            if (emergencyStop) return;
        }
        t0 += ms.interval;

        rs485.writeStream(streamByte);
    }
}

static void __time_critical_func(processMicroSegments)() {
    while (mBufHead != mBufTail) {
        if (emergencyStop) return;

        MicroSegment ms = masterBuf[mBufHead];

        // Poison pill — flush and signal estop
        if (ms.flags & MSEG_FLAG_ESTOP) {
            emergencyStop = true;
            return;
        }

        emitMicroSegment(ms);

        __dmb();
        mBufHead = (mBufHead + 1) % MASTER_BUF_SIZE;
    }
}

// ─── Core 1 Setup & Loop ──────────────────────────────────────────────────────

void setup1() {
    rs485.begin(RS485_BAUD, RS485_TX_PIN, RS485_RX_PIN, RS485_EN_PIN);
}

void loop1() {
    // 1. Handle emergency stop — flush the buffer and clear the flag
    if (emergencyStop) {
        mBufHead = mBufTail;
        __dmb();
        emergencyStop = false;
        return;
    }

    // 2. Emit any queued MicroSegments
    if (mBufHead != mBufTail) processMicroSegments();

    // 3. Handle text commands from Core 0 (ping / enable / disable / getpos)
    if (multicore_fifo_rvalid()) {
        uint32_t req  = multicore_fifo_pop_blocking();
        uint8_t  cmd  = (req >> 8) & 0xFF;
        uint8_t  node =  req & 0xFF;

        while (!rs485.txEmpty());
        rs485.flushRX();
        rs485.writeStream(0); // NOP stream byte to reset slave parsers

        switch (cmd) {
            case CMD_PING: {
                uint8_t pkt[4] = {node, CMD_PING, 0, 0};
                sendPacket(pkt, 4);
                uint8_t rxLen = receivePacket(node, CMD_PONG, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_PING << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            case CMD_GET_POS: {
                uint8_t pkt[4] = {node, CMD_GET_POS, 0, 0};
                sendPacket(pkt, 4);
                uint8_t payload[4];
                uint8_t rxLen = receivePacket(node, CMD_GET_POS, payload, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_GET_POS << 24) | (node << 16) | (rxLen == 4 ? 1u : 0u));
                if (rxLen == 4) {
                    int32_t pos = ((int32_t)payload[0] << 24) | ((int32_t)payload[1] << 16) |
                                  ((int32_t)payload[2] <<  8) |  (int32_t)payload[3];
                    multicore_fifo_push_blocking((uint32_t)pos);
                }
                break;
            }
            case CMD_ENABLE: {
                uint8_t pkt[4] = {node, CMD_ENABLE, 0, 0};
                sendPacket(pkt, 4);
                uint8_t rxLen = receivePacket(node, CMD_ENABLE, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_ENABLE << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
            case CMD_DISABLE: {
                uint8_t pkt[4] = {node, CMD_DISABLE, 0, 0};
                sendPacket(pkt, 4);
                uint8_t rxLen = receivePacket(node, CMD_DISABLE, nullptr, RESPONSE_TIMEOUT_MS);
                multicore_fifo_push_blocking((CMD_DISABLE << 24) | (node << 16) | (rxLen != 0xFF ? 1u : 0u));
                break;
            }
        }
    } else {
        delayMicroseconds(10);
    }
}
