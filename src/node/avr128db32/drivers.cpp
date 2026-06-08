// AVR128DB32-only stepper driver initialisation.
// Compiled only for avr128db32 envs via build_src_filter.
#include <Arduino.h>
#include "config.h"

#ifdef TMC_2660
#include <TMC2660Stepper.h>
static TMC2660Stepper tmc(TMC_CS_PIN, TMC_R_SENSE);
#endif

void drivers_init() {
#if defined(TMC_2660)
    tmc.begin();
    tmc.microsteps(TMC_MICROSTEPPING);
    tmc.rms_current(TMC_CURRENT);
    tmc.toff(0); // start disabled

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
void drivers_enable()  { tmc.toff(4); }
void drivers_disable() { tmc.toff(0); }
#endif
