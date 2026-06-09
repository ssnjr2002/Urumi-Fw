// AVR128DB32-only stepper driver initialisation.
// Compiled only for avr128db32 envs via build_src_filter.
#include <Arduino.h>
#include "config.h"

#ifdef TMC_2660
#include <SPI.h>
#include <TMCStepper.h>
static TMC2660Stepper tmc(TMC_CS_PIN, TMC_R_SENSE);
#endif

void drivers_init() {
#if defined(TMC_2660)
    // Configure everything once here while the bus is quiet, leaving toff NONZERO
    // (software-enabled). Runtime on/off is the hardware EN pin only — toggling
    // toff over SPI from loop() (with the RS485 RX ISR active) was tested and does
    // NOT reliably energize the driver.
    // TODO: Figure this out properly later
    SPI.begin();             // REQUIRED — TMC2660 is configured over hardware SPI
    tmc.begin();             // sets toff(8), tbl(1)
    tmc.toff(4);             // keep software-enabled for the driver's lifetime
    tmc.blank_time(24);
    tmc.rms_current(TMC_CURRENT);
    tmc.microsteps(TMC_MICROSTEPPING);

#elif defined(DRV8825)
    pinMode(DRV_M0_PIN, OUTPUT);
    pinMode(DRV_M1_PIN, OUTPUT);
    pinMode(DRV_M2_PIN, OUTPUT);
    // 1/32 microstepping: M2=1 M1=0 M0=1
    digitalWrite(DRV_M0_PIN, HIGH);
    digitalWrite(DRV_M1_PIN, LOW);
    digitalWrite(DRV_M2_PIN, HIGH);

#elif defined(DM542)
    // No extra init required; EN pin handled by MOTOR_ENABLE/DISABLE macros
#endif
}

#ifdef TMC_2660
// Runtime on/off via the hardware EN pin (ENN active-low); toff stays nonzero.
// Runtime toff-over-SPI was tested and does not energize the driver reliably.
void drivers_enable()  { digitalWrite(EN_PIN, LOW);  }
void drivers_disable() { digitalWrite(EN_PIN, HIGH); }
#endif
