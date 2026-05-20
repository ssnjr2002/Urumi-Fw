// Core 1: Handles the RS485 communication and syncing motors

#include <Arduino.h>
#include "shared.h"

#define OSCOPE_PROBING
#define OSCOPE_PIN 11 // Physically Pin 15 (GPIO11)
#define OSCOPE_PERIOD_MS 4

// ─── RS485 & Protocol Helpers (Local to Core 1) ───────────────────────────────

static uint16_t crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

static void rs485Send(const uint8_t *d, uint8_t len) {
    digitalWrite(RS485_EN_PIN, HIGH);
    delayMicroseconds(5); // Give transceiver time to switch to TX
    
    Serial2.write(d, len);
    Serial2.flush(); // Waits for TX FIFO buffer to empty

    // IMPORTANT HARDWARE FIX: 
    // On many RP2040/RP2350 Arduino cores, flush() returns when the FIFO is empty, 
    // but the final byte is still moving through the hardware UART shift register.
    // At 230400 baud, 1 byte (10 bits) takes ~43.4 microseconds. 
    // We wait 50us to ensure the last bit actually hits the wire before dropping the EN pin.
    delayMicroseconds(50);
    digitalWrite(RS485_EN_PIN, LOW);
}

// Read one response frame. Returns total bytes received (0 on timeout/error).
static uint8_t rs485Recv(uint8_t *buf, uint8_t maxLen) {
    uint32_t deadline = millis() + RESPONSE_TIMEOUT_MS;
    uint8_t  idx = 0;
    bool     act = false;
    uint8_t  exp = 0;

    // Loop until deadline is passed
    while ((int32_t)(millis() - deadline) < 0) {
        if (!Serial2.available()) continue;
        uint8_t b = (uint8_t)Serial2.read();
        
        if (!act) {
            if (!(b & 1u)) continue;          // Size bytes must have LSB=1
            uint8_t n = b >> 1;
            if (n < 5u || (uint8_t)(1u + n) > maxLen) continue;
            buf[0] = b; idx = 1; exp = 1u + n; act = true;
        } else {
            if (idx < maxLen) buf[idx++] = b;
            if (idx >= exp) break;
        }
    }
    return idx;
}

// Builds and sends a request. If waitResp=true, receives and validates response.
// Returns number of response DATA bytes (after STATUS) on success, 0 on failure.
static uint8_t sendCmd1(uint8_t addr, uint8_t cmd, const uint8_t *pay, uint8_t payLen, uint8_t *outData, bool waitResp = true) {
    uint8_t buf[32];
    uint8_t n = 0;
    uint8_t afterSize = 1u + 1u + payLen + 2u; // addr + cmd + pay + crc16
    
    buf[n++] = (uint8_t)((afterSize << 1) | 1u);
    buf[n++] = addr;
    buf[n++] = cmd;
    for (uint8_t i = 0; i < payLen; i++) buf[n++] = pay[i];
    
    uint16_t crc = crc16(buf, n);
    buf[n++] = (uint8_t)(crc & 0xFF);
    buf[n++] = (uint8_t)(crc >> 8);

    rs485Send(buf, n);
    if (!waitResp) return 0; // TODO: add not waiting if addr is broadcast? 

    uint8_t resp[32];
    uint8_t rlen = rs485Recv(resp, sizeof(resp));
    if (rlen < 5u) return 0;

    uint16_t calc = crc16(resp, rlen - 2);
    uint16_t got = (uint16_t)resp[rlen-2] | ((uint16_t)resp[rlen-1] << 8);
    
    if (calc != got || resp[1] != RESP_CHAR || resp[2] == STATUS_ERR) return 0;

    uint8_t dlen = rlen - 5u;
    if (outData && dlen > 0) memcpy(outData, &resp[3], dlen);
    return dlen ? dlen : 1;   // Return 1 for STATUS_OK with no data payload
}

// ─── Node Communication Helpers ───────────────────────────────────────────────

struct SlaveStatus { uint8_t running, used, free; uint16_t stepsRem; };

bool getStatus1(uint8_t addr, SlaveStatus &s) {
    uint8_t d[5] = {0};
    if (sendCmd1(addr, CMD_STATUS, nullptr, 0, d, true) < 5) return false;
    s.running = d[0]; 
    s.used    = d[1]; 
    s.free    = d[2];
    s.stepsRem = (uint16_t)d[3] | ((uint16_t)d[4] << 8);
    return true;
}

bool cmdQueueSlave1(uint8_t addr, bool cw, uint16_t steps, uint16_t sps, SlaveStatus &outStatus) {
    uint8_t p[5];
    p[0] = cw ? 0u : 1u;
    p[1] = (uint8_t)steps; p[2] = (uint8_t)(steps >> 8);
    p[3] = (uint8_t)sps;   p[4] = (uint8_t)(sps >> 8);
    
    // TODO: update Slave AVR's CMD_QUEUE to respond with these 3 bytes 
    // instead of just 1 byte, exactly like CMD_STATUS does).
    uint8_t respData[3] = {0};
    uint8_t len = sendCmd1(addr, CMD_QUEUE, p, 5, respData, true);
    
    if (len >= 3) {
        outStatus.running = respData[0];
        outStatus.used    = respData[1];
        outStatus.free    = respData[2];
        return true;
    }
    return false; // Comm failure
}

