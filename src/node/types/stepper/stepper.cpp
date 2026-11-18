// stepper.cpp — stepper node type: its own RX ISR (so the stream path inlines
// with no call), the step-pulse one-shot, the position counter, the limit gate
// and the hooks. Board symbols come from board.h and stepper/stepper.h and
// expand to register accesses. See docs/system/nodes.md §6–7.
#include <Arduino.h>
#include "board.h"
#include "stepper/stepper.h"
#include "common.h"
#include "node_hooks.h"
#include "rs485/frame.h"
#include "hall_index.h"
#include "stepper_state.h"
#include "leg.h"

// ─── Stream slot (CMD_ENGAGE) ───────────────────────────────────────────────
// The stream byte is four 2-bit slots (bit(2n)=step, bit(2n+1)=dir), bound at
// run time by the controller (docs/system/bus.md §3). The node boots with no
// slot: masks 0, every stream byte ignored, position frozen.
enum Slot : uint8_t {
    SLOT_X = 0,
    SLOT_Y,
    SLOT_Z,
    SLOT_A,
    SLOT_NONE = 0xFF,
};

// ─── Stepper state ──────────────────────────────────────────────────────────
// slot/masks change at runtime (ENGAGE handler, main-loop context) and are read
// in the RX ISR → volatile. The counter and DIR are shared with leg.cpp
// (stepper_state.h).
volatile int32_t        absolutePosition = 0;
static volatile uint8_t slot             = SLOT_NONE;
static volatile uint8_t stepBitMask      = 0;
static volatile uint8_t dirBitMask       = 0;
bool                    currentDir       = false;

#ifdef HAS_LIMIT_SWITCH
// ─── Limit gate (docs/system/nodes.md §7) ───────────────────────────────────
// While the switch reads pressed, every stream step is refused, both ways;
// driving off the switch is a retract leg. A press that lasts LIMIT_LATCH_MS
// latches the gate shut until a retract ends clear of the switch.
//
// The time base is stream bytes, not steps: a byte is 11 bit times at
// RS485_BAUD, a fixed tick with no timer read in the ISR.
#define LIMIT_STREAM_BYTES_PER_SEC ((uint32_t)RS485_BAUD / 11u)
#define LIMIT_LATCH_MS             500u
#define LIMIT_LATCH_BYTES          (LIMIT_STREAM_BYTES_PER_SEC * LIMIT_LATCH_MS / 1000u)

// Bytes seen while pressed since boot, never reset, so a chattering switch
// shows even when no press latched. The current press is (total − base), with
// base taken at each release.
volatile uint32_t limitBytesAsserted = 0;
volatile uint32_t limitRunBase       = 0;
volatile bool     limitLatched       = false;
#endif  // HAS_LIMIT_SWITCH

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

#ifdef HAS_LIMIT_SWITCH
    // The switch pulls to ground, so a cut wire reads pressed.
    pinMode(HAL_LIMIT_SWITCH_PIN, INPUT_PULLUP);
#endif
#ifdef HAS_HALL_INDEX
    // Same pin, analog, no pull-up.
    hallIndexSetup();
#endif

    // Before the first disable, which on a TMC2660 is an SPI write.
    drivers_init();
    HAL_MOTOR_DISABLE();

    // Step-pulse one-shot timer: pulls STEP low HAL_STEP_PULSE_CCMP cycles
    // after a step.
    HAL_STEP_TIMER_INST.CTRLB   = HAL_STEP_TIMER_CNTMODE;
    HAL_STEP_TIMER_INST.INTCTRL = HAL_STEP_TIMER_CAPT_bm;

#ifdef NODE_HAS_LASER
    // Laser gate: boots off.
    pinMode(HAL_LASER_PIN, OUTPUT); digitalWrite(HAL_LASER_PIN, LOW);
#endif
}

