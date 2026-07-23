// board.h — ATtiny3226 board-level config (type-agnostic).
// RS485 / USART / LED / clock — needed by every node type on this board.
// Type-specific pins live in board/attiny3226/<type>/<type>.h.
//
// The 3226 is the SOIC-20 sibling of the 3224 (same tinyAVR-2 peripherals, more
// I/O). We use it for pin-hungry types like the 6-servo + SSR vacuum node that
// do not fit the 3224's SOIC-14. The bus/LED bindings are kept identical to the
// 3224 board so every node on the fleet shares the same RS485 wiring:
//   TX PA1, RX PA2, DE PA3, LED PA5.
#pragma once
#include <Arduino.h>

// ─── RS485 / USART ──────────────────────────────────────────────────────────
#define HAL_RS485_DE_PIN    PIN_PA3
#define HAL_USART_INST      USART1
#define HAL_USART_RXC_vect  USART1_RXC_vect

// ATtiny: drive TX line high explicitly on init (idle state)
#define HAL_USART_TX_IDLE_INIT()  do { pinMode(PIN_PA1, OUTPUT); digitalWrite(PIN_PA1, HIGH); } while(0)

// USART baud register (ATtiny megaTinyCore formula). RS485_BAUD from common.h,
// resolved at the macro-expansion site (HAL_USART_INIT callers include common.h).
#define HAL_USART_BAUD_VAL  ((uint16_t)((F_CPU * 64.0) / (16.0 * RS485_BAUD) + 0.5))

// USART init — ATtiny bare-metal (no hardware XDIR support)
#define HAL_USART_INIT() do { \
    HAL_USART_INST.BAUD  = HAL_USART_BAUD_VAL; \
    HAL_USART_INST.CTRLC = USART_CHSIZE_9BITH_gc; \
    HAL_USART_INST.CTRLA = USART_RXCIE_bm; \
    HAL_USART_INST.CTRLB = USART_RXEN_bm | USART_TXEN_bm; \
} while(0)

// RS485 direction control — ATtiny has no hardware XDIR, so toggle DE manually.
#define HAL_RS485_TX_BEGIN() do { digitalWrite(HAL_RS485_DE_PIN, HIGH); delayMicroseconds(10); } while(0)
#define HAL_RS485_TX_END()   do { delayMicroseconds(1); digitalWrite(HAL_RS485_DE_PIN, LOW); } while(0)

// ─── LED ────────────────────────────────────────────────────────────────────
// The vacuum node board carries an RGB status LED (PA5/PA6/PA7). The core's
// address-blink uses the single HAL_LED_PIN; the extra two colours are driven by
// the vacuum type directly (see board/attiny3226/vacuum/vacuum.h).
#define HAL_LED_PIN   PIN_PA5
#define HAL_LED_PORT  PORTA
#define HAL_LED_BM    PIN5_bm
#define HAL_LED_DIRECT_PORT   // expose direct-port fast-toggle symbols
