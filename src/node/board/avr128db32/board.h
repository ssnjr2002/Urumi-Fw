// board.h — AVR128DB32 board-level config (type-agnostic).
// RS485 / USART / LED / clock — needed by every node type on this board.
// Stepper-specific pins/drivers live in board/avr128db32/stepper/.
//
// Defines the HAL_ symbols contracted by board/hal/hal.h, then #includes it
// at the end for self-checking completeness.
#pragma once
#include <Arduino.h>

// ─── RS485 / USART ──────────────────────────────────────────────────────────
#define HAL_RS485_DE_PIN    PIN_PF3
#define HAL_USART_INST      USART2
#define HAL_USART_RXC_vect  USART2_RXC_vect

// DxCore does not set pin direction when using bare-metal USART registers.
// PF0 (TX) must be explicitly driven high before USART takes over, otherwise
// it stays high-impedance and the bus never sees a valid idle state.
#define HAL_USART_TX_IDLE_INIT() do { \
    pinMode(PIN_PF0, OUTPUT); \
    pinMode(PIN_PF1, INPUT); \
} while(0)

// USART baud register. RS485_BAUD from common.h, resolved at the expansion site.
#define HAL_USART_BAUD_VAL  ((uint16_t)((F_CPU * 64.0) / (16.0 * RS485_BAUD) + 0.5))

// RS485 direction control.
// USART2's default mux puts XDIR on PF3 — the same pin as HAL_RS485_DE_PIN — so
// with hardware XDIR the USART drives DE automatically (hardware-timed, no
// collision). Toggle via -DRS485_USE_XDIR build flag; without it, fall back to
// manual toggle.
#ifdef RS485_USE_XDIR
#define HAL_USART_RS485_CFG  USART_RS485_ENABLE_gc
#define HAL_RS485_TX_BEGIN() do {} while(0)
#define HAL_RS485_TX_END()   do {} while(0)
#else
#define HAL_USART_RS485_CFG  0
#define HAL_RS485_TX_BEGIN() do { digitalWrite(HAL_RS485_DE_PIN, HIGH); delayMicroseconds(10); } while(0)
#define HAL_RS485_TX_END()   do { delayMicroseconds(1); digitalWrite(HAL_RS485_DE_PIN, LOW); } while(0)
#endif

// USART init — AVR DB bare-metal (USART2 on PF0/PF1)
// PORTMUX default routes USART2 to PF0(TX)/PF1(RX) — no PORTMUX change needed.
#define HAL_USART_INIT() do { \
    HAL_USART_INST.BAUD  = HAL_USART_BAUD_VAL; \
    HAL_USART_INST.CTRLC = USART_CHSIZE_9BITH_gc; \
    HAL_USART_INST.CTRLA = USART_RXCIE_bm | HAL_USART_RS485_CFG; \
    HAL_USART_INST.CTRLB = USART_RXEN_bm | USART_TXEN_bm; \
} while(0)

// ─── Debug console USART (opt-in, -DNODE_DEBUG_CONSOLE only) ─────────────────
// RS485 is USART2, so the free USART wired to this board's USB-serial adapter is
// USART1 (PC0 TX / PC1 RX) — DxCore's `Serial1`. Used by debug_console.cpp.
#define HAL_DEBUG_SERIAL   Serial1

// ─── LED ────────────────────────────────────────────────────────────────────
// Not uniform across the DB32 boards, so it is selected by board flag rather
// than assumed. -DBOARD_DB32_VACUUM marks the vacuum revision, which carries a
// single debug LED on PA3; every other DB32 board carries the RGB triple on
// PF2/PF4/PF5. The flag has to live here (not in the vacuum type's cell header)
// because the core's address blink in main.cpp sees only board.h.
#ifdef BOARD_DB32_VACUUM
#define HAL_LED_PIN       PIN_PA3
#else
// HAL_LED_PIN maps to red for protocol-compatible single-LED behaviour.
#define HAL_LED_PIN       PIN_PF2
#define HAL_LED_RED_PIN   PIN_PF2
#define HAL_LED_GREEN_PIN PIN_PF4
#define HAL_LED_BLUE_PIN  PIN_PF5
#define HAL_HAS_RGB_LED   // expose RGB channel symbols
#endif

// ─── HAL self-check ─────────────────────────────────────────────────────────
// (contract headers removed — board.h is a direct provider; consumers
//  #include "board.h" and use the HAL_ symbols directly)
