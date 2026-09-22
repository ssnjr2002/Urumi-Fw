// knife.h — AVR128DB32 knife hardware binding (the avr128db32 × knife cell).
// The pin bindings the portable knife logic (types/knife/) needs. Board-level
// bus/LED config is in ../board.h.
//
// The oscillating drag-knife node drives two outputs:
//   • the knife oscillator (a simple on/off digital enable), and
//   • the chip-clearing blower (a variable-speed fan on a PWM duty 0..100 %).
#pragma once
#include <Arduino.h>

// ─── Knife oscillator (digital on/off) ──────────────────────────────────────
#define HAL_KNIFE_OSC_PIN     PIN_PD1

// ─── Blower (variable-speed fan, PWM) ───────────────────────────────────────
// Driven with analogWrite(); DxCore routes a timer to this pin. PD2 must be a
// PWM-capable output on this core — the type scales a 0..100 % duty to the
// 8-bit analogWrite range.
#define HAL_KNIFE_BLOWER_PIN  PIN_PD2