void cmdDummyQueueSlave1(uint8_t numMotors, const uint8_t* addr, bool cw, uint16_t steps, uint16_t sps) {
    uint8_t pLen = 6 + numMotors;
    uint8_t p[pLen];

    p[0] = numMotors; // Motor exclusion count
    
    // Copy the exclusion addresses
    for (int i = 0; i < numMotors; i++) {
        p[1 + i] = addr[i]; 
    }
    
    // Append the dummy segment parameters
    uint8_t offset = 1 + numMotors;
    p[offset]     = cw ? 0u : 1u;
    p[offset + 1] = (uint8_t)steps; 
    p[offset + 2] = (uint8_t)(steps >> 8);
    p[offset + 3] = (uint8_t)sps;   
    p[offset + 4] = (uint8_t)(sps >> 8);

    sendCmd1(BROADCAST, CMD_QUEUE_DUMMY, p, pLen, nullptr, false);
}

// ─── Core 1 Setup & Loop (RS485 Engine) ───────────────────────────────────────

#ifdef OSCOPE_PROBING
    static struct repeating_timer myScopeTimer;
    bool timerRunning = false;

    // The callback function that toggles the pin for the oscilloscope
    bool timer_callback(struct repeating_timer *t) {
        digitalWrite(OSCOPE_PIN, !digitalRead(OSCOPE_PIN));
        return true; 
    }

    void start_waveform() {
        if (!timerRunning) {
            add_repeating_timer_ms(OSCOPE_PERIOD_MS, timer_callback, NULL, &myScopeTimer);
            timer_callback(&myScopeTimer);
            timerRunning = true;
        }
    }

    void stop_waveform() {
        if (timerRunning) {
            // Stop and destroy the active timer
            cancel_repeating_timer(&myScopeTimer);
            timerRunning = false;
            
            // Force the pin LOW so the scope shows a flatline baseline
            digitalWrite(OSCOPE_PIN, LOW); 
        }
    }
#endif

void setup1() {
    Serial2.setTX(RS485_TX_PIN);
    Serial2.setRX(RS485_RX_PIN);
    Serial2.begin(RS485_BAUD);
    
    #ifdef OSCOPE_PROBING
        pinMode(OSCOPE_PIN, OUTPUT);
        start_waveform();
    #endif
}

void loop1() {
    static uint8_t globalFreeSpace = SLAVE_BUF_SIZE;
    static uint32_t lastPollTime = 0;

    // 1. Check for Emergency Stop immediately
    if (emergencyStop) {
        sendCmd1(BROADCAST, CMD_STOP, nullptr, 0, nullptr, false);
        mBufHead = mBufTail; // Instantly dump the buffer
        emergencyStop = false; // Reset flag after handling
        return;
    }

    // 2. Process synchronous UI requests from Core 0
    if (reqPingAddr != 0) {
        uint8_t d[1];
        bool ok = sendCmd1(reqPingAddr, CMD_PING, nullptr, 0, d, true) > 0;
        pingResult = ok ? 1 : 0; // Return result to Core 0 safely
        reqPingAddr = 0;         // Clear request flag
    }
    
    if (reqEnableVal != -1) {
        uint8_t p = (reqEnableVal != 0) ? 1u : 0u;
        sendCmd1(reqEnableAddr, CMD_ENABLE, &p, 1, nullptr, (reqEnableAddr != BROADCAST));
        reqEnableVal = -1;       // Clear request flag
    }
    
    // 3. Process the Streaming Motion Buffer
    if (mBufHead != mBufTail) {
        if (globalFreeSpace > (SLAVE_BUF_SIZE - SLAVE_BUF_TARGET)) {
            volatile Segment &s = masterBuf[mBufHead];

            // 1 & 2. Calculate Max Duration AND Send Unicasts
            uint8_t mIdx = 0; // store index of longest node
            float maxDur = 0.0f;
            uint8_t lowestReportedFree = SLAVE_BUF_SIZE;
            bool anyIdle = false; 

            for (int i = 0; i < s.numMotors; i++) {
                // A. Find longest move
                if (s.sps[i] > 0) {
                    float d = (float)s.steps[i] / (float)s.sps[i];
                    if (d > maxDur) {
                        maxDur = d;
                        mIdx = i;
                    }
                }

                // B. Queue command & get status
                SlaveStatus ss;
                if (cmdQueueSlave1(s.addr[i], s.cw[i], s.steps[i], s.sps[i], ss)) {
                    if (ss.free < lowestReportedFree) lowestReportedFree = ss.free;
                    if (ss.running == 0) anyIdle = true;
                } else {
                    // Critical failure handling. A targeted node disconnected.
                    failedNode = s.addr[i];
                    sendCmd1(BROADCAST, CMD_STOP, nullptr, 0, nullptr, false);
                    emergencyStop = true;
                    alarmTriggered = true;
                    return;
                }
            }

            // Update local tracking based on worst-case node
            globalFreeSpace = lowestReportedFree;

            // 3. Send Broadcast Dummy to the rest of the nodes
            // They will clone the steps/sps of the longest move (mIdx)
            cmdDummyQueueSlave1(s.numMotors, (const uint8_t*)s.addr, s.cw[mIdx], s.steps[mIdx], s.sps[mIdx]);

            // 4. Trigger & Advance
            if (anyIdle) {
                stop_waveform();
                delayMicroseconds(1000);
                sendCmd1(BROADCAST, CMD_GO, nullptr, 0, nullptr, false);
                start_waveform();
            }

            __dmb();
            mBufHead = (mBufHead + 1) % MASTER_BUF_SIZE;

        } else { 
            // THE WAIT PATH: Buffers are full.
            if (millis() - lastPollTime >= POLL_INTERVAL_MS) {
                lastPollTime = millis();
                
                volatile Segment &sPeek = masterBuf[mBufHead];
                SlaveStatus ss;
                
                // Poll just the first active node
                if (getStatus1(sPeek.addr[0], ss)) {
                    globalFreeSpace = ss.free;
                }
            }
        }
    }
    
    // Tiny yield to prevent watchdog timeouts and allow core sync
    delayMicroseconds(10); 
}
