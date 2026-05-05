// Core 0: Handles serial commands and production for the master buffer

#include <Arduino.h>
#include <math.h>
#include "shared.h"

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
                    Serial.printf("Enable request sent: node %s = %d\n", enStr, enVal);
                }
                else if (input.startsWith("ping")) {
                    uint8_t addr = 0;
                    sscanf(input.c_str(), "ping %hhu", &addr);
                    reqPingAddr = addr; // Pass to Core 1
                    Serial.printf("Ping request sent to node %u...\n", addr);
                }
                else if (input.startsWith("xyz")) {
                    uint8_t next = (mBufTail + 1) % MASTER_BUF_SIZE;
                    if (next == mBufHead){
                        bufWasFull = true;
                        Serial.println("nope");
                        return;
                    }
                    Segment *s = &masterBuf[mBufTail];
                    int8_t parsed = sscanf(input.c_str(), 
                        "xyz %hd         %hd         %hd         %hu       %hu       %hu       %hd", 
                             &s->xSteps, &s->ySteps, &s->zSteps, &s->xSps, &s->ySps, &s->zSps, &s->aSteps
                    );
                    if (parsed < 6) {
                        Serial.println("parse error");
                        return;
                    }
                    s->xCw = (s->xSteps < 0);
                    s->yCw = (s->ySteps < 0);
                    s->zCw = (s->zSteps < 0);
                    s->aCw = (s->aSteps < 0);
                    s->aSps = 1024;
                    
                    // Memory barrier ensures struct is fully written BEFORE advancing the tail.
                    // Critical for dual-core RP2350 stability.
                    __asm__ volatile ("dmb" ::: "memory"); 
                    mBufTail = next;
                    __asm__ volatile ("dsb" ::: "memory"); 
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

    Serial.println("Custom RS485 Master (230400 baud)");
}

void loop() {
    // Check UI commands
    processSerial();

    // Reply ready if buffer is cleared more than MASTER_BUF_LOW_WATERMARK 
    if (bufWasFull) {
        int16_t diff = (int16_t)mBufTail - (int16_t)mBufHead;
        if (diff < 0) diff += MASTER_BUF_SIZE;
        uint8_t usedSlots = (uint8_t)diff;
        bufWasFull = (usedSlots < MASTER_BUF_LOW_WATERMARK);
        Serial.print("Used slots: ");
        Serial.println(usedSlots);
        if (!bufWasFull) Serial.println("ready");
    }

    // Process asynchronous UI responses from Core 1 safely
    if (pingResult != -1) {
        Serial.printf("Ping response: %s\n", pingResult == 1 ? "OK" : "TIMEOUT");
        pingResult = -1; // Reset
    }
}