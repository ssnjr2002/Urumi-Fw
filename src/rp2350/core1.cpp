// Core 1: Handles the real-time RS485 byte streaming

#include <Arduino.h>
#include "shared.h"
#include "hardware/gpio.h"

#include "hardware/gpio.h"

#include "RS485Bus.h"

RS485Bus rs485;

// ─── Local Helpers ────────────────────────────────────────────────────────────

static void sendPacket(uint8_t* packet, uint8_t len) {
    packet[len - 1] = crc8(packet, len - 1);
    for (int i = 0; i < len; i++) {
        rs485.writeCommand(packet[i]);
    }
}

static uint8_t receivePacket(uint8_t expectedNode, uint8_t expectedCmd, uint8_t* outPayload, uint32_t timeoutMs) {
    uint32_t startWait = millis();
    uint8_t rxBuf[32];
    int rxIdx = 0;
    
    while (millis() - startWait < timeoutMs) {
        if (!rs485.available()) {
            continue;
        }

        uint16_t rcv = rs485.read();
        
        // Abort packet parsing if a stream byte (9th bit = 0) is received
        if (!(rcv & (1 << 8))) {
            rxIdx = 0;
            continue;
        }
        
        // Command byte (9th bit = 1)
        rxBuf[rxIdx++] = (uint8_t)(rcv & 0xFF);
        
        // Minimum packet length is 4 (Node + Cmd + Len + CRC)
        if (rxIdx < 4) {
            continue;
        }
        
        uint8_t payloadLen = rxBuf[2];
        int expectedTotalLen = 3 + payloadLen + 1;
        
        // Wait until we have the full packet
        if (rxIdx < expectedTotalLen) {
            continue;
        }
        
        // Full packet received, validate it
        bool validNode = (rxBuf[0] == expectedNode);
        bool validCmd = (rxBuf[1] == expectedCmd);
        bool validCrc = (rxBuf[rxIdx - 1] == crc8(rxBuf, rxIdx - 1));
        
        if (validNode && validCmd && validCrc) {
            if (outPayload && payloadLen > 0) {
                memcpy(outPayload, &rxBuf[3], payloadLen);
            }
            return payloadLen; // Success
        }
        
        // Invalid packet (bad CRC, wrong node, etc), discard and restart
        rxIdx = 0;
    }
    return 0xFF; // Timeout
}

// ─── Core 1 Setup & Loop (RS485 Engine) ───────────────────────────────────────

void setup1() {
    rs485.begin(RS485_BAUD, RS485_TX_PIN, RS485_RX_PIN, RS485_EN_PIN);
    
    pinMode(11, OUTPUT);
}

static void __time_critical_func(processStreamingBuffer)() {
    while (mBufHead != mBufTail) {
        if (emergencyStop) break; // Drop out instantly if a stop comes in!

        volatile Segment &s = masterBuf[mBufHead];

        uint32_t maxSteps = 0;
        uint32_t stepInterval = 0;
        int majorAxisIdx = -1;

        // 1. Find the Major Axis
        for (int i = 0; i < s.numMotors; i++) {
            if (s.steps[i] > maxSteps) {
                maxSteps = s.steps[i];
                majorAxisIdx = i;
            }
        }

        if (maxSteps > 0 && majorAxisIdx != -1) {
            if (s.sps[majorAxisIdx] > 0) {
                stepInterval = F_CPU / s.sps[majorAxisIdx]; // CPU cycles per step of the major axis
            }

            // 2. Initialize Error Counters for Minor Axes
            uint32_t error[MAX_MOTORS] = {0};
            for (int i = 0; i < s.numMotors; i++) {
                if (i != majorAxisIdx) {
                    error[i] = maxSteps / 2; // Start with half the max steps for symmetric Bresenham error
                }
            }

            // Set initial direction baseline byte
            uint8_t baseByte = 0;
            for (int i = 0; i < s.numMotors; i++) {
                if (s.nodeId[i] >= 1 && s.nodeId[i] <= 4) { // bounds check (Nodes 1 to 4)
                    if (s.cw[i]) {
                        baseByte |= (1 << (((s.nodeId[i] - 1) * 2) + 1)); // Set Dir bit for Node
                    }
                }
            }

            // 3. Bresenham Execution Loop
            uint32_t lastStepTime = rp2040.getCycleCount();
            for (uint32_t stepCount = 0; stepCount < maxSteps; stepCount++) {
                if (emergencyStop) return; // Exit function immediately!

                uint8_t outByte = baseByte;
                
                // The major axis ALWAYS steps
                if (s.nodeId[majorAxisIdx] >= 1 && s.nodeId[majorAxisIdx] <= 4) {
                    outByte |= (1 << (((s.nodeId[majorAxisIdx] - 1) * 2)));
                }

                // Calculate which minor axes need to step
                for (int i = 0; i < s.numMotors; i++) {
                    if (i != majorAxisIdx && s.steps[i] > 0) {
                        error[i] += s.steps[i];
                        if (error[i] >= maxSteps) {
                            error[i] -= maxSteps;
                            if (s.nodeId[i] >= 1 && s.nodeId[i] <= 4) {
                                outByte |= (1 << (((s.nodeId[i] - 1) * 2)));
                            }
                        }
                    }
                }

                // Wait for the exact time interval dictated by the major axis
                gpio_xor_mask(1u << 11);
                while ((rp2040.getCycleCount() - lastStepTime) < stepInterval) {
                    if (emergencyStop) return;
                }
                gpio_xor_mask(1u << 11); 
                lastStepTime += stepInterval;

                // Send the step packet (Stream Data -> 9th bit = 0)
                rs485.writeStream(outByte);
            }
        }

        // Advance buffer to the next queued segment!
        __dmb();
        mBufHead = (mBufHead + 1) % MASTER_BUF_SIZE;
    }
}

