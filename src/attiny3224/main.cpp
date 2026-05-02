// ATtiny3224 + MAX485E + DRV8825
// Hardware-timed Step Generation (TCB0)
// Lock-Free Implementation

// Custom RS485 protocol — same framing style as tomrodinger/servomotor
//
// Frame structure (request):
//   [SIZE] [ADDR] [CMD] [PAYLOAD...] [CRC16 LE 2 bytes]
//   SIZE = ((remaining_bytes_after_size) << 1) | 1
//   LSB of SIZE is always 1 — used for self-synchronizing frame detection
//
// Frame structure (response):
//   [SIZE] [STATUS] [DATA...] [CRC16 LE 2 bytes]
//   STATUS: 0x00=ok/no data, 0x01=ok/data follows, 0xFF=error
//
// Broadcast address 0xFF: all slaves execute, none respond.
//
// Commands:
//   0x01 CMD_PING    → reply: node_id (1 byte)
//   0x02 CMD_QUEUE   → payload: dir(1)+steps(2 LE)+speed(2 LE)
//                    → reply: buf_free (1 byte);
//   0x03 CMD_GO      → broadcast: start all armed motors simultaneously
//   0x04 CMD_STOP    → stop immediately, clear buffer; reply: STATUS_OK if not broadcast
//   0x05 CMD_STATUS  → reply: running(1)+buf_used(1)+buf_free(1)+steps_remaining(2)
//   0x06 CMD_ENABLE  → payload: enable(1); reply: STATUS_OK if not broadcast
//
// Build with megaTinyCore.
// Baud: 230400 (matches servomotor firmware convention)

#include <Arduino.h>

#define DEBUG_SERIAL 0

// ─── Board config ──────────────────────────────────────────────────────────────
#include "nodeid.h"
// #define NODE_ID 1
#define RS485_DE_PIN  PIN_PA3
#define LED_PIN       PIN_PA5
#define ENABLE_PIN    PIN_PA4
#define STEP_PIN      PIN_PB0
#define DIR_PIN       PIN_PB1

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
#define MAX_STEPS     60000
#define MAX_SPEED     10000
#define MIN_SPEED     10

// ─── Lock-Free Ring Buffer ─────────────────────────────────────────────────────
#define BUF_SIZE  8 // Powers of 2 recommended for optimised wraps

struct Segment {
    uint16_t steps;
    uint16_t speed;
    bool     cw;
};

static Segment segBuf[BUF_SIZE];
static volatile uint8_t bufHead = 0; // volatile for usage in interrupts
static volatile uint8_t bufTail = 0;

static uint8_t getBufCount() {
    if (bufTail >= bufHead) return (bufTail - bufHead);
    return (BUF_SIZE - bufHead + bufTail);
}

static bool bufPush(uint16_t steps, uint16_t speed, bool cw) {
    uint8_t nextTail = (bufTail + 1) % BUF_SIZE;
    if (nextTail == bufHead) return false; // Full
    
    segBuf[bufTail].steps = steps;
    segBuf[bufTail].speed = speed;
    segBuf[bufTail].cw    = cw;
    
    // Memory barrier ensures struct is written to RAM before tail updates
    __asm__ volatile ("" ::: "memory");
    
    bufTail = nextTail;
    return true;
}

static bool bufPop(Segment &s) {
    if (bufHead == bufTail) return false; // Empty
    
    s = segBuf[bufHead];
    
    // Memory barrier ensures struct is read before head updates
    __asm__ volatile ("" ::: "memory");
    
    bufHead = (bufHead + 1) % BUF_SIZE;
    return true;
}

// ─── Motor state ───────────────────────────────────────────────────────────────
enum MotorState { IDLE, ARMED, RUNNING };
static volatile MotorState motorState = IDLE; 
static bool drvEnabled = false;
static volatile uint16_t stepsRemaining = 0;
static volatile bool segmentFinished = false; 

// ─── Read-Twice Pattern for 16-bit atomicity on 8-bit AVR ────────────────────
uint16_t getStepsRemainingSafe() {
    while (true) {
        uint16_t a = stepsRemaining;
        uint16_t b = stepsRemaining;
        if (a == b) return a;
    }
}

