// ATTINY_Custom_Slave.ino
// ATtiny3224 + MAX485E + DRV8825
// Custom RS485 protocol — same framing style as tomrodinger/servomotor
//
// Frame structure (request):
//   [SIZE] [ADDR] [CMD] [PAYLOAD...] [CRC32 LE 4 bytes]
//   SIZE = ((remaining_bytes_after_size) << 1) | 1
//   LSB of SIZE is always 1 — used for self-synchronizing frame detection
//
// Frame structure (response):
//   [SIZE] [0xFD] [STATUS] [DATA...] [CRC32 LE 4 bytes]
//   0xFD = response character (CRC32 enabled)
//   STATUS: 0x00=ok/no data, 0x01=ok/data follows, 0xFF=error
//
// Broadcast address 0xFF: all slaves execute, none respond.
//
// Commands:
//   0x01 CMD_PING    → reply: node_id (1 byte)
//   0x02 CMD_QUEUE   → payload: dir(1)+steps(2 LE)+speed(2 LE)
//                    → reply: buf_free (1 byte); auto-starts if idle
//   0x03 CMD_GO      → broadcast: start all armed motors simultaneously
//   0x04 CMD_STOP    → stop immediately, clear buffer; reply: STATUS_OK
//   0x05 CMD_STATUS  → reply: running(1)+buf_used(1)+buf_free(1)
//   0x06 CMD_ENABLE  → payload: enable(1); reply: STATUS_OK
//
// Build with megaTinyCore. Set NODE_ID per board.
// Baud: 230400 (matches servomotor firmware convention)

#include <Arduino.h>

// ─── Board config ──────────────────────────────────────────────────────────────
#define NODE_ID       1            // << CHANGE PER BOARD (1–253)

#define RS485_DE_PIN  PIN_PA3
#define LED_PIN       PIN_PA5
#define ENABLE_PIN    PIN_PA4
#define STEP_PIN      PIN_PB0
#define DIR_PIN       PIN_PB1

// Direct port masks for fast step pulses (PB0 = bit 0 of PORTB)
#define STEP_PORT     PORTB
#define STEP_BM       PIN0_bm

// ─── Protocol constants ────────────────────────────────────────────────────────
#define RS485_BAUD    230400
#define BROADCAST     0xFF
#define RESP_CHAR     0xFD

#define STATUS_OK     0x00
#define STATUS_DATA   0x01
#define STATUS_ERR    0xFF

#define CMD_PING      0x01
#define CMD_QUEUE     0x02
#define CMD_GO        0x03
#define CMD_STOP      0x04
#define CMD_STATUS    0x05
#define CMD_ENABLE    0x06

// ─── Motor limits ──────────────────────────────────────────────────────────────
#define MAX_STEPS     10000
#define MAX_SPEED     5000
#define MIN_SPEED     10
#define PULSE_US      2       // DRV8825 minimum step pulse (µs)

// ─── 8-segment ring buffer ─────────────────────────────────────────────────────
#define BUF_SIZE  8

struct Segment {
    uint16_t steps;
    uint16_t speed;   // steps/sec
    bool     cw;
};

static Segment  segBuf[BUF_SIZE];
static uint8_t  bufHead  = 0;
static uint8_t  bufTail  = 0;
static uint8_t  bufCount = 0;

static bool bufPush(uint16_t steps, uint16_t speed, bool cw) {
    if (bufCount >= BUF_SIZE) return false;
    segBuf[bufTail].steps = steps;
    segBuf[bufTail].speed = speed;
    segBuf[bufTail].cw    = cw;
    bufTail = (bufTail + 1) % BUF_SIZE;
    bufCount++;
    return true;
}

static bool bufPop(Segment &s) {
    if (bufCount == 0) return false;
    s       = segBuf[bufHead];
    bufHead = (bufHead + 1) % BUF_SIZE;
    bufCount--;
    return true;
}

static void bufClear() { bufHead = bufTail = bufCount = 0; }

// ─── Motor state ───────────────────────────────────────────────────────────────
// States: IDLE → ARMED (after CMD_QUEUE) → RUNNING (after CMD_GO or auto-start)
enum MotorState { IDLE, ARMED, RUNNING };
static MotorState motorState  = IDLE;
static bool       drvEnabled  = false;

