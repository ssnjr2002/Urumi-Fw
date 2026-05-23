// ATtiny3224 + MAX485E + DRV8825
// TODO: make this dumb

#include <Arduino.h>
#include <util/atomic.h>
#include "common.h"

// #define DEBUG_SERIAL

// ─── Board config ──────────────────────────────────────────────────────────────
#include "nodeid.h"
// #define NODE_ID 1
#define RS485_DE_PIN  PIN_PA3
#define LED_PIN       PIN_PA5
#define ENABLE_PIN    PIN_PA4
#define STEP_PIN      PIN_PB0
#define DIR_PIN       PIN_PB1

#define LED_PORT      PORTA
#define LED_BM        PIN5_bm

#define DIR_PORT      PORTB
#define DIR_BM        PIN1_bm

#define STEP_PORT     PORTB
#define STEP_BM       PIN0_bm

// ─── Motor limits ──────────────────────────────────────────────────────────────
#define MAX_STEPS       32000
#define MAX_SPEED       32000
#define MIN_SPEED       153         
#define STEP_CLOCK_FREQ (F_CPU / 2)
// MIN_SPEED any lower than 153 will cause an overflow if period is an uint16
// period = STEP_CLOCK_FREQ/SPEED
// STEP_CLOCK_FREQ = 20MHz / 2
// STEP_CLOCK_FREQ/MIN_SPEED < 2^16

// ─── Lock-Free Ring Buffer ─────────────────────────────────────────────────────
#define BUF_SIZE  8 // Powers of 2 recommended for optimised wraps

struct Segment {
    uint16_t steps;
    uint16_t period;
    // TODO: Maybe use a byte and bitmasks instead of bools
    bool     cw;
    bool     isDummy;
};

static Segment segBuf[BUF_SIZE];
static volatile uint8_t bufHead = 0; // volatile for usage in interrupts
static volatile uint8_t bufTail = 0;

static uint8_t getBufCount() {
    if (bufTail >= bufHead) return (bufTail - bufHead);
    return (BUF_SIZE - bufHead + bufTail);
}

static bool bufPush(uint16_t steps, uint16_t period, bool cw, bool isDummy) {
    uint8_t nextTail = (bufTail + 1) % BUF_SIZE;
    if (nextTail == bufHead) return false; // Full
    
    segBuf[bufTail].steps = steps;
    segBuf[bufTail].period = period;
    segBuf[bufTail].cw    = cw;
    segBuf[bufTail].isDummy = isDummy;
    
    // Memory barrier ensures struct is written to RAM before tail updates
    __asm__ volatile ("" ::: "memory");
    
    bufTail = nextTail;
    return true;
}

static inline bool bufPop(Segment &s) {
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
static volatile uint8_t activeStepMask = 0; // The "Virtual Port" mask
static volatile bool isStepPhase = false;   // Software toggle for cycle tracking

// Read-Twice Pattern for 16-bit atomicity on 8-bit AVR
uint16_t getStepsRemainingSafe() {
    while (true) {
        uint16_t a = stepsRemaining;
        uint16_t b = stepsRemaining;
        if (a == b) return a;
    }
}

// ─── Timer Helpers ───────────────────────────────────────────────────────────
uint16_t spsToPeriod(uint16_t sps) {
    uint32_t period = STEP_CLOCK_FREQ / sps;
    if (period > 0xFFFF) period = 0xFFFF;
    return (uint16_t)period;
}

static inline void applySegment(const Segment &s) {
    stepsRemaining = s.steps;
    isStepPhase = false;
    
    STEP_PORT.OUTCLR = STEP_BM; // Step will be low at start
    if (s.isDummy) {
        activeStepMask = 0; // Dummy: Toggles nothing
        LED_PORT.OUTCLR = LED_BM; // Turn off LED
    } else {
        activeStepMask = STEP_BM; // Real: Toggles the actual pin
        if (s.cw) DIR_PORT.OUTSET = DIR_BM; // Set direction
        else DIR_PORT.OUTCLR = DIR_BM;
        LED_PORT.OUTSET = LED_BM; // Turn on LED
    }
    
    TCB0.CCMP = s.period; // Update speed immediately
}

void startMotor() {
    if (motorState == RUNNING) return;

    Segment first;
    if (bufPop(first)) {
        motorState = RUNNING;
        applySegment(first);
        
        TCB0.CNT = 0; // Set timer to 0
        TCB0.INTFLAGS = TCB_CAPT_bm; // Clear any pending interrupts!
        TCB0.CTRLA |= TCB_ENABLE_bm; // Enable timer
    }
}

void stopMotor() {
    // ATOMIC_BLOCK makes sure no interrupts are interfering
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        TCB0.CTRLA          &= ~TCB_ENABLE_bm; // Disable timer
        TCB0.INTFLAGS       =   TCB_CAPT_bm;   // Clear pending interrupts
        STEP_PORT.OUTCLR    =   STEP_BM;       // Ensure step pin is LOW
        LED_PORT.OUTCLR     =   LED_BM;        // LED off
    
        // Reset buffer
        bufHead = 0;
        bufTail = 0;
        stepsRemaining = 0;
        motorState = IDLE;
        activeStepMask = 0;
        isStepPhase = false;
    }
}

