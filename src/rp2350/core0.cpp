// Core 0: Handles serial commands and production for the master buffer

#include <Arduino.h>
#include <math.h>
#include "shared.h"
#include "hardware/sync.h"

// ─── Local State for Core 0 ───────────────────────────────────────────────────
static char serialRxBuf[128];
static uint8_t serialRxLen = 0;
static bool bufWasFull = false;

// ─── Command Handlers ─────────────────────────────────────────────────────────

// 1. Control Commands (Volatile Flags - Instant Execution)
static bool handleControlCommand(const String& input) {
    if (input == "stop") {
        emergencyStop = true;
        Serial.println("!!! STOP DETECTED !!!");
        return true;
    } 
    else if (input.startsWith("unalarm")) {
        alarmTriggered = false;
        Serial.println("Alarm cleared!");
        return true;
    }
    return false;
}

// 2. Non-Streaming Commands (Multicore FIFO Queue)
static bool handleNonStreamingCommand(const String& input) {
    bool isCommand = input.startsWith("ping") || input.startsWith("enable") || 
                     input.startsWith("disable") || input.startsWith("getpos") || input.startsWith("suction");
    
    if (!isCommand) return false;

    // Check FIFO queue space once for all non-streaming commands
    if (!multicore_fifo_wready()) {
        Serial.println("Error: Command queue full.");
        return true; // Handled (by rejecting it)
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
        uint8_t targetNode = (uint8_t)strtoul(ptr, NULL, 10);
        if (targetNode >= 1 && targetNode <= 4) {
            Serial.printf("Queued PING for Node %d...\n", targetNode);
            multicore_fifo_push_blocking((CMD_PING << 8) | targetNode);
        } else Serial.println("Error: Invalid Node ID for ping.");
    }
    else if (input.startsWith("enable")) {
        char* ptr = (char*)input.c_str() + 6; 
        while (*ptr == ' ') ptr++; 
        uint8_t targetNode = (uint8_t)strtoul(ptr, NULL, 10);
        if (targetNode >= 1 && targetNode <= 4) {
            Serial.printf("Queued ENABLE for Node %d...\n", targetNode);
            multicore_fifo_push_blocking((CMD_ENABLE << 8) | targetNode);
        } else Serial.println("Error: Invalid Node ID for enable.");
    }
    else if (input.startsWith("disable")) {
        char* ptr = (char*)input.c_str() + 7; 
        while (*ptr == ' ') ptr++; 
        uint8_t targetNode = (uint8_t)strtoul(ptr, NULL, 10);
        if (targetNode >= 1 && targetNode <= 4) {
            Serial.printf("Queued DISABLE for Node %d...\n", targetNode);
            multicore_fifo_push_blocking((CMD_DISABLE << 8) | targetNode);
        } else Serial.println("Error: Invalid Node ID for disable.");
    }
    else if (input.startsWith("getpos")) {
        char* ptr = (char*)input.c_str() + 6; 
        while (*ptr == ' ') ptr++; 
        uint8_t targetNode = (uint8_t)strtoul(ptr, NULL, 10);
        if (targetNode >= 1 && targetNode <= 4) {
            Serial.printf("Queued Position Query for Node %d...\n", targetNode);
            multicore_fifo_push_blocking((CMD_GET_POS << 8) | targetNode);
        } else Serial.println("Error: Invalid Node ID for getpos.");
    }
    else if (input.startsWith("suction")) {
        Serial.println("ok");
    }
    
    return true;
}

// 3. Streaming Commands (Ring Buffer)
static bool handleStreamingCommand(const String& input) {
    if (!input.startsWith("move")) return false;

    if (alarmTriggered) {
        Serial.println("Error: Alarm triggered! Type 'unalarm' to continue.");
        return true;
    }

    uint8_t next = (mBufTail + 1) % MASTER_BUF_SIZE;
    if (next == mBufHead) {
        bufWasFull = true;
        Serial.println("nope");
        return true;
    }

    char* ptr = (char*)input.c_str() + 4; 
    while (*ptr == ' ') ptr++; 
    char* endPtr;

    uint8_t count = (uint8_t)strtoul(ptr, &endPtr, 10);
    if (ptr == endPtr || count == 0 || count > MAX_MOTORS) {
        Serial.printf("Error: Invalid motor count: %d\n", count);
        return true;
    }
    ptr = endPtr;

    Segment s; 
    s.numMotors = count;

    for (int i = 0; i < count; i++) {
        s.nodeId[i] = (uint8_t)strtoul(ptr, &endPtr, 10);
        if (ptr == endPtr) {
            Serial.printf("Error: Couldn't parse address for motor: %d\n", i);
            return true; 
        }
        ptr = endPtr;
    }

    for (int i = 0; i < count; i++) {
        long long val = strtoll(ptr, &endPtr, 10);
        if (ptr == endPtr) { 
            Serial.printf("Error: Couldn't parse steps for motor: %d\n", i);
            return true; 
        }
        ptr = endPtr;
        s.steps[i] = (uint32_t)llabs(val);
        s.cw[i] = (val < 0); 
    }

    // Parse the 4 global kinematic parameters
    s.v_entry = strtof(ptr, &endPtr);
    if (ptr == endPtr) { Serial.println("Error: Couldn't parse v_entry"); return true; }
    ptr = endPtr;
    
    s.v_cruise = strtof(ptr, &endPtr);
    if (ptr == endPtr) { Serial.println("Error: Couldn't parse v_cruise"); return true; }
    ptr = endPtr;
    
    s.v_exit = strtof(ptr, &endPtr);
    if (ptr == endPtr) { Serial.println("Error: Couldn't parse v_exit"); return true; }
    ptr = endPtr;
    
    s.accel = strtof(ptr, &endPtr);
    if (ptr == endPtr) { Serial.println("Error: Couldn't parse accel"); return true; }
    ptr = endPtr;

    // --- CRITICAL SECTION: Shared Memory Update ---
    masterBuf[mBufTail] = s;
    __dmb(); // Hardware Barrier
    mBufTail = next;
    
    Serial.println("ok");
    return true;
}

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

                if (handleControlCommand(input)) { /* Handled */ }
                else if (handleStreamingCommand(input)) { /* Handled */ }
                else if (handleNonStreamingCommand(input)) { /* Handled */ }
                else {
                    Serial.println("Error: Unknown command.");
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
    
    // Stateless Asynchronous FIFO Polling
    while (multicore_fifo_rvalid()) {
        uint32_t resp = multicore_fifo_pop_blocking();
        uint8_t cmd = (resp >> 24) & 0xFF;
        uint8_t node = (resp >> 16) & 0xFF;
        uint16_t success = resp & 0xFFFF;
        
        switch (cmd) {
            case CMD_PING:
                if (success) Serial.printf("Node %d: Received PONG!\n", node);
                else Serial.printf("Node %d: Timeout waiting for PONG.\n", node);
                break;
                
            case CMD_GET_POS:
                if (success) {
                    int32_t pos = (int32_t)multicore_fifo_pop_blocking();
                    Serial.printf("Node %d Position: %ld\n", node, pos);
                } else {
                    Serial.printf("Node %d: Timeout querying position.\n", node);
                }
                break;
                
            case CMD_ENABLE:
                if (success) {Serial.printf("Node %d: Enabled!\nok\n", node);}
                else Serial.printf("Node %d: Timeout enabling.\n", node);
                break;
                
            case CMD_DISABLE:
                if (success) Serial.printf("Node %d: Disabled!\nok\n", node);
                else Serial.printf("Node %d: Timeout disabling.\n", node);
                break;
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