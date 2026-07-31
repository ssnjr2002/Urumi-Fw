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

// ─── Stream slot (runtime-assigned via CMD_ENGAGE) ──────────────────────────
// The stream byte is four 2-bit slots (bit(2n)=step, bit(2n+1)=dir). Which slot
// this node reads is NO LONGER derived from NODE_ID — it is assigned at runtime
// by the Pico's axis map (docs/engage_and_axis_map.md §4). The node boots
// DISENGAGED (slot == SLOT_NONE): masks are 0, so it ignores every stream byte
// and its position freezes until an ENGAGE binds it to a slot.
enum Slot : uint8_t {
    SLOT_X = 0,
    SLOT_Y,
    SLOT_Z,
    SLOT_A,
    SLOT_NONE = 0xFF,
};

// ─── Stepper state ──────────────────────────────────────────────────────────
// slot/masks change at runtime (ENGAGE handler, main-loop context) and are read
// in the RX ISR → volatile.
static volatile int32_t absolutePosition = 0;
static volatile uint8_t slot             = SLOT_NONE;
static volatile uint8_t stepBitMask      = 0;
static volatile uint8_t dirBitMask       = 0;
static bool             currentDir       = false;

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

    // No NODE_ID-derived slot — the node boots disengaged and ignores the stream
    // until CMD_ENGAGE binds it (slot/masks stay at their SLOT_NONE/0 defaults).

#ifdef NODE_HAS_LASER
    // Laser gate (only the one stepper node wired to a laser): boot OFF.
    pinMode(HAL_LASER_PIN, OUTPUT); digitalWrite(HAL_LASER_PIN, LOW);
#endif
}

// CMD_ENABLE / CMD_DISABLE effect: ENERGIZE ONLY — no stream role.
// The stream gate is the slot (ENGAGE), decoupled from holding torque (ENABLE):
// a parked dual-head axis is ENABLED (holds Z height) but DISENGAGED (ignores
// the stream). See docs/engage_and_axis_map.md §4.3.
void node_set_enabled(bool on) {
    if (on) HAL_MOTOR_ENABLE();
    else    HAL_MOTOR_DISABLE();
}

// Stepper does all its work in the RX ISR — nothing to tick each loop.
void node_loop(void) {}

static int32_t readPositionAtomic() {
    cli();
    int32_t pos = absolutePosition;
    sei();
    return pos;
}

// Type-specific status tail: [pos int32 BE][slot]. Lets a host see both what the
// node counted and which stream slot it is ENGAGE-bound to (0xFF = disengaged).
uint8_t node_status(uint8_t* buf) {
    int32_t pos = readPositionAtomic();
    buf[0] = (pos >> 24) & 0xFF;
    buf[1] = (pos >> 16) & 0xFF;
    buf[2] = (pos >> 8)  & 0xFF;
    buf[3] =  pos        & 0xFF;
    buf[4] = slot;
    return 5;
}

bool node_handle_command(const uint8_t* pkt, uint8_t len,
                         uint8_t* reply, uint8_t* replyLen) {
    (void)len;
    switch (pkt[1]) {
        case CMD_ENGAGE: {
            // payload [slot]: 0..3 bind to that stream slot, 0xFF = disengage.
            uint8_t s = pkt[3];
            if (s == SLOT_NONE) {
                stepBitMask = 0;
                dirBitMask  = 0;
            } else if (s <= SLOT_A) {
                stepBitMask = 1 << (s * 2);
                dirBitMask  = 1 << (s * 2 + 1);
            } else {
                return false;              // out-of-range slot → NAK, keep state
            }
            slot = s;
            // ACK carries slot, pos and energised state which currently happens 
            // to be exactly the same as the full state: [type][flags][pos][slot],
            // sampled after the bind. That makes an engage one atomic observation
            // of (bound, position, enabled) — a separate follow-up read could
            // straddle a node reboot and report a position for a slot the node no
            // longer holds. The echoed slot also self-verifies the bind.
            reply[0] = NODE_ID;
            reply[1] = CMD_ENGAGE;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
#ifdef NODE_HAS_LASER
        case CMD_LASER: {
            // payload [state]: 1 = laser on, 0 = off. Compiled only on the laser
            // node; every other stepper NAKs this (falls through to return false).
            if (len < 5) return false;             // [id][cmd][1][state][crc]
            digitalWrite(HAL_LASER_PIN, pkt[3] ? HIGH : LOW);
            reply[0] = NODE_ID;
            reply[1] = CMD_LASER;
            reply[2] = 0;
            *replyLen = 4;
            return true;
        }
#endif
        case CMD_GET_POS: {
            // Same payload as CMD_NODE_STATUS / the ENGAGE ack — position never
            // travels in a shape of its own, so there is one parser on the host
            // side and one place to extend. Kept as a distinct verb only because
            // the direct UPDI debug console asks for it by name.
            reply[0] = NODE_ID;
            reply[1] = CMD_GET_POS;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
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
    if (slot == SLOT_NONE) return;  // disengaged → ignore stream, freeze position

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
