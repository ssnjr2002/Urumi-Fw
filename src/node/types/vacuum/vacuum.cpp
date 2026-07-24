// vacuum.cpp — vacuum node type (6 servos + a soft-started AC pump on an SSR).
// Ported from the standalone servo_ssr_node.ino sketch. Everything the sketch
// re-implemented itself — the 9-bit RX ISR, TX/DE handling, CRC, command
// framing, PING — is provided by the type-agnostic node core, so this file keeps
// only the vacuum-specific logic and wires it to the four core↔type hooks.
//
// The RX ISR comes from rs485/isr_generic.cpp (compiled for every non-stepper
// type); board pins come from the HAL binding (board/<mcu>/vacuum/vacuum.h via
// vacuum/vacuum.h). CMD_SERVO_SET / CMD_SSR_SET dispatch through
// node_handle_command; the SSR burst-fire / soft-start machine is ticked from
// node_loop.
#include <Arduino.h>
#include <Servo.h>
#include "board.h"
#include "vacuum/vacuum.h"
#include "common.h"
#include "node_hooks.h"

// Guard against an env that compiles this type dir with the wrong identity flag.
#ifdef NODE_TYPE
static_assert(NODE_TYPE == NODE_TYPE_VACUUM,
              "vacuum.cpp compiled with a non-vacuum -DNODE_TYPE");
#endif

// ─── Hooks: identity ────────────────────────────────────────────────────────
uint8_t node_type(void) { return NODE_TYPE_VACUUM; }

// ─── Servos (angle-controlled, 1-based to match the wire) ───────────────────
// Each channel is a real RC servo driven via the Servo lib (50 Hz 1–2 ms pulse),
// NOT a plain on/off GPIO. Index 0 is unused so servo commands stay 1-based;
// servoAngle[] tracks the last commanded angle for the green idle LED.
static Servo   servos[HAL_VACUUM_SERVO_COUNT + 1];
static uint8_t servoAngle[HAL_VACUUM_SERVO_COUNT + 1];

static void servoWriteAngle(uint8_t idx, uint8_t angle) {
    if (angle > 180) angle = 180;
    servos[idx].write(angle);
    servoAngle[idx] = angle;
}

// Green = idle: lit only while every servo is parked at 0°.
static void servoUpdateLed(void) {
    bool anyOn = false;
    for (uint8_t i = 1; i <= HAL_VACUUM_SERVO_COUNT; i++)
        if (servoAngle[i]) { anyOn = true; break; }
    digitalWrite(HAL_VACUUM_LED_GREEN, anyOn ? LOW : HIGH);
}

// ─── SSR soft-start (non-blocking integral-cycle control) ───────────────────
// An AC solid-state relay only switches at a mains zero-crossing, so the finest
// control unit is one whole mains half-cycle (~10 ms @ 50 Hz). We approximate a
// duty by firing N whole cycles out of every WINDOW_CYCLES-cycle window, spread
// evenly with a Bresenham accumulator. Soft-start ramps the duty from
// START_DUTY_PCT up to END_DUTY_PCT over RAMP_DURATION_MS to limit pump inrush.
#define WINDOW_CYCLES     10
#define CYCLE_MS          10UL
#define START_DUTY_PCT    40.0f
#define END_DUTY_PCT      100.0f
#define RAMP_DURATION_MS  5000UL

enum SSRState : uint8_t { SSR_OFF, SSR_RAMP, SSR_FULL };
static SSRState      ssrState     = SSR_OFF;
static unsigned long ssrRampStart = 0;
static unsigned long lastSlotTime = 0;
static uint8_t       windowSlot   = 0;
static int           cyclesOn     = 0;
static int           bresErr      = 0;

static void ssrUpdate() {
    if (ssrState == SSR_OFF) return;

    // TODO(zcd): this slot clock is free-running on millis(), not locked to
    // mains. With the reserved HAL_VACUUM_ZCD_PIN wired to a zero-cross detector,
    // advance a slot on each zero-cross edge instead of on the CYCLE_MS timer so
    // firing aligns to real half-cycles. Everything below stays the same.
    unsigned long now = millis();
    if ((now - lastSlotTime) < CYCLE_MS) return;
    lastSlotTime = now;

    if (windowSlot == 0) {
        if (ssrState == SSR_RAMP) {
            unsigned long elapsed = now - ssrRampStart;
            if (elapsed >= RAMP_DURATION_MS) {
                ssrState = SSR_FULL;
                cyclesOn = WINDOW_CYCLES;
            } else {
                float progress = (float)elapsed / (float)RAMP_DURATION_MS;
                float duty = START_DUTY_PCT + progress * (END_DUTY_PCT - START_DUTY_PCT);
                cyclesOn = (int)round(duty / 100.0f * (float)WINDOW_CYCLES);
                cyclesOn = constrain(cyclesOn, 0, WINDOW_CYCLES);
            }
        } else {
            cyclesOn = WINDOW_CYCLES;
        }
        bresErr = WINDOW_CYCLES / 2;
    }

    bresErr += cyclesOn;
    if (bresErr >= WINDOW_CYCLES) {
        digitalWrite(HAL_VACUUM_SSR_PIN, HIGH);
        bresErr -= WINDOW_CYCLES;
    } else {
        digitalWrite(HAL_VACUUM_SSR_PIN, LOW);
    }

    windowSlot = (windowSlot + 1) % WINDOW_CYCLES;
}

