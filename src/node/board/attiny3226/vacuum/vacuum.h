// vacuum.h — ATtiny3226 vacuum hardware binding (the attiny3226 × vacuum cell).
// The pin bindings the portable vacuum logic (types/vacuum/) needs. Board-level
// bus/LED config is in ../board.h.
//
// Ported from the standalone servo_ssr_node.ino sketch. DE is on PA4 (see
// ../board.h), matching the physical board — which leaves PA3 free for the NC
// switch input below.
#pragma once
#include <Arduino.h>

// ─── Servos ─────────────────────────────────────────────────────────────────
// Index 0 is unused so servo commands can be 1-based on the wire.
#define HAL_VACUUM_SERVO_COUNT 6
static const uint8_t HAL_VACUUM_SERVO_PINS[HAL_VACUUM_SERVO_COUNT + 1] = {
    0,          // index 0 unused
    PIN_PB0,    // Servo 1
    PIN_PC0,    // Servo 2
    PIN_PC1,    // Servo 3
    PIN_PB5,    // Servo 4
    PIN_PB4,    // Servo 5
    PIN_PB1,    // Servo 6
};

// ─── SSR (solid-state relay driving the AC pump) ────────────────────────────
#define HAL_VACUUM_SSR_PIN   PIN_PC2

// ─── Status LEDs (RGB) ──────────────────────────────────────────────────────
// HAL_LED_PIN (PA5, blue) is owned by the core for the address blink; the
// vacuum type additionally drives red/green for SSR + servo activity.
#define HAL_VACUUM_LED_RED   PIN_PA6
#define HAL_VACUUM_LED_GREEN PIN_PA7

// ─── NC switch input ────────────────────────────────────────────────────────
// Normally-closed switch on PA3 (free because DE is on PA4). Wired switch→GND
// with the internal pull-up, so: closed/rest = LOW, opened/actuated = HIGH.
#define HAL_VACUUM_SWITCH_PIN   PIN_PA3