void loop1() {
    // 1. Reset on emergency stop
    if (emergencyStop) {
        mBufHead = mBufTail; // Instantly dump the buffer
        emergencyStop = false; // Reset flag after handling
        return;
    }

    // 2. Process the Streaming Motion Buffer entirely in SRAM!
    if (mBufHead != mBufTail) processStreamingBuffer();

    // 3. Process Core0 Requests via Hardware FIFO
    if (multicore_fifo_rvalid()) {
        uint32_t req = multicore_fifo_pop_blocking();
        uint8_t cmd = (req >> 8) & 0xFF;
        uint8_t node = req & 0xFF;
        
        while(!rs485.txEmpty());
        rs485.flushRX();
        rs485.writeStream(0); // NOP Stream Byte to reset parsers
        
        switch (cmd) {
            case CMD_PING: {
                uint8_t packet[4] = {node, CMD_PING, 0, 0};
                sendPacket(packet, 4);
                
                uint8_t rxLen = receivePacket(node, CMD_PONG, nullptr, RESPONSE_TIMEOUT_MS);
                uint32_t success = (rxLen != 0xFF) ? 1 : 0;
                multicore_fifo_push_blocking((CMD_PING << 24) | (node << 16) | success);
                break;
            }
            case CMD_GET_POS: {
                uint8_t packet[4] = {node, CMD_GET_POS, 0, 0};
                sendPacket(packet, 4);
                
                uint8_t payload[4];
                uint8_t rxLen = receivePacket(node, CMD_GET_POS, payload, RESPONSE_TIMEOUT_MS);
                
                uint32_t success = (rxLen == 4) ? 1 : 0;
                multicore_fifo_push_blocking((CMD_GET_POS << 24) | (node << 16) | success);
                if (success) {
                    int32_t pos = ((int32_t)payload[0] << 24) | ((int32_t)payload[1] << 16) | ((int32_t)payload[2] << 8) | payload[3];
                    multicore_fifo_push_blocking((uint32_t)pos);
                }
                break;
            }
            case CMD_ENABLE: {
                uint8_t packet[4] = {node, CMD_ENABLE, 0, 0};
                sendPacket(packet, 4);
                
                uint8_t rxLen = receivePacket(node, CMD_ENABLE, nullptr, RESPONSE_TIMEOUT_MS);
                uint32_t success = (rxLen != 0xFF) ? 1 : 0;
                multicore_fifo_push_blocking((CMD_ENABLE << 24) | (node << 16) | success);
                break;
            }
            case CMD_DISABLE: {
                uint8_t packet[4] = {node, CMD_DISABLE, 0, 0};
                sendPacket(packet, 4);
                
                uint8_t rxLen = receivePacket(node, CMD_DISABLE, nullptr, RESPONSE_TIMEOUT_MS);
                uint32_t success = (rxLen != 0xFF) ? 1 : 0;
                multicore_fifo_push_blocking((CMD_DISABLE << 24) | (node << 16) | success);
                break;
            }
        }
    } else {
        delayMicroseconds(10);
    }
}