static uint16_t stepsRemaining = 0;
static uint32_t stepPeriodUs   = 0;
static uint32_t lastStepUs     = 0;
static bool     inPulse        = false;
static uint32_t pulseStartUs   = 0;

static void loadNextSegment() {
    Segment s;
    if (bufPop(s)) {
        stepsRemaining = s.steps;
        stepPeriodUs   = 1000000UL / s.speed;
        if (stepPeriodUs < (uint32_t)PULSE_US * 2)
            stepPeriodUs = (uint32_t)PULSE_US * 2;
        digitalWrite(DIR_PIN, s.cw ? HIGH : LOW);
        lastStepUs  = micros();
        inPulse     = false;
        motorState  = RUNNING;
        digitalWrite(LED_PIN, HIGH);
    } else {
        motorState = IDLE;
        digitalWrite(LED_PIN, LOW);
    }
}

static void stopMotor() {
    bufClear();
    stepsRemaining = 0;
    inPulse        = false;
    motorState     = IDLE;
    digitalWrite(LED_PIN, LOW);
}

// ─── CRC32 ─────────────────────────────────────────────────────────────────────
static uint32_t crc32(const uint8_t *buf, uint8_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(uint32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

// ─── RS485 TX ─────────────────────────────────────────────────────────────────
static void rs485Write(const uint8_t *data, uint8_t len) {
    digitalWrite(RS485_DE_PIN, HIGH);
    delayMicroseconds(10);
    Serial1.write(data, len);
    Serial1.flush();          // wait until shift register is empty
    delayMicroseconds(10);    // last-bit line-idle guard
    digitalWrite(RS485_DE_PIN, LOW);
}

// ─── Response builder ──────────────────────────────────────────────────────────
static void respond(bool isBroadcast, uint8_t status,
                    const uint8_t *data, uint8_t dlen) {
    if (isBroadcast) return;

    // [SIZE][0xFD][STATUS][data...][CRC32 4 bytes]
    uint8_t remaining = 1u + 1u + dlen + 4u;   // 0xFD + status + data + crc
    uint8_t buf[32];
    uint8_t n = 0;
    buf[n++] = (uint8_t)((remaining << 1) | 1u);
    buf[n++] = RESP_CHAR;
    buf[n++] = status;
    for (uint8_t i = 0; i < dlen; i++) buf[n++] = data[i];
    uint32_t crc = crc32(buf, n);
    buf[n++] = (uint8_t)(crc);
    buf[n++] = (uint8_t)(crc >> 8);
    buf[n++] = (uint8_t)(crc >> 16);
    buf[n++] = (uint8_t)(crc >> 24);
    rs485Write(buf, n);
}

// ─── Frame dispatcher ──────────────────────────────────────────────────────────
// rxBuf[0] = SIZE, rxBuf[1] = ADDR, rxBuf[2] = CMD, rxBuf[3..] = payload,
// last 4 bytes = CRC32
static void processFrame(const uint8_t *rxBuf, uint8_t totalLen) {
    // Verify CRC32 over all bytes except the trailing 4
    if (totalLen < 7) return;   // size(1)+addr(1)+cmd(1)+crc(4) minimum
    uint32_t calcCrc = crc32(rxBuf, totalLen - 4);
    uint32_t rxCrc   = (uint32_t)rxBuf[totalLen-4]
                     | ((uint32_t)rxBuf[totalLen-3] <<  8)
                     | ((uint32_t)rxBuf[totalLen-2] << 16)
                     | ((uint32_t)rxBuf[totalLen-1] << 24);
    if (calcCrc != rxCrc) return;

    uint8_t addr = rxBuf[1];
    bool    isBc = (addr == BROADCAST);
    if (addr != (uint8_t)NODE_ID && !isBc) return;

    uint8_t        cmd  = rxBuf[2];
    const uint8_t *pay  = rxBuf + 3;    // payload start
    uint8_t        resp[4];

    switch (cmd) {

    case CMD_PING:
        resp[0] = (uint8_t)NODE_ID;
        respond(isBc, STATUS_DATA, resp, 1);
        break;

    case CMD_QUEUE: {
        // payload: dir(1) steps(2 LE) speed(2 LE)
        bool     cw    = (pay[0] == 0);
        uint16_t steps = (uint16_t)pay[1] | ((uint16_t)pay[2] << 8);
        uint16_t speed = (uint16_t)pay[3] | ((uint16_t)pay[4] << 8);
        if (steps == 0 || steps > MAX_STEPS) { respond(isBc, STATUS_ERR, 0, 0); break; }
        if (speed < MIN_SPEED) speed = MIN_SPEED;
        if (speed > MAX_SPEED) speed = MAX_SPEED;
        bool ok = bufPush(steps, speed, cw);
        if (ok && motorState == IDLE && drvEnabled) {
            // Auto-start: transition to ARMED, wait for CMD_GO or auto-launch
            motorState = ARMED;
        }
        resp[0] = ok ? (uint8_t)(BUF_SIZE - bufCount) : 0u;
        respond(isBc, ok ? STATUS_DATA : STATUS_ERR, resp, 1);
        break;
    }

    case CMD_GO:
        if (motorState == ARMED && drvEnabled) loadNextSegment();
        respond(isBc, STATUS_OK, 0, 0);
        break;

    case CMD_STOP:
        stopMotor();
        respond(isBc, STATUS_OK, 0, 0);
        break;

    case CMD_STATUS:
        resp[0] = (motorState == RUNNING) ? 1u : 0u;
        resp[1] = bufCount;
        resp[2] = (uint8_t)(BUF_SIZE - bufCount);
        respond(isBc, STATUS_DATA, resp, 3);
        break;

    case CMD_ENABLE:
        drvEnabled = (pay[0] != 0);
        digitalWrite(ENABLE_PIN, drvEnabled ? LOW : HIGH);  // active-low
        if (!drvEnabled) stopMotor();
        respond(isBc, STATUS_OK, 0, 0);
        break;

    default:
        respond(isBc, STATUS_ERR, 0, 0);
        break;
    }
}

// ─── RX state machine ──────────────────────────────────────────────────────────
static uint8_t rxBuf[32];
static uint8_t rxLen    = 0;
static uint8_t rxExpect = 0;   // bytes expected after SIZE byte
static bool    rxActive = false;

static void rxByte(uint8_t b) {
    if (!rxActive) {
        if (b & 1u) {                      // LSB=1 → valid SIZE byte
            rxBuf[0]  = b;
            rxExpect  = b >> 1;
            rxLen     = 1;
            rxActive  = (rxExpect > 0);
            if (!rxActive) processFrame(rxBuf, rxLen);
        }
    } else {
        if (rxLen < (uint8_t)sizeof(rxBuf)) rxBuf[rxLen++] = b;
        if (rxLen == 1u + rxExpect) {
            rxActive = false;
            processFrame(rxBuf, rxLen);
            rxLen = 0;
        }
    }
}

// ─── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    pinMode(RS485_DE_PIN, OUTPUT); digitalWrite(RS485_DE_PIN, LOW);
    pinMode(LED_PIN,      OUTPUT); digitalWrite(LED_PIN,      LOW);
    pinMode(STEP_PIN,     OUTPUT); digitalWrite(STEP_PIN,     LOW);
    pinMode(DIR_PIN,      OUTPUT); digitalWrite(DIR_PIN,      LOW);
    pinMode(ENABLE_PIN,   OUTPUT); digitalWrite(ENABLE_PIN,   HIGH); // disabled

    Serial1.begin(RS485_BAUD);

    // Blink NODE_ID times → visual address confirmation
    uint8_t blinks = (NODE_ID < 6) ? (uint8_t)NODE_ID : 5u;
    for (uint8_t i = 0; i < blinks; i++) {
        digitalWrite(LED_PIN, HIGH); delay(150);
        digitalWrite(LED_PIN, LOW);  delay(150);
    }
}

// ─── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = micros();

    // ── Non-blocking step generation ─────────────────────────────────────────
    if (motorState == RUNNING && stepsRemaining > 0) {
        if (!inPulse) {
            if ((now - lastStepUs) >= stepPeriodUs) {
                STEP_PORT.OUTSET = STEP_BM;  // STEP HIGH (fast direct port write)
                pulseStartUs = now;
                inPulse      = true;
            }
        } else {
            if ((now - pulseStartUs) >= (uint32_t)PULSE_US) {
                STEP_PORT.OUTCLR = STEP_BM;  // STEP LOW
                inPulse          = false;
                lastStepUs       = now;
                if (--stepsRemaining == 0) loadNextSegment();
            }
        }
    }

    // ── RS485 receive ─────────────────────────────────────────────────────────
    while (Serial1.available()) rxByte((uint8_t)Serial1.read());
}
