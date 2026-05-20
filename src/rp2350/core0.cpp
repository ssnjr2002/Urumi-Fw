// Core 0: Handles serial commands and production for the master buffer

#include <Arduino.h>
#include <math.h>
#include "shared.h"
#include "hardware/sync.h"

// ─── Local State for Core 0 ───────────────────────────────────────────────────
static char serialRxBuf[128];
static uint8_t serialRxLen = 0;
static bool bufWasFull = false;

// ─── Core 0 Serial & UI Logic ─────────────────────────────────────────────────

// Single, non-blocking serial parser to prevent buffer conflicts
void processSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialRxLen > 0) {
                serialRxBuf[serialRxLen] = '\0';
                String input = String(serialRxBuf);
                serialRxLen = 0; // Reset for next command

                if (input == "stop") {
                    emergencyStop = true;
                    // mBufHead = mBufTail; // Instantly clear producer's view of the buffer    
                    Serial.println("!!! STOP DETECTED !!!");
                } 
                else if (input.startsWith("enable")) {
                    char enStr[8] = {};
                    int enVal = 0;
                    sscanf(input.c_str(), "enable %7s %d", enStr, &enVal);
                    reqEnableAddr = (strcmp(enStr, "all") == 0) ? BROADCAST : (uint8_t)atoi(enStr);
                    reqEnableVal = (enVal != 0) ? 1 : 0; // Pass to Core 1
                    Serial.println("ok");
                    Serial.printf("Enable request sent: node %s = %d\n", enStr, enVal);
                }
                else if (input.startsWith("ping")) {
                    uint8_t addr = 0;
                    sscanf(input.c_str(), "ping %hhu", &addr);
                    reqPingAddr = addr; // Pass to Core 1
                    Serial.printf("Ping request sent to node %u...\n", addr);
                }
                else if (input.startsWith("unalarm")) {
                    alarmTriggered = false;
                    Serial.println("Alarm cleared!");
                }
                else if (input.startsWith("move")) {
                    if (alarmTriggered) {
                        Serial.println("Error: Alarm triggered! Type 'unalarm' to continue.");
                        return;
                    }

                    uint8_t next = (mBufTail + 1) % MASTER_BUF_SIZE;
                    if (next == mBufHead) {
                        bufWasFull = true;
                        Serial.println("nope");
                        return;
                    }

                    // Skip "move" and any leading spaces
                    char* ptr = (char*)input.c_str() + 4; 
                    while (*ptr == ' ') ptr++; 
                    char* endPtr;

                    // 1. Parse Number of Motors
                    uint8_t count = (uint8_t)strtoul(ptr, &endPtr, 10);
                    if (ptr == endPtr || count == 0 || count > MAX_MOTORS) {
                        Serial.print("Error: Invalid motor count: ");
                        Serial.println(count);
                        return;
                    }
                    ptr = endPtr;

                    // LOCAL struct to build the command. 
                    Segment s; 
                    s.numMotors = count;

                    // 2. Parse Addresses
                    for (int i = 0; i < count; i++) {
                        s.addr[i] = (uint8_t)strtoul(ptr, &endPtr, 10);
                        if (ptr == endPtr) {
                            Serial.print("Error: Couldn't parse address for motor: ");
                            Serial.println(i);
                            return; 
                        }
                        ptr = endPtr;
                    }

                    // 3. Parse Steps (Signed)
                    for (int i = 0; i < count; i++) {
                        long val = strtol(ptr, &endPtr, 10);
                        if (ptr == endPtr) { 
                            Serial.print("Error: Couldn't parse steps for motor: ");
                            Serial.println(i);
                            return; 
                        }
                        ptr = endPtr;
                        s.steps[i] = (uint16_t)abs(val);
                        s.cw[i] = (val < 0);
                    }

                    // 4. Parse SPS
                    for (int i = 0; i < count; i++) {
                        s.sps[i] = (uint16_t)strtoul(ptr, &endPtr, 10);
                        if (ptr == endPtr) { 
                            Serial.print("Error: Couldn't parse sps for motor: ");
                            Serial.println(i);
                            return; 
                        }
                        ptr = endPtr;
                    }

                    // --- CRITICAL SECTION: Shared Memory Update ---
                    
                    // Copy the entire validated struct to shared memory at once
                    masterBuf[mBufTail] = s;

                    // Hardware Barrier: Ensure all data is physically in RAM 
                    // before the tail index is updated.
                    __dmb(); 
                    
                    mBufTail = next;
                    
                    // Data Synchronization Barrier: Ensure the tail update 
                    // is visible to Core 1 immediately.
                    __dsb(); // is this necessary? 

                    Serial.println("ok");
                }
            }
        } 
        else if (serialRxLen < sizeof(serialRxBuf) - 1) {
            serialRxBuf[serialRxLen++] = c;
        }
    }
}


// ─── Core 0 Setup & Loop ──────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    // Optional: wait for serial, but keep it brief
    while (!Serial && millis() < 10000) {}

    // Init the RS485 enable pin (Core 1 handles UART tx/rx pins)
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, LOW);

    Serial.printf("Custom RS485 Master (%d baud)\n", RS485_BAUD);
}

static uint8_t getBufCount() {
    if (mBufTail >= mBufHead) return (mBufTail - mBufHead);
    return (MASTER_BUF_SIZE - mBufHead + mBufTail);
}

void loop() {
    // Check UI commands
    processSerial();
    
    // Reply ready if buffer is cleared more than MASTER_BUF_LOW_WATERMARK 
    static uint8_t usedSlots = 0;
    if (bufWasFull) {
        usedSlots = getBufCount();
        bufWasFull = (usedSlots > MASTER_BUF_LOW_WATERMARK);
        // Serial.printf("Used slots: %d, low watermark: %d\n", usedSlots, MASTER_BUF_LOW_WATERMARK);
        if (!bufWasFull) Serial.println("ready");
    }

    // Process asynchronous UI responses from Core 1 safely
    if (pingResult != -1) {
        Serial.printf("Ping response: %s\n", pingResult == 1 ? "OK" : "TIMEOUT");
        pingResult = -1; // Reset
    }

    if (failedNode > 0) {
        Serial.printf("Failed Node: %d\n", failedNode);
        failedNode = 0;
    }
}