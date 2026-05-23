// Core 1: Handles the real-time RS485 byte streaming

#include <Arduino.h>
#include "shared.h"

// ─── Core 1 Setup & Loop (RS485 Engine) ───────────────────────────────────────

void setup1() {
    Serial2.setTX(RS485_TX_PIN);
    Serial2.setRX(RS485_RX_PIN);
    
    // Configure UART with 2 Stop Bits for maximum frame-alignment reliability
    Serial2.begin(RS485_BAUD, SERIAL_8N2);
    
    // Set RS485 transceiver to continuous TX mode
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, HIGH); 
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

        // --- Real-Time Streaming Execution ---
        
        uint32_t currentSteps[MAX_MOTORS] = {0};
        uint32_t lastStepTime[MAX_MOTORS] = {0};
        uint32_t stepInterval[MAX_MOTORS] = {0};
        bool anyStepsRemaining = false;

        for (int i = 0; i < s.numMotors; i++) {
            if (s.steps[i] > 0 && s.sps[i] > 0) {
                anyStepsRemaining = true;
                stepInterval[i] = 1000000 / s.sps[i]; // Microseconds per step
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

        uint32_t tStart = micros();
        for (int i = 0; i < s.numMotors; i++) {
            lastStepTime[i] = tStart;
        }

        while (anyStepsRemaining) {
            if (emergencyStop) return; // Exit loop immediately

            uint32_t now = micros();
            bool stepNow = false;
            
            // Start with the direction bits
            uint8_t outByte = baseByte; 
            
            anyStepsRemaining = false;

            for (int i = 0; i < s.numMotors; i++) {
                if (currentSteps[i] < s.steps[i]) {
                    anyStepsRemaining = true;
                    if ((now - lastStepTime[i]) >= stepInterval[i]) {
                        if (s.nodeId[i] >= 1 && s.nodeId[i] <= 4) {
                            outByte |= (1 << (((s.nodeId[i] - 1) * 2))); // Set Step bit
                        }
                        currentSteps[i]++;
                        lastStepTime[i] += stepInterval[i];
                        stepNow = true;
                    }
                }
            }

            if (stepNow) {
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
