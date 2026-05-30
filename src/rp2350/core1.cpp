// Core 1: Handles the real-time RS485 byte streaming

#include <Arduino.h>
#include "shared.h"
#include "hardware/gpio.h"

// ─── Core 1 Setup & Loop (RS485 Engine) ───────────────────────────────────────

void setup1() {
    Serial2.setTX(RS485_TX_PIN);
    Serial2.setRX(RS485_RX_PIN);
    
    // Configure UART with 2 Stop Bits for maximum frame-alignment reliability
    Serial2.begin(RS485_BAUD, SERIAL_8N2);
    
    // Set RS485 transceiver to continuous TX mode
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, HIGH); 
    pinMode(11, OUTPUT);
}

void loop1() {
    // 1. Check for Emergency Stop immediately
    if (emergencyStop) {
        mBufHead = mBufTail; // Instantly dump the buffer
        emergencyStop = false; // Reset flag after handling
        return;
    }

    // 2. Process the Streaming Motion Buffer
    if (mBufHead != mBufTail) {
        volatile Segment &s = masterBuf[mBufHead];

        // --- Real-Time Streaming Execution (Spatial Bresenham) ---
        
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
                if (emergencyStop) return; // Exit loop immediately

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

                // Send the step packet
                // Blocks if TX FIFO is full, implicitly throttling step rate to baud rate
                Serial2.write(outByte);
            }
        }

        // Advance buffer
        __dmb();
        mBufHead = (mBufHead + 1) % MASTER_BUF_SIZE;
    } else { 
        // Tiny yield if no commands
        delayMicroseconds(10); 
    }
}
