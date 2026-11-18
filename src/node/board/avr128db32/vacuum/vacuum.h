// vacuum.h — AVR128DB32 vacuum hardware binding (the avr128db32 × vacuum cell).
// The pin bindings the portable vacuum logic (types/vacuum/) needs. Board-level
// bus/LED config is in ../board.h.
//
// The second vacuum board, after the ATtiny3226 one. Both use NODE_ID 7, so
// only one may be on the bus.
//
// The board also routes a second "limit switch" on PD7, whose purpose is not
// known yet, so it is left unbound.
#pragma once
#include <Arduino.h>

// ─── Servos ─────────────────────────────────────────────────────────────────
// Index 0 is unused so servo commands can be 1-based on the wire.
#define HAL_VACUUM_SERVO_COUNT 6
static const uint8_t HAL_VACUUM_SERVO_PINS[HAL_VACUUM_SERVO_COUNT + 1] = {
    0,          // index 0 unused
    PIN_PC2,    // Servo 1
    PIN_PC3,    // Servo 2
    PIN_PD1,    // Servo 3
    PIN_PD2,    // Servo 4
    PIN_PD4,    // Servo 5
    PIN_PD6,    // Servo 6
};

// ─── SSR (solid-state relay driving the AC pump) ────────────────────────────
#define HAL_VACUUM_SSR_PIN   PIN_PD5

// ─── Status LEDs ────────────────────────────────────────────────────────────
// None. The one LED (PA3) is the core's address blink. vacuum.cpp leaves the
// activity LEDs out when HAL_VACUUM_LED_RED / _GREEN are undefined.

// ─── Z probe switch input ───────────────────────────────────────────────────
// The tool-height probe switch (docs/tool_probe.md). Normally closed, to
// ground: closed reads LOW and open HIGH, as on the 3226 board.
#define HAL_VACUUM_SWITCH_PIN   PIN_PA2