// ─── Timer Interrupt (ISR) ─────────────────────────────────────────────────────
ISR(TCB0_INT_vect) {
    TCB0.INTFLAGS = TCB_CAPT_bm; 
    
    // Toggle based on mask. If mask is 0 (dummy), nothing happens to the pin.
    STEP_PORT.OUTTGL = activeStepMask; 
    
    // We toggle a software boolean to track the "high" phase of the step cycle
    // This ensures we decrement stepsRemaining even for dummy segments.
    isStepPhase = !isStepPhase;
    
    if (!isStepPhase) { // falling edge to make sure we end our segments with step pin set to low
        if (--stepsRemaining == 0) {
            Segment next;
            if (bufPop(next)) {
                applySegment(next); // apply next segment
            } else {
                TCB0.CTRLA &= ~TCB_ENABLE_bm; // Stop timer
                activeStepMask = 0;
                STEP_PORT.OUTCLR = STEP_BM;
                LED_PORT.OUTCLR = LED_BM; // LED off
                motorState = IDLE;
            }
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
            respond(isBc, STATUS_DATA, resp, 1);
            // TODO: get rid of this blocking jank
            digitalWrite(LED_PIN, HIGH); delay(150);
            digitalWrite(LED_PIN, LOW);  delay(150);
            break;

        case CMD_QUEUE: {
            // Serial.println("Processing queue");
            bool cw = (pay[0] == 0);
            uint16_t steps = (uint16_t)pay[1] | ((uint16_t)pay[2] << 8);
            uint16_t speed = (uint16_t)pay[3] | ((uint16_t)pay[4] << 8);
            
            if (steps == 0 || steps > MAX_STEPS) { 
                respond(isBc, STATUS_ERR, 0, 0); 
                break; 
            }
            
            speed = constrain(speed, MIN_SPEED, MAX_SPEED);
            uint16_t period = spsToPeriod(speed);
            bool ok = bufPush(steps, period, cw, false); // false = Not a dummy
            
            if (ok && motorState == IDLE && drvEnabled) motorState = ARMED;
            
            // 3-Byte Response [running, used, free]
            resp[0] = (motorState == RUNNING) ? 1u : 0u;
            uint8_t used = getBufCount();
            resp[1] = used;
            resp[2] = 8 - used; 

            respond(isBc, ok ? STATUS_DATA : STATUS_ERR, resp, 3);
            break;
        }

        case CMD_QUEUE_DUMMY: {
            uint8_t numMotors = pay[0];
            bool iAmExcluded = false;
            
            // 1. Check if my NODE_ID is in the exclusion list
            for (uint8_t i = 0; i < numMotors; i++) {
                if (pay[1 + i] == NODE_ID) {
                    iAmExcluded = true;
                    break; // break from loop
                }
            }
            if (iAmExcluded) break; // break from switch

            uint8_t offset = 1 + numMotors;    
            bool cw = (pay[offset] == 0);
            uint16_t steps = (uint16_t)pay[offset + 1] | ((uint16_t)pay[offset + 2] << 8);
            uint16_t speed = (uint16_t)pay[offset + 3] | ((uint16_t)pay[offset + 4] << 8);
            // TODO: think about what happens when master sends a dummy outside step constraints 
            // but the non dummy nodes had steps within constraint. Maybe not an issue since 
            // at least one of the non dummy nodes would also fail which would halt execution.
            if (steps == 0 || steps > MAX_STEPS) break;

            speed = constrain(speed, MIN_SPEED, MAX_SPEED);
            uint16_t period = spsToPeriod(speed);
            bool ok = bufPush(steps, period, cw, true);
            if (ok && motorState == IDLE && drvEnabled) motorState = ARMED;

            break;
        }

        case CMD_GO:
            if (motorState == ARMED && drvEnabled) startMotor();
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
    while (Serial1.available()) rxByte((uint8_t)Serial1.read());
}