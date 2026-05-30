// Core 0: Handles serial commands and production for the master buffer

#include <Arduino.h>
#include <math.h>
#include "shared.h"
#include "hardware/sync.h"

// ─── Local State for Core 0 ───────────────────────────────────────────────────
static char serialRxBuf[128];
static uint8_t serialRxLen = 0;
static bool bufWasFull = false;

static bool pingAllActive = false;
static uint8_t pingAllCurrentNode = 1;

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
                    Serial.println("!!! STOP DETECTED !!!");
                } 
                else if (input.startsWith("unalarm")) {
                    alarmTriggered = false;
                    Serial.println("Alarm cleared!");
                }
                else if (input.startsWith("ping all")) {
                    if (pingStatus == PING_PENDING || pingAllActive) {
                        Serial.println("Error: Ping already in progress.");
                    } else {
                        Serial.println("Starting PING ALL sequence...");
                        pingAllActive = true;
                        pingAllCurrentNode = 1;
                        pingStatus = PING_PENDING;
                        pendingPingNode = pingAllCurrentNode;
                        Serial.printf("Sending PING to Node %d...\n", pingAllCurrentNode);
                    }
                }
                else if (input.startsWith("ping")) {
                    char* ptr = (char*)input.c_str() + 4; 
                    while (*ptr == ' ') ptr++; 
                    uint8_t targetNode = (uint8_t)strtoul(ptr, NULL, 10);
                    
                    if (targetNode >= 1 && targetNode <= 4) {
                        if (pingStatus == PING_PENDING || pingAllActive) {
                            Serial.println("Error: Ping already in progress.");
                        } else {
                            Serial.printf("Sending PING to Node %d...\n", targetNode);
                            pingStatus = PING_PENDING;
                            pendingPingNode = targetNode;
                        }
                    } else {
                        Serial.println("Error: Invalid Node ID for ping.");
                    }
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

                    // 2. Parse Addresses (Node IDs 0-3)
                    for (int i = 0; i < count; i++) {
                        s.nodeId[i] = (uint8_t)strtoul(ptr, &endPtr, 10);
                        if (ptr == endPtr) {
                            Serial.print("Error: Couldn't parse address for motor: ");
                            Serial.println(i);
                            return; 
                        }
                        ptr = endPtr;
                    }

                    // 3. Parse Steps (Signed)
                    for (int i = 0; i < count; i++) {
                        long long val = strtoll(ptr, &endPtr, 10);
                        if (ptr == endPtr) { 
                            Serial.print("Error: Couldn't parse steps for motor: ");
                            Serial.println(i);
                            return; 
                        }
                        ptr = endPtr;
                        s.steps[i] = (uint32_t)llabs(val);
                        s.cw[i] = (val < 0); // Inverted: Positive is CCW, Negative is CW
                    }

                    // 4. Parse SPS
                    for (int i = 0; i < count; i++) {
                        s.sps[i] = (uint32_t)strtoul(ptr, &endPtr, 10);
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
    while (!Serial && millis() < 10000) {}

    Serial.printf("Custom RS485 Master (%d baud) [1-Byte Protocol]\n", RS485_BAUD);
}

static uint8_t getBufCount() {
    if (mBufTail >= mBufHead) return (mBufTail - mBufHead);
    return (MASTER_BUF_SIZE - mBufHead + mBufTail);
}

void loop() {
    // Check UI commands
    processSerial();
    
    // Check non-blocking ping status
    if (pingStatus == PING_OK) {
        Serial.println("Received PONG!");
        pingStatus = PING_IDLE;
        
        if (pingAllActive) {
            pingAllCurrentNode++;
            if (pingAllCurrentNode <= 4) {
                pingStatus = PING_PENDING;
                pendingPingNode = pingAllCurrentNode;
                Serial.printf("Sending PING to Node %d...\n", pingAllCurrentNode);
            } else {
                pingAllActive = false;
                Serial.println("PING ALL Complete.");
            }
        }
    } else if (pingStatus == PING_TIMEOUT) {
        Serial.println("Timeout waiting for PONG.");
        pingStatus = PING_IDLE;
        
        if (pingAllActive) {
            pingAllCurrentNode++;
            if (pingAllCurrentNode <= 4) {
                pingStatus = PING_PENDING;
                pendingPingNode = pingAllCurrentNode;
                Serial.printf("Sending PING to Node %d...\n", pingAllCurrentNode);
            } else {
                pingAllActive = false;
                Serial.println("PING ALL Complete.");
            }
        }
    }
    
    // Reply ready if buffer is cleared more than MASTER_BUF_LOW_WATERMARK 
    static uint8_t usedSlots = 0;
    if (bufWasFull) {
        usedSlots = getBufCount();
        bufWasFull = (usedSlots > MASTER_BUF_LOW_WATERMARK);
        if (!bufWasFull) Serial.println("ready");
    }
}