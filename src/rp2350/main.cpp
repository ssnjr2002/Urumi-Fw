// PICO_Custom_Master.ino
// Raspberry Pi Pico 2 — RS485 master matching ATTINY_Custom_Slave protocol
//
// Frame format (request):
//   [SIZE] [ADDR] [CMD] [PAYLOAD little-endian] [CRC32 LE]
//   SIZE = ((bytes_after_SIZE) << 1) | 1   — LSB always 1
//   Broadcast addr = 0xFF (all execute, none respond)
//
// Frame format (response):
//   [SIZE] [0xFD] [STATUS] [DATA LE] [CRC32 LE]
//   STATUS: 0x00=ok/no data  0x01=ok/data  0xFF=error
//
// Commands (match slave exactly):
//   0x01 CMD_PING    →  resp: node_id(1)
//   0x02 CMD_QUEUE   →  pay: dir(1) steps(2 LE) speed(2 LE)
//                        resp: buf_free(1)
//   0x03 CMD_GO      →  broadcast: arms → running simultaneously
//   0x04 CMD_STOP    →  broadcast: immediate stop + clear buffer
//   0x05 CMD_STATUS  →  resp: running(1) buf_used(1) buf_free(1)
//   0x06 CMD_ENABLE  →  pay: enable(1)
//
// Wiring (SP3485EN):
//   GP4 TX → DI,  GP5 RX ← RO,  GP6 → DE+/RE
//
// CoreXY node map:
//   Node 1 = Motor A,  Node 2 = Motor B,  Node 3 = Z (pen)
//   Motor A steps = dx + dy
//   Motor B steps = dx - dy
//
// USB commands:
//   ping <addr>
//   enable <addr|all> <0|1>
//   stop
//   <addr> f/b <steps> [sps]            — single-axis jog
//   xy <dx> <dy> [sps]                   — CoreXY in steps (signed)
//   rect <W_mm> <H_mm> [speed_mm_s]     — draw rectangle
//   circle <R_mm> [speed_mm_s] [segs]   — draw circle

#include <Arduino.h>
#include <math.h>

// ─── Pins ──────────────────────────────────────────────────────────────────────
#define RS485_TX_PIN  4
#define RS485_RX_PIN  5
#define RS485_EN_PIN  6

// ─── Bus ───────────────────────────────────────────────────────────────────────
#define RS485_BAUD          230400
#define RESPONSE_TIMEOUT_MS    80

// ─── Protocol ──────────────────────────────────────────────────────────────────
#define BROADCAST    0xFF
#define RESP_CHAR    0xFD

#define STATUS_OK    0x00
#define STATUS_DATA  0x01
#define STATUS_ERR   0xFF

#define CMD_PING     0x01
#define CMD_QUEUE    0x02
#define CMD_GO       0x03
#define CMD_STOP     0x04
#define CMD_STATUS   0x05
#define CMD_ENABLE   0x06

// ─── Machine config ────────────────────────────────────────────────────────────
#define NODE_A  1
#define NODE_B  2
#define NODE_Z  3

// GT2 belt + 20-tooth pulley
// Set MICROSTEP to match your DRV8825 jumpers (1/2/4/8/16/32)
#define BELT_PITCH_MM   2.0f
#define PULLEY_TEETH    20
#define MOTOR_FULL_SPS  200
#define MICROSTEP       1
#define MM_PER_REV      (BELT_PITCH_MM * PULLEY_TEETH)              // 40 mm
#define STEPS_PER_MM    (MOTOR_FULL_SPS * MICROSTEP / MM_PER_REV)   // 80 steps/mm

// Pen Z axis
#define PEN_DOWN_MM    2.0f
#define PEN_UP_MM      2.0f
#define PEN_SPEED_SPS  800

#define DEFAULT_SPD_MM_S  35.0f   // used when speed arg omitted

