// stepper.cpp — stepper node type.
// Owns the RX ISR (so the time-critical stream path inlines with no register
// spill), the step-pulse one-shot timer, position tracking, and the core↔type
// hooks (node_type / node_setup / node_set_enabled / node_handle_command).
//
// All board-specific symbols (USART instance, step/dir ports, timer peripheral,
// motor enable polarity) come from the HAL contract via board.h +
// stepper/stepper.h → board/hal/. After preprocessing these are direct register
// accesses — zero indirection, safe inside ISRs.
#include <Arduino.h>
#include "board.h"
#include "stepper/stepper.h"
#include "common.h"
#include "node_hooks.h"
#include "rs485/frame.h"

// drivers_init() is declared by hal_stepper.h (via motor.h). A weak no-op
// default lives in board/hal/motor.cpp (always compiled); a board with real
// driver init (AVR128DB32 TMC/DRV) provides a strong override in its
// drivers.cpp. Call unconditionally — no null check needed.

// ─── Stepper state ──────────────────────────────────────────────────────────
static volatile int32_t absolutePosition = 0;
static volatile bool    streamEnabled    = false;
static bool             currentDir       = false;
static uint8_t          stepBitMask      = 0;
static uint8_t          dirBitMask       = 0;

// ─── Hooks ──────────────────────────────────────────────────────────────────
// Guard against an env that compiles this type dir with the wrong identity flag.
#ifdef NODE_TYPE
static_assert(NODE_TYPE == NODE_TYPE_STEPPER,
              "stepper.cpp compiled with a non-stepper -DNODE_TYPE");
#endif

uint8_t node_type(void) { return NODE_TYPE_STEPPER; }

void node_setup(void) {
    pinMode(HAL_STEP_PIN, OUTPUT); digitalWrite(HAL_STEP_PIN, LOW);
    pinMode(HAL_DIR_PIN,  OUTPUT); digitalWrite(HAL_DIR_PIN,  LOW);
    pinMode(HAL_EN_PIN,   OUTPUT);

    // Init driver (brings up SPI for TMC2660) BEFORE the first HAL_MOTOR_DISABLE,
    // which for TMC issues a toff() over SPI.
    drivers_init();
    HAL_MOTOR_DISABLE();

    // Step-pulse one-shot timer: pulls STEP low HAL_STEP_PULSE_CCMP cycles
    // after a step.
    HAL_STEP_TIMER_INST.CTRLB   = HAL_STEP_TIMER_CNTMODE;
    HAL_STEP_TIMER_INST.INTCTRL = HAL_STEP_TIMER_CAPT_bm;

    // Stream slot from NODE_ID (until CMD_ENGAGE makes this runtime-assigned).
    stepBitMask = 1 << ((NODE_ID - 1) * 2);
    dirBitMask  = 1 << (((NODE_ID - 1) * 2) + 1);
}

// CMD_ENABLE / CMD_DISABLE effect: gate stream processing + energize motor.
void node_set_enabled(bool on) {
    streamEnabled = on;
    if (on) HAL_MOTOR_ENABLE();
    else    HAL_MOTOR_DISABLE();
}

static int32_t readPositionAtomic() {
    cli();
    int32_t pos = absolutePosition;
    sei();
    return pos;
}

bool node_handle_command(const uint8_t* pkt, uint8_t len,
                         uint8_t* reply, uint8_t* replyLen) {
    (void)len;
    switch (pkt[1]) {
        case CMD_GET_POS: {
            int32_t pos = readPositionAtomic();
            reply[0] = NODE_ID;
            reply[1] = CMD_GET_POS;
            reply[2] = 4;
            reply[3] = (pos >> 24) & 0xFF;
            reply[4] = (pos >> 16) & 0xFF;
            reply[5] = (pos >> 8)  & 0xFF;
            reply[6] =  pos        & 0xFF;
            *replyLen = 8;
            return true;
        }
        default:
            return false;
    }
}

// ─── RX ISR — command framing + stream stepping ─────────────────────────────
ISR(HAL_USART_RXC_vect) {
    uint8_t status = HAL_USART_INST.RXDATAH;
    uint8_t b      = HAL_USART_INST.RXDATAL;

    if (status & 0x01) {            // 9th bit = 1 → command frame
        frame_command_byte(b);
        return;
    }

    frame_stream_reset();           // 9th bit = 0 → stream byte
    if (!streamEnabled) return;

    bool stepReq = (b & stepBitMask) != 0;
    bool newDir  = (b & dirBitMask)  != 0;

    if (newDir != currentDir) {
        if (newDir) HAL_DIR_PORT.OUTSET = HAL_DIR_BM;
        else        HAL_DIR_PORT.OUTCLR = HAL_DIR_BM;
        currentDir = newDir;
        delayMicroseconds(5);       // DM542 DIR-before-STEP setup guard
    }

    if (stepReq) {
        HAL_STEP_PORT.OUTSET = HAL_STEP_BM;
        absolutePosition += (currentDir ? 1 : -1);
        HAL_STEP_TIMER_INST.CCMP  = HAL_STEP_PULSE_CCMP;
        HAL_STEP_TIMER_INST.CNT   = 0;
        HAL_STEP_TIMER_INST.CTRLA = HAL_STEP_TIMER_CLKSEL | HAL_STEP_TIMER_ENABLE_bm;
    }
}

// ─── Step-pulse timer — end of step pulse ───────────────────────────────────
ISR(HAL_STEP_TIMER_vect) {
    HAL_STEP_TIMER_INST.INTFLAGS = HAL_STEP_TIMER_CAPT_bm;
    HAL_STEP_PORT.OUTCLR = HAL_STEP_BM;
    HAL_STEP_TIMER_INST.CTRLA &= ~HAL_STEP_TIMER_ENABLE_bm;
}
