#pragma once
#include <Arduino.h>

// RS485 Pins
#define RS485_TX_PIN  PIN_PF0
#define RS485_RX_PIN  PIN_PF1
#define RS485_EN_PIN  PIN_PF3

// Stepper Pins
#define STEP_PIN PIN_PD4
#define DIR_PIN  PIN_PD5
#define EN_PIN   PIN_PD6

// TMC2660 Settings
#ifdef TMC_2660
#define TMC_CS_PIN PIN_PA7
#define TMC_CURRENT       2000 // mA
#define TMC_MICROSTEPPING 32
#define TMC_R_SENSE       0.1f // Sense resistor value (typical 0.1 ohm)
#endif

// DRV8825 Settings
#ifdef DRV8825
#define DRV_M0_PIN PIN_PA4 // Repurposed MOSI
#define DRV_M1_PIN PIN_PA6 // Repurposed SCK
#define DRV_M2_PIN PIN_PA7 // Repurposed CS
#define DRV_MICROSTEPPING 32
#endif

// RGB LED Pins
#define LED_RED_PIN   PIN_PF2
#define LED_GREEN_PIN PIN_PF4
#define LED_BLUE_PIN  PIN_PF5

// Limit Switch Pin
#define LIMIT_SWITCH_PIN PIN_PD1

// Thermistor Pin
#define THERMISTOR_PIN PIN_PD2

// Serial Setup
#define USB_SERIAL Serial1
#define USB_BAUD   115200

#define RS485_SERIAL Serial2
#define RS485_BAUD   921600
