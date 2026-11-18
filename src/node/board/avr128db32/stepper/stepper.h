// stepper.h — AVR128DB32 stepper hardware binding (the avr128db32 × stepper cell).
// Pins/timer plus the driver-chip selection (DM542 / DRV8825 / TMC2660) chosen
// per env by build flags, set up in types/stepper/drivers.cpp. Board-level
// config is in ../board.h.
#pragma once
#include <Arduino.h>

// ─── Step / Dir pins — direct port access for ISR speed ─────────────────────
#define HAL_STEP_PIN      PIN_PD4
#define HAL_DIR_PIN       PIN_PD5
#define HAL_EN_PIN        PIN_PD6

#define HAL_STEP_PORT     PORTD
#define HAL_STEP_BM       PIN4_bm
#define HAL_DIR_PORT      PORTD
#define HAL_DIR_BM        PIN5_bm
#define HAL_ENABLE_PORT   PORTD
#define HAL_ENABLE_BM     PIN6_bm

// ─── Step-pulse one-shot timer (TCB0) ───────────────────────────────────────
// Step pulse width: 3µs @ 24MHz = 72 cycles
#define HAL_STEP_TIMER_INST      TCB0
#define HAL_STEP_TIMER_vect      TCB0_INT_vect
#define HAL_STEP_TIMER_CNTMODE   TCB_CNTMODE_SINGLE_gc
#define HAL_STEP_TIMER_CAPT_bm   TCB_CAPT_bm
#define HAL_STEP_TIMER_CLKSEL    TCB_CLKSEL_CLKDIV1_gc
#define HAL_STEP_TIMER_ENABLE_bm TCB_ENABLE_bm
#define HAL_STEP_PULSE_CCMP      72

// ─── TMC2660 SPI config (driver-internal, not HAL contract) ─────────────────
#ifdef TMC_2660
#define TMC_CS_PIN            PIN_PA7
#ifndef TMC_CURRENT
    #define TMC_CURRENT       1000 // in mA
#endif
#ifndef TMC_MICROSTEPPING
    #define TMC_MICROSTEPPING 32
#endif
#ifndef TMC_R_SENSE
    #define TMC_R_SENSE       0.1f
#endif
#endif

// ─── DRV8825 microstepping pins (driver-internal, not HAL contract) ─────────
#ifdef DRV8825
#define DRV_M0_PIN        PIN_PA4
#define DRV_M1_PIN        PIN_PA6
#define DRV_M2_PIN        PIN_PA7
#ifndef DRV_MICROSTEPPING
    #define DRV_MICROSTEPPING 32
#endif
#endif

// ─── Driver enable polarity (driver-chip-dependent) ─────────────────────────
#if defined(DM542)
#define HAL_MOTOR_ENABLE()   HAL_ENABLE_PORT.OUTCLR = HAL_ENABLE_BM  // LOW = enabled
#define HAL_MOTOR_DISABLE()  HAL_ENABLE_PORT.OUTSET = HAL_ENABLE_BM
#elif defined(DRV8825)
#define HAL_MOTOR_ENABLE()  HAL_ENABLE_PORT.OUTCLR = HAL_ENABLE_BM
#define HAL_MOTOR_DISABLE()   HAL_ENABLE_PORT.OUTSET = HAL_ENABLE_BM  // HIGH = disabled
#elif defined(TMC_2660)
void drivers_enable();
void drivers_disable();
#define HAL_MOTOR_ENABLE()   drivers_enable()
#define HAL_MOTOR_DISABLE()  drivers_disable()
#endif

// ─── Optional peripherals ───────────────────────────────────────────────────
// Limit switch. Port symbols as well as the pin: the stream path reads it in
// the RX ISR on every step, so it must be one IN, not a digitalRead(). Wired to
// ground against the internal pull-up, so pressed reads LOW and a cut wire
// reads pressed. See docs/system/nodes.md §7.

#ifdef HAS_LIMIT_SWITCH
    #define HAL_LIMIT_SWITCH_PIN PIN_PD1
    #define HAL_LIMIT_PORT       PORTD
    #define HAL_LIMIT_BM         PIN1_bm
    #define HAL_LIMIT_PINCTRL    PORTD.PIN1CTRL

    // Polarity depends on how this node's switch is wired. Wrong, the gate runs
    // backwards, and the limit reads 1 released and 0 pressed.
    #ifdef LIMIT_ACTIVE_HIGH
    #define HAL_LIMIT_ASSERTED() ((HAL_LIMIT_PORT.IN & HAL_LIMIT_BM) != 0)
    #else
    #define HAL_LIMIT_ASSERTED() ((HAL_LIMIT_PORT.IN & HAL_LIMIT_BM) == 0)
    #endif
#endif

// Hall index (rotary axes). A magnet on the output side passes a fixed A1324
// whose analog output dips once per output revolution; the dip's centre is the
// index. See docs/rotary_a_axis.md.
//
// PD1 again: the same pin ends either kind of axis's leg, and an axis is one
// kind.
#ifdef HAS_HALL_INDEX
    #if defined(HAS_LIMIT_SWITCH)
    #error "HAS_HALL_INDEX and HAS_LIMIT_SWITCH both claim PD1 — an axis is one or the other"
    #endif
    #define HAL_HALL_PIN     PIN_PD1
    #define HAL_HALL_MUXPOS  ADC_MUXPOS_AIN1_gc    // PD1 = AIN1 on AVR128DB32
#endif

// Either terminator runs the same leg pulser; only what ends the leg differs.
#if defined(HAS_LIMIT_SWITCH) || defined(HAS_HALL_INDEX)
#define HAS_HOMING 1
#endif


// PD2 is the thermistor input, or the laser's on/off gate on the laser build
// (-DNODE_HAS_LASER, CMD_LASER).
#ifdef NODE_HAS_LASER
#define HAL_LASER_PIN        PIN_PD2
#else
#define HAL_THERMISTOR_PIN   PIN_PD2
#define HAL_HAS_THERMISTOR
#endif

// ─── Driver init ────────────────────────────────────────────────────────────
// Defined in types/stepper/drivers.cpp.
void drivers_init(void);
