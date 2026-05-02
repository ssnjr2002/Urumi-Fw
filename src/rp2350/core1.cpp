// Core 1: Handles the RS485 communication and syncing motors

#include <Arduino.h>
#include "shared.h"

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
    if (!waitResp) return 0;

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

void cmdQueueSlave1(uint8_t addr, bool cw, uint16_t steps, uint16_t sps) {
    uint8_t p[5];
    p[0] = cw ? 0u : 1u;
    p[1] = (uint8_t)steps; p[2] = (uint8_t)(steps >> 8);
    p[3] = (uint8_t)sps;   p[4] = (uint8_t)(sps   >> 8);
    sendCmd1(addr, CMD_QUEUE, p, 5, nullptr, true); // Wait for ACK to ensure sync
}

// ─── Core 1 Setup & Loop (RS485 Engine) ───────────────────────────────────────

void setup1() {
    Serial2.setTX(RS485_TX_PIN);
    Serial2.setRX(RS485_RX_PIN);
    Serial2.begin(RS485_BAUD);
}

void loop1() {
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
        SlaveStatus sX, sY, sZ, sA;
        // Poll all four axes. X and Y are mandatory; Z and A are optional —
        // if a node isn't on the bus its status check simply returns false and
        // we skip it entirely (no 20 ms timeout wasted per segment).
        bool okX = getStatus1(NODE_X, sX);
        bool okY = getStatus1(NODE_Y, sY);
        bool okZ = getStatus1(NODE_Z, sZ);
        bool okA = getStatus1(NODE_A, sA);

        // X and Y are required — abort this cycle if either is missing
        if (!okX || !okY) {
            delayMicroseconds(10);
            return;
        }

        // groupFree = tightest free-slot count across every connected node.
        // All axes must have room before we push anything, so that a segment
        // queued to X/Y is always matched by Z/A arriving in the same slot.
        uint8_t groupFree = min(sX.free, sY.free);
        if (okZ) groupFree = min(groupFree, sZ.free);
        if (okA) groupFree = min(groupFree, sA.free);

        // groupIdle = every connected node has finished its last move.
        // CMD_GO via broadcast restarts all of them simultaneously.
        bool groupIdle = (sX.running == 0 && sY.running == 0);
        if (okZ) groupIdle = groupIdle && (sZ.running == 0);
        if (okA) groupIdle = groupIdle && (sA.running == 0);

        // Drain Core 0's buffer into the physical slaves
        while (groupFree > (SLAVE_BUF_SIZE - SLAVE_BUF_TARGET) && mBufHead != mBufTail) {

            // Read from Ring Buffer
            Segment s = masterBuf[mBufHead];

            // Memory Barrier ensures struct is read fully before head advances
            __asm__ volatile ("" ::: "memory");

            // Dispatch each axis only if the node is present and has steps to move.
            // X and Y are always present (guarded above). Z and A are skipped when
            // their node didn't respond to the status check, keeping the bus clean.
            if (s.xSteps != 0)        cmdQueueSlave1(NODE_X, s.xCw, abs(s.xSteps), s.xSps);
            if (s.ySteps != 0)        cmdQueueSlave1(NODE_Y, s.yCw, abs(s.ySteps), s.ySps);
            if (s.zSteps != 0 && okZ) cmdQueueSlave1(NODE_Z, s.zCw, abs(s.zSteps), s.zSps);
            if (s.aSteps != 0 && okA) cmdQueueSlave1(NODE_A, s.aCw, abs(s.aSteps), s.aSps);

            // Advance head
            mBufHead = (mBufHead + 1) % MASTER_BUF_SIZE;
            groupFree--;

            // If all connected engines were halted, kick them off together
            if (groupIdle) {
                sendCmd1(BROADCAST, CMD_GO, nullptr, 0, nullptr, false);
                groupIdle = false;
            }
        }
    }
    
    // Tiny yield to prevent watchdog timeouts and allow core sync
    delayMicroseconds(10); 
}