// ─── Hardware Timer Control ────────────────────────────────────────────────────
void startTimer(uint16_t speed) {
    uint32_t period = (F_CPU / 2) / speed;
    if (period > 0xFFFF) period = 0xFFFF;
    TCB0.CCMP = (uint16_t)period;
    TCB0.CNT = 0;
    TCB0.INTFLAGS = TCB_CAPT_bm; // Clear any pending interrupts!
    TCB0.CTRLA |= TCB_ENABLE_bm;
}

void stopTimer() {
    TCB0.CTRLA &= ~TCB_ENABLE_bm;
    STEP_PORT.OUTCLR = STEP_BM;
}

static void loadNextSegment() {
    Segment s;
    if (bufPop(s)) {
        stepsRemaining = s.steps;
        digitalWrite(DIR_PIN, s.cw ? HIGH : LOW);
        segmentFinished = false;
        motorState = RUNNING;
        digitalWrite(LED_PIN, HIGH);
        startTimer(s.speed);
    } else {
        motorState = IDLE;
        digitalWrite(LED_PIN, LOW);
        stopTimer();
    }
}

static void stopMotor() {
    stopTimer();
    bufHead = 0;
    bufTail = 0;
    stepsRemaining = 0;
    motorState = IDLE;
    segmentFinished = false; // Fixed: prevent accidental restart!
    digitalWrite(LED_PIN, LOW);
}

// ─── Timer Interrupt (ISR) ─────────────────────────────────────────────────────
ISR(TCB0_INT_vect) {
    TCB0.INTFLAGS = TCB_CAPT_bm; // Clear interrupt flag
    STEP_PORT.OUTTGL = STEP_BM;
    
    // Toggling the pin takes two interrupts to make one full step pulse
    if (STEP_PORT.OUT & STEP_BM) {
        if (--stepsRemaining == 0) {
            TCB0.CTRLA &= ~TCB_ENABLE_bm;
            segmentFinished = true;
        }
    }
}

// ─── CRC16 & Communication ─────────────────────────────────────────────────────
static uint16_t crc16(const uint8_t *buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)buf[i] << 8);
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

static void rs485Write(const uint8_t *data, uint8_t len) {
    digitalWrite(RS485_DE_PIN, HIGH);
    delayMicroseconds(5);
    Serial1.write(data, len);
    Serial1.flush(); // AVR core waits for Shift Register properly 
    delayMicroseconds(5);
    digitalWrite(RS485_DE_PIN, LOW);
}

static void respond(bool isBroadcast, uint8_t status, const uint8_t *data, uint8_t dlen) {
    if (isBroadcast) return;
    uint8_t buf[32], n = 0;
    uint8_t remaining = 1u + 1u + dlen + 2u; 
    buf[n++] = (uint8_t)((remaining << 1) | 1u);
    buf[n++] = RESP_CHAR;
    buf[n++] = status;
    for (uint8_t i = 0; i < dlen; i++) buf[n++] = data[i];
    uint16_t crc = crc16(buf, n);
    buf[n++] = (uint8_t)(crc & 0xFF); buf[n++] = (uint8_t)(crc >> 8);
    rs485Write(buf, n);
}

