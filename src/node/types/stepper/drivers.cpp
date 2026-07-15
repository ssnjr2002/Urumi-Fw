// drivers.cpp — stepper driver-chip initialisation (board-agnostic, driver-chip-selected).
//
// Compiled for every stepper build via type_stepper. The active board's
// stepper.h is pulled in by -I; the driver chip is selected by build flags
// (TMC_2660 / DRV8825 / DM542). ATtiny/DM542 builds hit the #else no-op branch
// and never see the SPI/TMCStepper includes.
#include <Arduino.h>
#include "stepper/stepper.h"

#ifdef TMC_2660
#include <SPI.h>
#include <TMCStepper.h>
static TMC2660Stepper tmc(TMC_CS_PIN, TMC_R_SENSE);
#endif

void drivers_init() {
#if defined(TMC_2660)
    SPI.begin();             // REQUIRED — TMC2660 is configured over hardware SPI
    tmc.begin();             // sets toff(8), tbl(1)
    tmc.sdoff(0);            // Use STEP/DIR interface, this is the default behaviour
                             // but explicitly defined anyway
    tmc.rms_current(TMC_CURRENT);
    tmc.microsteps(TMC_MICROSTEPPING);

#elif defined(DRV8825)
    pinMode(DRV_M0_PIN, OUTPUT);
    pinMode(DRV_M1_PIN, OUTPUT);
    pinMode(DRV_M2_PIN, OUTPUT);

    // Map the microstep value (1-32) to its exponent/index (0-5)
    uint8_t step_idx = 0;
    switch (DRV_MICROSTEPPING) {
        case 2:  step_idx = 1; break;
        case 4:  step_idx = 2; break;
        case 8:  step_idx = 3; break;
        case 16: step_idx = 4; break;
        case 32: step_idx = 5; break;
        default: step_idx = 0; break; // Default to full-step if invalid
    }

    // Bit pos 1 goes to M0, Bit pos 2 goes to M1, Bit pos 3 goes to M2.
    digitalWrite(DRV_M0_PIN, (step_idx & 0x01) ? HIGH : LOW);
    digitalWrite(DRV_M1_PIN, (step_idx & 0x02) ? HIGH : LOW);
    digitalWrite(DRV_M2_PIN, (step_idx & 0x04) ? HIGH : LOW);

#elif defined(DM542)
    // No extra init required; EN pin handled by HAL_MOTOR_ENABLE/DISABLE macros
#endif
}

#ifdef TMC_2660
void drivers_enable()  {
    tmc.toff(8);                          // Set toff
    HAL_ENABLE_PORT.OUTCLR = HAL_ENABLE_BM;  // EN pin low
}
void drivers_disable() {
    tmc.toff(0);                          // toff zero
    HAL_ENABLE_PORT.OUTSET = HAL_ENABLE_BM;  // EN pin high
}
#endif