// Energise only. Whether the node steps is the slot: a parked head is energised
// to hold its height but bound to no slot.
void node_set_enabled(bool on) {
    if (on) {
        HAL_MOTOR_ENABLE();
    } else {
#ifdef HAS_HOMING
        // Stop any leg, or it would count steps the motor no longer takes. So
        // the broadcast e-stop stops a leg too.
        legHalt();
#endif
        HAL_MOTOR_DISABLE();
    }
}

// Disengage, as CMD_ENGAGE with SLOT_NONE. The laser is a crosshair and stays
// as commanded.
void node_release(void) {
    slot        = SLOT_NONE;
    stepBitMask = 0;
    dirBitMask  = 0;
}

// The stream works in the RX ISR; loop context finishes legs and publishes the
// flags, which node_set_flag() may only write from here.
void node_loop(void) {
#ifdef HAS_HOMING
    legLoop();
#endif
#ifdef HAS_LIMIT_SWITCH
    // The pin or the latch.
    node_set_flag(NODE_FLAG_LIMIT, HAL_LIMIT_ASSERTED() || limitLatched);
#endif
}


static int32_t readPositionAtomic() {
    cli();
    int32_t pos = absolutePosition;
    sei();
    return pos;
}

// Status tail: [pos int32 BE][slot][homing kind], then [span int32 BE] on a
// build with legs, then the Hall answer on a Hall build. The controller decodes
// on a minimum length, so the longer tails parse everywhere.
uint8_t node_status(uint8_t* buf) {
    int32_t pos = readPositionAtomic();
    buf[0] = (pos >> 24) & 0xFF;
    buf[1] = (pos >> 16) & 0xFF;
    buf[2] = (pos >> 8)  & 0xFF;
    buf[3] =  pos        & 0xFF;
    buf[4] = slot;
    // Which terminator this board has (HOMING_KIND_*, common.h).
#if defined(HAS_HALL_INDEX)
    buf[5] = HOMING_KIND_INDEX;
#elif defined(HAS_LIMIT_SWITCH)
    buf[5] = HOMING_KIND_LIMIT;
#else
    buf[5] = HOMING_KIND_NONE;
#endif
#ifdef HAS_HOMING
    const int32_t span = legSpanSteps();
    buf[6] = (span >> 24) & 0xFF;
    buf[7] = (span >> 16) & 0xFF;
    buf[8] = (span >> 8)  & 0xFF;
    buf[9] =  span        & 0xFF;
#ifdef HAS_HALL_INDEX
    // The index, not where the sweep stopped: a sweep runs through it.
    const int32_t idx = hallIndexPos();
    buf[10] = (idx >> 24) & 0xFF;
    buf[11] = (idx >> 16) & 0xFF;
    buf[12] = (idx >> 8)  & 0xFF;
    buf[13] =  idx        & 0xFF;
    buf[14] = hallIndexCause();
    // Live reading and the last sweep's baseline, to tell NOTFOUND from a
    // sensor that is not wired.
    const int16_t raw  = hallIndexRaw();
    const int16_t base = hallIndexBaseline();
    buf[15] = (raw  >> 8) & 0xFF;
    buf[16] =  raw        & 0xFF;
    buf[17] = (base >> 8) & 0xFF;
    buf[18] =  base       & 0xFF;
    // Measured steps per revolution, and the crossings completed: 0 means the
    // magnet was never seen, fewer than HALL_CROSSINGS that the budget ran out.
    const int32_t spr = hallIndexStepsPerRev();
    buf[19] = (spr >> 24) & 0xFF;
    buf[20] = (spr >> 16) & 0xFF;
    buf[21] = (spr >> 8)  & 0xFF;
    buf[22] =  spr        & 0xFF;
    buf[23] = hallIndexCrossings();
    return 24;
#else
    return 10;
#endif
#else
    return 6;
#endif
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
            // The ack is the status, taken after the bind: slot, position and
            // energised in one reply, which a separate read could not promise
            // across a reboot.
            reply[0] = NODE_ID;
            reply[1] = CMD_ENGAGE;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
#ifdef NODE_HAS_LASER
        case CMD_LASER: {
            // payload [state]: 1 = laser on, 0 = off.
            if (len < 5) return false;             // [id][cmd][1][state][crc]
            digitalWrite(HAL_LASER_PIN, pkt[3] ? HIGH : LOW);
            reply[0] = NODE_ID;
            reply[1] = CMD_LASER;
            reply[2] = 0;
            *replyLen = 4;
            return true;
        }
#endif
#ifdef HAS_HOMING
        case CMD_HOME_LEG: {
            // [id][cmd][len][11 payload][crc]. Too short is BAD_ARG: the opcode
            // is supported, the bytes are wrong.
            if (len < 3 + CMD_HOME_LEG_PAYLOAD_LEN + 1) {
                node_reply_nak(CMD_HOME_LEG, NAK_BAD_ARG, reply, replyLen);
                return true;
            }

            const uint8_t* p = &pkt[3];
            const bool     dir             = (p[0] & 0x01) != 0;
            const uint16_t startUs   = ((uint16_t)p[1] << 8) | p[2];
            const uint16_t floorUs   = ((uint16_t)p[3] << 8) | p[4];
            const uint16_t rampSteps = ((uint16_t)p[5] << 8) | p[6];
            const uint32_t maxSteps  = ((uint32_t)p[7]  << 24) |
                                       ((uint32_t)p[8]  << 16) |
                                       ((uint32_t)p[9]  <<  8) |
                                        (uint32_t)p[10];

#ifdef HAS_LIMIT_SWITCH
            const bool intendedRetract = (p[0] & 0x02) != 0;

            // The mode: one pin read, now. On the switch, the move is off it.
            const bool    retract = HAL_LIMIT_ASSERTED();
            const LegMode mode    = retract ? LEG_RETRACT : LEG_SEEK;

            // The intent bit is checked against the read, not used for it. A
            // seek's budget is a runaway cap; a retract would run all of it.
            if (retract != intendedRetract) {
                node_reply_nak(CMD_HOME_LEG, NAK_INTENT_MISMATCH, reply, replyLen);
                return true;
            }
#else
            // Rotary: one kind of leg, and the intent bit is ignored.
            const LegMode mode = LEG_SWEEP;
#endif

            const uint8_t why = legArm(mode, dir, startUs, floorUs,
                                       rampSteps, maxSteps);
            if (why) {
                node_reply_nak(CMD_HOME_LEG, why, reply, replyLen);
                return true;
            }

            // The ack is the status, taken after the arm.
            reply[0] = NODE_ID;
            reply[1] = CMD_HOME_LEG;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
        case CMD_PARK_LEG: {
            // [id][cmd][len][10 payload][crc].
            if (len < 3 + CMD_PARK_LEG_PAYLOAD_LEN + 1) {
                node_reply_nak(CMD_PARK_LEG, NAK_BAD_ARG, reply, replyLen);
                return true;
            }
            const uint8_t* p = &pkt[3];
            const int32_t  target    = (int32_t)(((uint32_t)p[0] << 24) |
                                                 ((uint32_t)p[1] << 16) |
                                                 ((uint32_t)p[2] <<  8) |
                                                  (uint32_t)p[3]);
            const uint16_t startUs   = ((uint16_t)p[4] << 8) | p[5];
            const uint16_t floorUs   = ((uint16_t)p[6] << 8) | p[7];
            const uint16_t rampSteps = ((uint16_t)p[8] << 8) | p[9];

            // A counter with a broken witness is not a position.
            if (!node_flag(NODE_FLAG_DATUM)) {
                node_reply_nak(CMD_PARK_LEG, NAK_NO_DATUM, reply, replyLen);
                return true;
            }
#ifdef HAS_LIMIT_SWITCH
            // A park starts clear of the switch; on it, the switch would stop
            // the leg before its first step whichever way it went.
            if (HAL_LIMIT_ASSERTED() || limitLatched) {
                node_reply_nak(CMD_PARK_LEG, NAK_INTENT_MISMATCH, reply, replyLen);
                return true;
            }
#endif
            const int32_t pos = readPositionAtomic();
            if (target != pos) {
                const bool     dir   = target > pos;
                const uint32_t steps = dir ? (uint32_t)(target - pos)
                                           : (uint32_t)(pos - target);
                const uint8_t why = legArm(LEG_PARK, dir, startUs, floorUs,
                                           rampSteps, steps);
                if (why) {
                    node_reply_nak(CMD_PARK_LEG, why, reply, replyLen);
                    return true;
                }
            }
            // Already there: nothing armed, and the ack's NODE_FLAG_LEG is clear.
            reply[0] = NODE_ID;
            reply[1] = CMD_PARK_LEG;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
        case CMD_LEG_ABORT: {
            // Stops the pulser only; the motor stays energised, so the counter
            // and the datum stay good. Guarded: halting an idle pulser would
            // re-run the last leg's finish.
            if (legActive) legHalt();
            reply[0] = NODE_ID;
            reply[1] = CMD_LEG_ABORT;
            uint8_t n = buildNodeStatus(&reply[3]);
            reply[2] = n;
            *replyLen = 3 + n + 1;
            return true;
        }
#endif
        case CMD_GET_POS: {
            // The status, as CMD_NODE_STATUS.
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
    if (frame_rx_reject(status)) return;

    if (status & 0x01) {            // 9th bit = 1 → command frame
        frame_command_byte(b);
        return;
    }

    frame_stream_reset();           // 9th bit → stream byte
    if (slot == SLOT_NONE) return;  // disengaged → ignore stream, freeze position

#ifdef HAS_HOMING
    // A leg owns the axis. The bus's empty preamble bytes must not move DIR or
    // feed the limit count while it runs.
    if (legActive) return;
#endif

#ifdef HAS_LIMIT_SWITCH
    // Refused at once; the count decides only whether the refusal latches.
    const bool limAsserted = HAL_LIMIT_ASSERTED();
    if (limAsserted) {
        if (++limitBytesAsserted - limitRunBase >= LIMIT_LATCH_BYTES)
            limitLatched = true;
    } else {
        limitRunBase = limitBytesAsserted;
    }
    if (limAsserted || limitLatched) return;
#endif

    // DIR moves only on a byte that steps this axis: every byte carries a dir
    // bit for every slot.
    if ((b & stepBitMask) == 0) return;

    bool newDir = (b & dirBitMask) != 0;
    if (newDir != currentDir) {
        if (newDir) HAL_DIR_PORT.OUTSET = HAL_DIR_BM;
        else        HAL_DIR_PORT.OUTCLR = HAL_DIR_BM;
        currentDir = newDir;
        delayMicroseconds(5);       // DM542 DIR-before-STEP setup guard
    }

    HAL_STEP_PORT.OUTSET = HAL_STEP_BM;
    absolutePosition += (currentDir ? 1 : -1);
    HAL_STEP_TIMER_INST.CCMP  = HAL_STEP_PULSE_CCMP;
    HAL_STEP_TIMER_INST.CNT   = 0;
    HAL_STEP_TIMER_INST.CTRLA = HAL_STEP_TIMER_CLKSEL | HAL_STEP_TIMER_ENABLE_bm;
}

// ─── Step-pulse timer — end of step pulse ───────────────────────────────────
ISR(HAL_STEP_TIMER_vect) {
    HAL_STEP_TIMER_INST.INTFLAGS = HAL_STEP_TIMER_CAPT_bm;
    HAL_STEP_PORT.OUTCLR = HAL_STEP_BM;
    HAL_STEP_TIMER_INST.CTRLA &= ~HAL_STEP_TIMER_ENABLE_bm;
}
