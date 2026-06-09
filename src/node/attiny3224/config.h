#pragma once
#include <Arduino.h>

// RS485
#define RS485_DE_PIN  PIN_PA3
#define NODE_USART    USART1
#define USART_RXC_vect_ USART1_RXC_vect

// Stepper pins — direct port access for ISR speed
#define STEP_PIN      PIN_PB0
#define DIR_PIN       PIN_PB1
#define EN_PIN        PIN_PA4

#define STEP_PORT     PORTB
#define STEP_BM       PIN0_bm
#define DIR_PORT      PORTB
#define DIR_BM        PIN1_bm
#define ENABLE_PORT   PORTA
#define ENABLE_BM     PIN4_bm

// LED
#define LED_PIN       PIN_PA5
#define LED_PORT      PORTA
#define LED_BM        PIN5_bm

// ATtiny: drive TX line high explicitly on init (idle state)
#define USART_TX_IDLE_INIT()  do { pinMode(PIN_PA1, OUTPUT); digitalWrite(PIN_PA1, HIGH); } while(0)

// Step pulse width: 3µs @ 20MHz = 60 cycles
#define STEP_PULSE_CCMP  60

// USART baud register (ATtiny megaTinyCore formula)
#define USART_BAUD_VAL  ((uint16_t)((F_CPU * 64.0) / (16.0 * RS485_BAUD) + 0.5))

// USART init — ATtiny bare-metal
#define USART_INIT() do { \
    NODE_USART.BAUD  = USART_BAUD_VAL; \
    NODE_USART.CTRLC = USART_CHSIZE_9BITH_gc; \
    NODE_USART.CTRLA = USART_RXCIE_bm; \
    NODE_USART.CTRLB = USART_RXEN_bm | USART_TXEN_bm; \
} while(0)

// RS485 direction control — ATtiny has no hardware XDIR, so toggle DE manually.
#define RS485_TX_BEGIN() do { digitalWrite(RS485_DE_PIN, HIGH); delayMicroseconds(10); } while(0)
#define RS485_TX_END()   do { delayMicroseconds(1); digitalWrite(RS485_DE_PIN, LOW); } while(0)

// Driver enable polarity: DM542 — LOW = enabled
#define MOTOR_ENABLE()   ENABLE_PORT.OUTCLR = ENABLE_BM
#define MOTOR_DISABLE()  ENABLE_PORT.OUTSET = ENABLE_BM
