// Core 0: Handles serial commands and production for the master buffer

#include <Arduino.h>
#include <math.h>
#include "shared.h"

// ─── Local State for Core 0 ───────────────────────────────────────────────────
static char serialRxBuf[64];
static uint8_t serialRxLen = 0;

// Forward declarations
void processSerial();
bool checkStop();
bool queueCoreXY0(int16_t dx, int16_t dy, uint16_t sps);
void drawCircle(float rMm, float spdMmS, uint16_t segs);

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
                    mBufHead = mBufTail; // Instantly clear producer's view of the buffer
                    Serial.println("!!! STOP DETECTED !!!");
                } 
                else if (input.startsWith("ping")) {
                    uint8_t addr = 0;
                    sscanf(input.c_str(), "ping %hhu", &addr);
                    reqPingAddr = addr; // Pass to Core 1
                    Serial.printf("Ping request sent to node %u...\n", addr);
                }
                else if (input.startsWith("enable")) {
                    char enStr[8] = {};
                    int enVal = 0;
                    sscanf(input.c_str(), "enable %7s %d", enStr, &enVal);
                    reqEnableAddr = (strcmp(enStr, "all") == 0) ? BROADCAST : (uint8_t)atoi(enStr);
                    reqEnableVal = (enVal != 0) ? 1 : 0; // Pass to Core 1
                    Serial.printf("Enable request sent: node %s = %d\n", enStr, enVal);
                }
                else if (input.startsWith("xy")) {
                    int16_t  dx = 0, dy = 0;
                    uint16_t sps = 0;
                    sscanf(input.c_str(), "xy %hd %hd %hu", &dx, &dy, &sps);
                    if (sps == 0) sps = (uint16_t)(DEFAULT_SPD_MM_S * STEPS_PER_MM);
                    queueCoreXY0(dx, dy, sps);
                }
                else if (input.startsWith("circle")) {
                    float r, s; int segs;
                    sscanf(input.c_str(), "circle %f %f %d", &r, &s, &segs);
                    drawCircle(r, s, segs);
                }
            }
        } 
        else if (serialRxLen < sizeof(serialRxBuf) - 1) {
            serialRxBuf[serialRxLen++] = c;
        }
    }
}

// ─── Trajectory & Buffer Management ───────────────────────────────────────────
bool checkStop() {
    if (emergencyStop) return true; // Already stopping
    processSerial();                // Read inputs while blocked
    return emergencyStop;
}

bool queueCoreXY0(int16_t dx, int16_t dy, uint16_t sps) {
    uint8_t next = (mBufTail + 1) % MASTER_BUF_SIZE;

    // While buffer is full, keep checking for the stop command
    while (next == mBufHead) {
        if (checkStop()) return false;
        delay(1);
    }
    if (emergencyStop) return false;

    // CoreXY Kinematics translation
    // int32_t xSteps = (int32_t)dx + dy;
    // int32_t ySteps = (int32_t)dx - dy;
    // Cartesian
    int32_t aSteps = dx;
    int32_t bSteps = dy;
    float maxM = max(abs(aSteps), abs(bSteps));
    if (maxM == 0) return true;

    Segment s;
    s.aSteps = (int16_t)aSteps; 
    s.bSteps = (int16_t)bSteps;
    s.aCw    = (aSteps >= 0); 
    s.bCw    = (bSteps >= 0);
    
    // Explicit float cast to prevent integer division (0) truncation
    s.aSps = (uint16_t)(sps * ((float)abs(aSteps) / maxM));
    s.bSps = (uint16_t)(sps * ((float)abs(bSteps) / maxM));
    
    // Enforce minimum speeds
    if (s.aSps < 10 && s.aSteps != 0) s.aSps = 10;
    if (s.bSps < 10 && s.bSteps != 0) s.bSps = 10;

    // Push to buffer
    masterBuf[mBufTail] = s;
    
    // Memory barrier ensures struct is fully written BEFORE advancing the tail.
    // Critical for dual-core RP2350 stability.
    __asm__ volatile ("" ::: "memory"); 
    
    mBufTail = next;
    return true;
}

void drawCircle(float rMm, float spdMmS, uint16_t segs) {
    segs = constrain(segs, 4, 1024);
    Serial.printf("Circle r=%.1f mm  @ %.1f mm/s  %d segs\n", rMm, spdMmS, segs);

    float    rSteps = rMm * STEPS_PER_MM;
    uint16_t sps    = (uint16_t)(spdMmS * STEPS_PER_MM);
    float    px = rSteps, py = 0.0f, ex = 0.0f, ey = 0.0f;

    // Move to starting edge
    // if (!queueCoreXY0((int16_t)rSteps, 0, sps)) return; 

    // Segmented circular path
    for (uint16_t i = 1; i <= segs; i++) {
        float   a   = 2.0f * (float)M_PI * i / segs;
        float   nx  = rSteps * cosf(a);
        float   ny  = rSteps * sinf(a);
        
        // Bresenham-style float error tracking
        float   rdx = nx - px + ex;
        float   rdy = ny - py + ey;
        int16_t dx  = (int16_t)roundf(rdx);
        int16_t dy  = (int16_t)roundf(rdy);
        
        ex = rdx - (float)dx;
        ey = rdy - (float)dy;
        
        if (dx != 0 || dy != 0) {
            if (!queueCoreXY0(dx, dy, sps)) return;
        }
        px = nx; py = ny;
    }

    // Return to center
    // if (!queueCoreXY0(-(int16_t)rSteps, 0, sps)) return; 
    Serial.println("Circle done.");
}

// ─── Core 0 Setup & Loop ──────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    // Optional: wait for serial, but keep it brief
    while (!Serial && millis() < 10000) {}

    // Init the RS485 enable pin (Core 1 handles UART tx/rx pins)
    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, LOW);

    Serial.println("Custom RS485 Master  (230400 baud)");
    Serial.printf("STEPS_PER_MM = %.1f  (MICROSTEP=%d)\n", (float)STEPS_PER_MM, MICROSTEP);
    Serial.println("─────────────────────────────────────────");
    Serial.println("  ping <addr>                           ");
    Serial.println("  enable <addr|all> <0|1>               ");
    Serial.println("  stop                                  ");
    Serial.println("  xy <dx> <dy> [sps]  (steps, signed)   ");
    Serial.println("  circle <R_mm> [speed_mm_s] [segs]     ");
    Serial.println("─────────────────────────────────────────");
}

void loop() {
    // 1. Check UI commands
    processSerial();

    // 2. Process asynchronous UI responses from Core 1 safely
    if (pingResult != -1) {
        Serial.printf("Ping response: %s\n", pingResult == 1 ? "OK" : "TIMEOUT");
        pingResult = -1; // Reset
    }
}