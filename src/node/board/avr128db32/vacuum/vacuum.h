// vacuum.h — AVR128DB32 vacuum hardware binding (the avr128db32 × vacuum cell).
// The pin bindings the portable vacuum logic (types/vacuum/) needs. Board-level
// bus/LED config is in ../board.h.
//
// This is the SECOND vacuum board. It supersedes the ATtiny3226 one
// (board/attiny3226/vacuum/vacuum.h) but does not retire it — both cells stay in
// the tree while this revision is in development. They share NODE_ID 7 and are
// therefore mutually exclusive on the bus; only one may be powered at a time.
//
// What changed from the 3226 board, and why it matters here:
//   - external 24 MHz crystal (the 3226 board ran the 16 MHz internal osc)
//   - hardware XDIR on PF3 drives DE, so RS485_USE_XDIR applies — the 3226 had
//     to toggle DE in software
//   - ONE debug LED (PA3) instead of an RGB triple, so the SSR/servo activity
//     colours are gone; see ../board.h's BOARD_DB32_VACUUM block and the
//     HAL_VACUUM_LED_* absence below
//   - the probe switch moved PA3 -> PA2 (PA3 is the LED here)
//
// The board also routes a second "limit switch" on PD7. Its purpose on a
// non-motion node is unclear and is being clarified with the hardware team, so
// it is deliberately left unbound — no pinMode, no protocol surface. Do not
// wire it up on a guess.
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
// Intentionally absent. This board has a single LED (PA3), and the core already
// owns it for the address blink (HAL_LED_PIN, ../board.h). One LED cannot carry
// address + SSR state + servo state honestly, so the activity indication is
// dropped rather than multiplexed. vacuum.cpp compiles the LED writes out when
// HAL_VACUUM_LED_RED / HAL_VACUUM_LED_GREEN are undefined — that absence IS the
// capability test, so do not define them here to "keep the build symmetrical".
//   #define HAL_VACUUM_LED_RED   — not on this board
//   #define HAL_VACUUM_LED_GREEN — not on this board

// ─── Z probe switch input ───────────────────────────────────────────────────
// The bed-floor tool-height probe switch (docs/tool_probe.md). NC, wired
// switch->GND against the internal pull-up, so: closed/rest = LOW,
// opened/actuated = HIGH — same sense as the 3226 board, so CMD_SWITCH_GET's
// reply byte means the same thing on both. common.h documents that byte as a
// raw PA3 read; on this board it is PA2.
#define HAL_VACUUM_SWITCH_PIN   PIN_PA2
