// stepper.h — ATtiny3224 stepper hardware binding (the attiny3224 × stepper cell).
// The pin/timer/driver values the portable stepper logic (types/stepper/) needs;
// see board/hal/hal_stepper.h for the contract. Board-level config is in ../board.h.
//
// Defines the HAL_ symbols contracted by hal_stepper.h, then #includes it
// at the end for self-checking completeness.
#pragma once
#include <Arduino.h>

// ─── Step / Dir pins — direct port access for ISR speed ─────────────────────
#define HAL_STEP_PIN      PIN_PB0
#define HAL_DIR_PIN       PIN_PB1
#define HAL_EN_PIN        PIN_PA4

#define HAL_STEP_PORT     PORTB
#define HAL_STEP_BM       PIN0_bm
#define HAL_DIR_PORT      PORTB
#define HAL_DIR_BM        PIN1_bm
#define HAL_ENABLE_PORT   PORTA
#define HAL_ENABLE_BM     PIN4_bm

// ─── Step-pulse one-shot timer (TCB0) ───────────────────────────────────────
// Step pulse width: 3µs @ 20MHz = 60 cycles
#define HAL_STEP_TIMER_INST     TCB0
#define HAL_STEP_TIMER_vect     TCB0_INT_vect
#define HAL_STEP_TIMER_CNTMODE  TCB_CNTMODE_SINGLE_gc
#define HAL_STEP_TIMER_CAPT_bm  TCB_CAPT_bm
#define HAL_STEP_TIMER_CLKSEL   TCB_CLKSEL_CLKDIV1_gc
#define HAL_STEP_TIMER_ENABLE_bm TCB_ENABLE_bm
#define HAL_STEP_PULSE_CCMP     60

// ─── Driver enable polarity: DM542 — LOW = enabled ──────────────────────────
#define HAL_MOTOR_ENABLE()   HAL_ENABLE_PORT.OUTCLR = HAL_ENABLE_BM
#define HAL_MOTOR_DISABLE()  HAL_ENABLE_PORT.OUTSET = HAL_ENABLE_BM

// ─── Driver init ────────────────────────────────────────────────────────────
// Defined in types/stepper/drivers.cpp (compiled for every stepper build).
// ATtiny/DM542: no-op body. DB32/TMC: SPI init.
void drivers_init(void);

// ─── HAL self-check ─────────────────────────────────────────────────────────
// (contract headers removed — stepper.h is a direct provider; consumers
//  #include "stepper/stepper.h" and use the HAL_ symbols directly)
