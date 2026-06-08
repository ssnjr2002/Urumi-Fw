#pragma once
#include <Arduino.h>

// RS485
#define RS485_DE_PIN  PIN_PF3
#define NODE_USART    USART2
#define USART_RXC_vect_ USART2_RXC_vect

// Stepper pins — direct port access for ISR speed
#define STEP_PIN      PIN_PD4
#define DIR_PIN       PIN_PD5
#define EN_PIN        PIN_PD6

#define STEP_PORT     PORTD
#define STEP_BM       PIN4_bm
#define DIR_PORT      PORTD
#define DIR_BM        PIN5_bm
#define ENABLE_PORT   PORTD
#define ENABLE_BM     PIN6_bm

// RGB LED — LED_PIN maps to red for protocol-compatible single-LED behaviour
#define LED_PIN       PIN_PF2
#define LED_RED_PIN   PIN_PF2
#define LED_GREEN_PIN PIN_PF4
#define LED_BLUE_PIN  PIN_PF5

// DxCore does not set pin direction when using bare-metal USART registers.
// PF0 (TX) must be explicitly driven high before USART takes over, otherwise
// it stays high-impedance and the bus never sees a valid idle state.
#define USART_TX_IDLE_INIT() do { \
    pinMode(PIN_PF0, OUTPUT); \
    pinMode(PIN_PF1, INPUT); \
} while(0)

// Step pulse width: 3µs @ 24MHz = 72 cycles
#define STEP_PULSE_CCMP  72

// USART baud register
#define USART_BAUD_VAL  ((uint16_t)((F_CPU * 64.0) / (16.0 * RS485_BAUD) + 0.5))

// USART init — AVR DB bare-metal (USART2 on PF0/PF1)
// PORTMUX default routes USART2 to PF0(TX)/PF1(RX) — no PORTMUX change needed.
#define USART_INIT() do { \
    NODE_USART.BAUD  = USART_BAUD_VAL; \
    NODE_USART.CTRLC = USART_CHSIZE_9BITH_gc; \
    NODE_USART.CTRLA = USART_RXCIE_bm; \
    NODE_USART.CTRLB = USART_RXEN_bm | USART_TXEN_bm; \
} while(0)

// TMC2660 SPI
#ifdef TMC_2660
#define TMC_CS_PIN        PIN_PA7
#define TMC_CURRENT       2000
#define TMC_MICROSTEPPING 32
#define TMC_R_SENSE       0.1f
#endif

// DRV8825 microstepping pins
#ifdef DRV8825
#define DRV_M0_PIN        PIN_PA4
#define DRV_M1_PIN        PIN_PA6
#define DRV_M2_PIN        PIN_PA7
#define DRV_MICROSTEPPING 32
#endif

// Driver enable polarity
#if defined(DM542)
#define MOTOR_ENABLE()   ENABLE_PORT.OUTCLR = ENABLE_BM  // LOW = enabled
#define MOTOR_DISABLE()  ENABLE_PORT.OUTSET = ENABLE_BM
#elif defined(DRV8825)
#define MOTOR_ENABLE()   ENABLE_PORT.OUTSET = ENABLE_BM  // HIGH = enabled
#define MOTOR_DISABLE()  ENABLE_PORT.OUTCLR = ENABLE_BM
#elif defined(TMC_2660)
// Enable/disable via SPI in drivers.cpp; EN pin not used for power state
#define MOTOR_ENABLE()   drivers_enable()
#define MOTOR_DISABLE()  drivers_disable()
#endif

// Thermistor / limit switch
#define LIMIT_SWITCH_PIN PIN_PD1
#define THERMISTOR_PIN   PIN_PD2