static void processFrame(const uint8_t *rxBuf, uint8_t totalLen) {
#ifdef DEBUG_SERIAL
    Serial.print("Data (");
    Serial.print(totalLen);
    Serial.print(" bytes): ");
    
    for (uint8_t i = 0; i < totalLen; i++) {
        // Print in Hexadecimal format for clarity
        if (rxBuf[i] < 0x10) Serial.print("0"); // Leading zero for single digits
        Serial.print(rxBuf[i], HEX);
        Serial.print(" ");
    }
    Serial.println();
#endif
    
    if (totalLen < 5) {
#ifdef DEBUG_SERIAL
            Serial.println("Frame too small, discarding it...");
#endif
        return;
    }
    // Check address 
    uint8_t addr = rxBuf[1];
    bool isBc = (addr == BROADCAST);
    if (addr != (uint8_t)NODE_ID && !isBc) {
#ifdef DEBUG_SERIAL
            Serial.println("Wrong address, discarding frame...");
#endif
        return;
    }

    // Check crc
    uint16_t calcCrc = crc16(rxBuf, totalLen - 2);
    uint16_t rxCrc   = (uint16_t)rxBuf[totalLen-2] | ((uint16_t)rxBuf[totalLen-1] << 8);
    if (calcCrc != rxCrc) {
#ifdef DEBUG_SERIAL
            Serial.println("Bad crc, discarding frame...");
#endif
        return;
    }

    // Get command and payload
    uint8_t cmd = rxBuf[2];
    const uint8_t *pay = rxBuf + 3;
    
    // Prepare response
    uint8_t resp[8];

    switch (cmd) {
        case CMD_PING:
            resp[0] = (uint8_t)NODE_ID;
#ifdef DEBUG_SERIAL
            Serial.println("Pong");
#endif
            respond(isBc, STATUS_DATA, resp, 1);  // respond immediately — before any delay
            digitalWrite(LED_PIN, HIGH); delay(150);
            digitalWrite(LED_PIN, LOW);  delay(150);
            break;

        case CMD_QUEUE: {
            // Serial.println("Processing queue");
            bool cw = (pay[0] == 0);
            uint16_t steps = (uint16_t)pay[1] | ((uint16_t)pay[2] << 8);
            uint16_t speed = (uint16_t)pay[3] | ((uint16_t)pay[4] << 8);
            if (steps == 0 || steps > MAX_STEPS) { respond(isBc, STATUS_ERR, 0, 0); break; }
            speed = constrain(speed, MIN_SPEED, MAX_SPEED);
            bool ok = bufPush(steps, speed, cw);
            if (ok && motorState == IDLE && drvEnabled) motorState = ARMED;
            resp[0] = ok ? (uint8_t)(8 - getBufCount()) : 0u; 
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

        case CMD_STATUS: {
            uint8_t bc = getBufCount();
            uint16_t currentSteps = getStepsRemainingSafe();

            resp[0] = (motorState == RUNNING) ? 1u : 0u;
            resp[1] = bc;
            resp[2] = (uint8_t)(8 - bc);
            resp[3] = (uint8_t)(currentSteps & 0xFF); 
            resp[4] = (uint8_t)(currentSteps >> 8);   
            
            respond(isBc, STATUS_DATA, resp, 5);
            break;
        }

        case CMD_ENABLE:
            drvEnabled = (pay[0] != 0);
            digitalWrite(ENABLE_PIN, drvEnabled ? LOW : HIGH);
            if (!drvEnabled) stopMotor();
            respond(isBc, STATUS_OK, 0, 0);
            break;

        default:
            respond(isBc, STATUS_ERR, 0, 0);
            break;
    }
}

// ─── RX State Machine ──────────────────────────────────────────────────────────
static uint8_t rxBuf[32], rxLen = 0, rxExpect = 0;
static bool rxActive = false;

static void rxByte(uint8_t b) {
    if (!rxActive) {
        if (b & 1u) {
            rxBuf[0] = b; rxExpect = b >> 1; rxLen = 1;
            rxActive = (rxExpect > 0);
            if (!rxActive) processFrame(rxBuf, rxLen);
        }
    } else {
        if (rxLen < (uint8_t)sizeof(rxBuf)) rxBuf[rxLen++] = b;
        if (rxLen == 1u + rxExpect) {
            rxActive = false;
            processFrame(rxBuf, rxLen);
        }
    }
}

void setup() {
    pinMode(RS485_DE_PIN, OUTPUT); digitalWrite(RS485_DE_PIN, LOW);
    pinMode(LED_PIN,      OUTPUT); digitalWrite(LED_PIN,      LOW);
    pinMode(STEP_PIN,     OUTPUT); digitalWrite(STEP_PIN,     LOW);
    pinMode(DIR_PIN,      OUTPUT); digitalWrite(DIR_PIN,      LOW);
    pinMode(ENABLE_PIN,   OUTPUT); digitalWrite(ENABLE_PIN,   LOW);

    Serial1.begin(RS485_BAUD);
#ifdef DEBUG_SERIAL
    Serial.begin(115200);
#endif

    // Timer settings
    TCB0.CTRLB = TCB_CNTMODE_INT_gc; // Periodic interrupt
    TCB0.INTCTRL = TCB_CAPT_bm; // Capture or Timeout bit mask
    TCB0.CTRLA = TCB_CLKSEL_DIV1_gc; // CLK_PER

    // Blink NODE_ID times → visual address confirmation
    uint8_t blinks = (NODE_ID < 6) ? (uint8_t)NODE_ID : 5u;
    for (uint8_t i = 0; i < blinks; i++) {
        digitalWrite(LED_PIN, HIGH); delay(150);
        digitalWrite(LED_PIN, LOW);  delay(150);
    }
}

void loop() {
    if (segmentFinished) loadNextSegment();
    while (Serial1.available()) rxByte((uint8_t)Serial1.read());
}