// ─── CRC32 ─────────────────────────────────────────────────────────────────────
static uint32_t crc32(const uint8_t *data, uint16_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(uint32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

// ─── RS485 low-level ───────────────────────────────────────────────────────────
static void rs485Send(const uint8_t *d, uint8_t len) {
    digitalWrite(RS485_EN_PIN, HIGH);
    delayMicroseconds(80);
    Serial2.write(d, len);
    Serial2.flush();
    delayMicroseconds(80);
    digitalWrite(RS485_EN_PIN, LOW);
}

// Read one response frame.  Returns total bytes received (0 on timeout/error).
static uint8_t rs485Recv(uint8_t *buf, uint8_t maxLen) {
    uint32_t deadline = millis() + RESPONSE_TIMEOUT_MS;
    uint8_t  idx = 0;
    bool     act = false;
    uint8_t  exp = 0;

    while ((int32_t)(millis() - deadline) < 0) {
        if (!Serial2.available()) continue;
        uint8_t b = (uint8_t)Serial2.read();
        if (!act) {
            if (!(b & 1u)) continue;          // not a SIZE byte
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

// ─── Core frame function ───────────────────────────────────────────────────────
// Builds and sends a request.  If waitResp=true, receives and validates response.
// Returns number of response DATA bytes (after STATUS) on success, 0 on failure.
static uint8_t sendCmd(uint8_t addr, uint8_t cmd,
                       const uint8_t *pay, uint8_t payLen,
                       uint8_t *outData, bool waitResp = true) {
    uint8_t buf[32];
    uint8_t n = 0;
    uint8_t afterSize = 1u + 1u + payLen + 4u;   // addr + cmd + payload + crc32
    buf[n++] = (uint8_t)((afterSize << 1) | 1u);
    buf[n++] = addr;
    buf[n++] = cmd;
    for (uint8_t i = 0; i < payLen; i++) buf[n++] = pay[i];
    uint32_t crc = crc32(buf, n);
    buf[n++] = (uint8_t)(crc);
    buf[n++] = (uint8_t)(crc >>  8);
    buf[n++] = (uint8_t)(crc >> 16);
    buf[n++] = (uint8_t)(crc >> 24);

    rs485Send(buf, n);
    if (!waitResp) return 0;

    uint8_t resp[32];
    uint8_t rlen = rs485Recv(resp, sizeof(resp));
    if (rlen < 7u) return 0;

    uint32_t calc = crc32(resp, rlen - 4);
    uint32_t got  = (uint32_t)resp[rlen-4]
                  | ((uint32_t)resp[rlen-3] <<  8)
                  | ((uint32_t)resp[rlen-2] << 16)
                  | ((uint32_t)resp[rlen-1] << 24);
    if (calc != got || resp[1] != RESP_CHAR || resp[2] == STATUS_ERR) return 0;

    uint8_t dlen = rlen - 7u;
    if (outData) for (uint8_t i = 0; i < dlen; i++) outData[i] = resp[3u + i];
    return dlen ? dlen : 1;   // return 1 for STATUS_OK with no data
}

// ─── Command helpers ───────────────────────────────────────────────────────────
bool cmdPing(uint8_t addr) {
    uint8_t d[1];
    return sendCmd(addr, CMD_PING, nullptr, 0, d) > 0;
}

bool cmdEnable(uint8_t addr, bool en) {
    uint8_t p = en ? 1u : 0u;
    bool bc = (addr == BROADCAST);
    return bc ? (sendCmd(addr, CMD_ENABLE, &p, 1, nullptr, false), true)
              : (sendCmd(addr, CMD_ENABLE, &p, 1, nullptr, true) > 0);
}

// Queue one segment. Returns buffer free slots after push (0 = full or error).
uint8_t cmdQueue(uint8_t addr, bool cw, uint16_t steps, uint16_t sps) {
    uint8_t pay[5];
    pay[0] = cw ? 0u : 1u;           // slave: pay[0]==0 → cw
    pay[1] = (uint8_t)(steps);        // little-endian
    pay[2] = (uint8_t)(steps >> 8);
    pay[3] = (uint8_t)(sps);
    pay[4] = (uint8_t)(sps   >> 8);
    uint8_t d[1] = {0};
    uint8_t r = sendCmd(addr, CMD_QUEUE, pay, 5, d);
    return (r > 0) ? d[0] : 0u;
}

// Broadcast GO — fires all armed motors simultaneously (no response).
void cmdGo() {
    sendCmd(BROADCAST, CMD_GO, nullptr, 0, nullptr, false);
}

// Broadcast STOP — immediate halt, clears all buffers (no response).
void cmdStop() {
    sendCmd(BROADCAST, CMD_STOP, nullptr, 0, nullptr, false);
    Serial.println("STOP sent.");
}

// Read motor status.  Returns true on success.
bool cmdGetStatus(uint8_t addr, uint8_t *running,
                  uint8_t *bufUsed, uint8_t *bufFree) {
    uint8_t d[3] = {};
    if (sendCmd(addr, CMD_STATUS, nullptr, 0, d) < 1) return false;
    if (running) *running = d[0];
    if (bufUsed) *bufUsed = d[1];
    if (bufFree) *bufFree = d[2];
    return true;
}

// Poll until motor reports idle (running=0, bufUsed=0).
bool waitIdle(uint8_t addr, uint32_t timeoutMs) {
    uint32_t dl = millis() + timeoutMs;
    uint8_t  lastRun = 0xFF, lastUsed = 0xFF;
    while ((int32_t)(millis() - dl) < 0) {
        uint8_t run = 1, used = 1;
        if (!cmdGetStatus(addr, &run, &used, nullptr)) {
            Serial.printf("Node %d: STATUS no response\n", addr);
        } else {
            if (run != lastRun || used != lastUsed) {
                Serial.printf("Node %d: running=%u  buf_used=%u\n", addr, run, used);
                lastRun = run; lastUsed = used;
            }
            if (run == 0 && used == 0) return true;
        }
        delay(50);
    }
    Serial.printf("Node %d: waitIdle timeout  (last: running=%u buf_used=%u)\n",
                  addr, lastRun, lastUsed);
    return false;
}

// ─── Single-axis move ──────────────────────────────────────────────────────────
bool axisMove(uint8_t addr, bool cw, uint16_t steps, uint16_t sps) {
    if (!cmdQueue(addr, cw, steps, sps)) {
        Serial.printf("Node %d: queue failed\n", addr);
        return false;
    }
    cmdGo();
    uint32_t estMs = (uint32_t)steps * 1000UL / sps + 150UL;
    delay(estMs);
    return waitIdle(addr, 2000);
}

// ─── CoreXY coordinated move ───────────────────────────────────────────────────
// dx/dy in steps (signed).  sps = speed of the faster motor.
bool corexyMove(int16_t dx, int16_t dy, uint16_t sps) {
    int16_t aSteps = dx + dy;
    int16_t bSteps = dx - dy;
    if (aSteps == 0 && bSteps == 0) return true;

    float   magA = (float)abs(aSteps);
    float   magB = (float)abs(bSteps);
    float   maxM = (magA > magB) ? magA : magB;
    uint16_t spsA = (magA > 0.5f) ? (uint16_t)(sps * magA / maxM + 0.5f) : 1u;
    uint16_t spsB = (magB > 0.5f) ? (uint16_t)(sps * magB / maxM + 0.5f) : 1u;

    bool sentA = (aSteps != 0);
    bool sentB = (bSteps != 0);

    if (sentA && !cmdQueue(NODE_A, aSteps > 0, (uint16_t)abs(aSteps), spsA))
        return false;
    if (sentB && !cmdQueue(NODE_B, bSteps > 0, (uint16_t)abs(bSteps), spsB))
        return false;

    cmdGo();

    uint32_t estMs = (uint32_t)(uint16_t)maxM * 1000UL / sps + 150UL;
    delay(estMs);

    bool ok = true;
    if (sentA) ok &= waitIdle(NODE_A, 2000);
    if (sentB) ok &= waitIdle(NODE_B, 2000);
    return ok;
}

// mm/s convenience wrapper
bool corexyMoveMm(float dxMm, float dyMm, float spdMmS) {
    return corexyMove(
        (int16_t)(dxMm * STEPS_PER_MM),
        (int16_t)(dyMm * STEPS_PER_MM),
        (uint16_t)(spdMmS * STEPS_PER_MM));
}

// ─── Pen control ───────────────────────────────────────────────────────────────
static void penDown() {
    axisMove(NODE_Z, true,  (uint16_t)(PEN_DOWN_MM * STEPS_PER_MM), PEN_SPEED_SPS);
}
static void penUp() {
    axisMove(NODE_Z, false, (uint16_t)(PEN_UP_MM   * STEPS_PER_MM), PEN_SPEED_SPS);
}

// ─── Rectangle ─────────────────────────────────────────────────────────────────
void drawRectangle(float wMm, float hMm, float spdMmS) {
    Serial.printf("Rect %.1f x %.1f mm  @ %.1f mm/s\n", wMm, hMm, spdMmS);
    penDown();
    corexyMoveMm( wMm,    0,    spdMmS);
    corexyMoveMm( 0,      hMm,  spdMmS);
    corexyMoveMm(-wMm,    0,    spdMmS);
    corexyMoveMm( 0,     -hMm,  spdMmS);
    penUp();
    Serial.println("Rect done.");
}

// ─── Circle ────────────────────────────────────────────────────────────────────
void drawCircle(float rMm, float spdMmS, uint8_t segs) {
    if (segs < 8)  segs = 8;
    if (segs > 72) segs = 72;
    Serial.printf("Circle r=%.1f mm  @ %.1f mm/s  %d segs\n", rMm, spdMmS, segs);

    float    rSteps = rMm * STEPS_PER_MM;
    uint16_t sps    = (uint16_t)(spdMmS * STEPS_PER_MM);
    float    px = rSteps, py = 0.0f, ex = 0.0f, ey = 0.0f;

    // corexyMove((int16_t)rSteps, 0, sps);
    // penDown();

    for (uint8_t i = 1; i <= segs; i++) {
        float   a   = 2.0f * (float)M_PI * i / segs;
        float   nx  = rSteps * cosf(a);
        float   ny  = rSteps * sinf(a);
        float   rdx = nx - px + ex;
        float   rdy = ny - py + ey;
        int16_t dx  = (int16_t)roundf(rdx);
        int16_t dy  = (int16_t)roundf(rdy);
        ex = rdx - (float)dx;
        ey = rdy - (float)dy;
        if (dx != 0 || dy != 0) corexyMove(dx, dy, sps);
        px = nx; py = ny;
    }
    // penUp();
    // corexyMove(-(int16_t)rSteps, 0, sps);
    Serial.println("Circle done.");
}

// ─── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {}

    pinMode(RS485_EN_PIN, OUTPUT);
    digitalWrite(RS485_EN_PIN, LOW);
    Serial2.setTX(RS485_TX_PIN);
    Serial2.setRX(RS485_RX_PIN);
    Serial2.begin(RS485_BAUD);

    Serial.println("Custom RS485 Master  (230400 baud)");
    Serial.printf("STEPS_PER_MM = %.1f  (MICROSTEP=%d)\n",
                  (float)STEPS_PER_MM, MICROSTEP);
    Serial.println("─────────────────────────────────────────");
    Serial.println("  ping <addr>                           ");
    Serial.println("  enable <addr|all> <0|1>               ");
    Serial.println("  stop                                  ");
    Serial.println("  <addr> f/b <steps> [sps]              ");
    Serial.println("  xy <dx> <dy> [sps]  (steps, signed)   ");
    Serial.println("  rect <W_mm> <H_mm> [speed_mm_s]       ");
    Serial.println("  circle <R_mm> [speed_mm_s] [segs]     ");
}

// ─── Serial command parser ─────────────────────────────────────────────────────
static char    cmdBuf[80];
static uint8_t cmdLen = 0;

void loop() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (cmdLen == 0) continue;
            cmdBuf[cmdLen] = '\0';
            cmdLen = 0;

            uint8_t  addr  = 0;
            char     dir   = 0;
            uint16_t steps = 0, sps = 0;
            int16_t  dx = 0, dy = 0;
            float    fW = 0, fH = 0, fR = 0, fSpd = DEFAULT_SPD_MM_S;
            uint8_t  segs = 36;
            char     enStr[8] = {};
            int      enVal = 0;

            if (strcmp(cmdBuf, "stop") == 0) {
                cmdStop();

            } else if (sscanf(cmdBuf, "ping %hhu", &addr) == 1) {
                Serial.printf("Ping node %u: %s\n",
                              addr, cmdPing(addr) ? "OK" : "no response");

            } else if (sscanf(cmdBuf, "enable %7s %d", enStr, &enVal) == 2) {
                uint8_t a = (strcmp(enStr, "all") == 0)
                            ? BROADCAST : (uint8_t)atoi(enStr);
                cmdEnable(a, enVal != 0);
                Serial.printf("Enable node %s = %d\n", enStr, enVal);

            } else if (sscanf(cmdBuf, "rect %f %f %f", &fW, &fH, &fSpd) >= 2
                       && fW > 0 && fH > 0) {
                drawRectangle(fW, fH, fSpd);

            } else if (sscanf(cmdBuf, "circle %f %f %hhu", &fR, &fSpd, &segs) >= 1
                       && fR > 0) {
                drawCircle(fR, fSpd, segs);

            } else if (sscanf(cmdBuf, "xy %hd %hd %hu", &dx, &dy, &sps) >= 2) {
                if (sps == 0) sps = (uint16_t)(DEFAULT_SPD_MM_S * STEPS_PER_MM);
                corexyMove(dx, dy, sps);

            } else if (sscanf(cmdBuf, "%hhu %c %hu %hu",
                               &addr, &dir, &steps, &sps) >= 3
                       && addr >= 1 && steps > 0
                       && (dir=='f'||dir=='F'||dir=='b'||dir=='B')) {
                if (sps == 0) sps = 500;
                axisMove(addr, (dir=='f'||dir=='F'), steps, sps);

            } else {
                Serial.println("Unknown command.");
            }
        } else if (cmdLen < (uint8_t)(sizeof(cmdBuf) - 1)) {
            cmdBuf[cmdLen++] = c;
        }
    }
}