static void ssrStart() {
    ssrState     = SSR_RAMP;
    ssrRampStart = millis();
    windowSlot   = 0;
    lastSlotTime = 0;
    bresErr      = 0;
    digitalWrite(HAL_VACUUM_LED_RED, HIGH);
}

static void ssrStop() {
    ssrState   = SSR_OFF;
    windowSlot = 0;
    digitalWrite(HAL_VACUUM_SSR_PIN, LOW);
    digitalWrite(HAL_VACUUM_LED_RED, LOW);
}

// ─── Hooks: setup ───────────────────────────────────────────────────────────
void node_setup(void) {
    for (uint8_t i = 1; i <= HAL_VACUUM_SERVO_COUNT; i++) {
        servos[i].attach(HAL_VACUUM_SERVO_PINS[i]);
        servoWriteAngle(i, 0);              // park at 0°
    }
    pinMode(HAL_VACUUM_SSR_PIN, OUTPUT);   digitalWrite(HAL_VACUUM_SSR_PIN, LOW);
    pinMode(HAL_VACUUM_LED_RED, OUTPUT);   digitalWrite(HAL_VACUUM_LED_RED, LOW);
    pinMode(HAL_VACUUM_LED_GREEN, OUTPUT); digitalWrite(HAL_VACUUM_LED_GREEN, HIGH);
    pinMode(HAL_VACUUM_SWITCH_PIN, INPUT_PULLUP);   // NC switch → GND
}

// ─── Hooks: CMD_ENABLE / CMD_DISABLE effect ─────────────────────────────────
// "Enabled" for a vacuum node means the pump runs (soft-started); "disabled"
// stops it. Per-servo control is independent, via CMD_SERVO_SET.
void node_set_enabled(bool on) {
    if (on) ssrStart();
    else    ssrStop();
}

// ─── Hooks: per-loop tick ───────────────────────────────────────────────────
void node_loop(void) {
    ssrUpdate();
}

// Type-specific status tail: [servo-active bits][ssr state]. Bit i of the first
// byte = servo (i+1) is off-park (angle > 0); second byte = ssrState (0 off,
// 1 ramp, 2 full).
uint8_t node_status(uint8_t* buf) {
    uint8_t bits = 0;
    for (uint8_t i = 1; i <= HAL_VACUUM_SERVO_COUNT; i++)
        if (servoAngle[i]) bits |= (1 << (i - 1));
    buf[0] = bits;
    buf[1] = (uint8_t)ssrState;
    return 2;
}

// ─── Hooks: type-specific commands ──────────────────────────────────────────
// Reply convention (see dispatch.cpp): reply[] = [id][cmd][payloadLen][payload…];
// replyLen counts through the trailing CRC slot, which the core fills in.
bool node_handle_command(const uint8_t* pkt, uint8_t len,
                         uint8_t* reply, uint8_t* replyLen) {
    switch (pkt[1]) {
        case CMD_SERVO_SET: {
            // [dest][cmd][len=2][idx(0=all,1..N)][angle(0..180)][crc]
            if (len < 6) return false;
            uint8_t idx   = pkt[3];
            uint8_t angle = pkt[4];
            if (idx == 0) {
                for (uint8_t i = 1; i <= HAL_VACUUM_SERVO_COUNT; i++)
                    servoWriteAngle(i, angle);
            } else if (idx <= HAL_VACUUM_SERVO_COUNT) {
                servoWriteAngle(idx, angle);
            }
            servoUpdateLed();
            reply[0] = NODE_ID; reply[1] = CMD_SERVO_SET; reply[2] = 0;
            *replyLen = 4;
            return true;
        }
        case CMD_SSR_SET: {
            // [dest][cmd][len=1][state][crc]
            if (len < 5) return false;
            if (pkt[3]) ssrStart();
            else        ssrStop();
            reply[0] = NODE_ID; reply[1] = CMD_SSR_SET; reply[2] = 0;
            *replyLen = 4;
            return true;
        }
        case CMD_SWITCH_GET: {
            // [dest][cmd][len=0][crc] → reply [id][cmd][1][level][crc]
            uint8_t level = digitalRead(HAL_VACUUM_SWITCH_PIN) ? 1 : 0;
            reply[0] = NODE_ID; reply[1] = CMD_SWITCH_GET; reply[2] = 1;
            reply[3] = level;
            *replyLen = 5;
            return true;
        }
        default:
            return false;
    }
}
