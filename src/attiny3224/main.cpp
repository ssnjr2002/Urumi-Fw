// ATtiny3224 + MAX485E + DM542
// 1-Byte Protocol Node

#include <Arduino.h>
#include "common.h"

// ─── Board config ──────────────────────────────────────────────────────────────
#define RS485_DE_PIN  PIN_PA3
#define LED_PIN       PIN_PA5
#define ENABLE_PIN    PIN_PA4
#define STEP_PIN      PIN_PB0
#define DIR_PIN       PIN_PB1

#define LED_PORT      PORTA
#define LED_BM        PIN5_bm

#define DIR_PORT      PORTB
#define DIR_BM        PIN1_bm

#define STEP_PORT     PORTB
#define STEP_BM       PIN0_bm

#define ENABLE_PORT   PORTA
#define ENABLE_BM     PIN4_bm

static uint8_t stepBitMask;
static uint8_t dirBitMask;
static bool currentDir = false;

// 0 = Idle, 1 = Waiting for 5us Dir setup, 2 = 5us Step Pulse HIGH
volatile uint8_t timerState = 0; 

// Macros for exact cycle counts at 20MHz
#define CYCLES_5US  100

void setup() {
    pinMode(RS485_DE_PIN, OUTPUT); digitalWrite(RS485_DE_PIN, LOW); // RX Mode
    pinMode(LED_PIN,      OUTPUT); digitalWrite(LED_PIN,      LOW);
    pinMode(STEP_PIN,     OUTPUT); digitalWrite(STEP_PIN,     LOW);
    pinMode(DIR_PIN,      OUTPUT); digitalWrite(DIR_PIN,      LOW);
    pinMode(ENABLE_PIN,   OUTPUT); digitalWrite(ENABLE_PIN,   LOW); // Driver enabled

    // Node ID Bit Masks
    stepBitMask = 1 << ((NODE_ID - 1) * 2);
    dirBitMask  = 1 << (((NODE_ID - 1) * 2) + 1);

    // Hardware USART Initialization
    USART1.BAUD = (uint16_t)( (F_CPU * 64.0) / (16.0 * RS485_BAUD) + 0.5 );
    USART1.CTRLA = USART_RXCIE_bm; // Enable RX Complete Interrupt
    USART1.CTRLB = USART_RXEN_bm;  // Enable Receiver
    
    // TCB0 Hardware Timer Initialization (Single Shot Mode)
    TCB0.CTRLB = TCB_CNTMODE_SINGLE_gc;
    TCB0.INTCTRL = TCB_CAPT_bm; // Enable TCB interrupt
    // We will enable the timer and set the clock (DIV1) dynamically in the ISR

    // Enable Global Interrupts
    sei();

    // Blink NODE_ID times → visual address confirmation
    uint8_t blinks = (NODE_ID < 6) ? (uint8_t)NODE_ID : 5u;
    for (uint8_t i = 0; i < blinks; i++) {
        digitalWrite(LED_PIN, HIGH); delay(150);
        digitalWrite(LED_PIN, LOW);  delay(150);
    }
}

void loop() {
    // Handled in USART1_RXC_vect ISR
}

ISR(USART1_RXC_vect) {
    uint8_t b = USART1.RXDATAL; // Reading RXDATAL clears the interrupt flag
    
    bool stepReq = (b & stepBitMask) != 0;
    bool newDir = (b & dirBitMask) != 0;
    
    // If a pulse is actively executing, drop this incoming step command.
    // At physical speeds, this overlap should only happen if SPS > 40,000.
    if (timerState != 0) {
        return; 
    }

    if (newDir != currentDir) {
        if (newDir) DIR_PORT.OUTSET = DIR_BM;
        else DIR_PORT.OUTCLR = DIR_BM;
        currentDir = newDir;
        
        if (stepReq) {
            timerState = 1; // Waiting for Dir setup
            TCB0.CCMP = CYCLES_5US;
            TCB0.CNT = 0;
            TCB0.CTRLA = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm; 
        }
    } else {
        if (stepReq) {
            STEP_PORT.OUTSET = STEP_BM;
            timerState = 2; // Step Pulse HIGH
            TCB0.CCMP = CYCLES_5US;
            TCB0.CNT = 0;
            TCB0.CTRLA = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;
        }
    }
}

ISR(TCB0_INT_vect) {
    TCB0.INTFLAGS = TCB_CAPT_bm; // Clear the interrupt flag
    
    if (timerState == 1) {
        // The 5us Dir setup is complete. Start the step pulse.
        STEP_PORT.OUTSET = STEP_BM;
        timerState = 2;
        TCB0.CCMP = CYCLES_5US;
        TCB0.CNT = 0;
        // Timer is still enabled, it will instantly restart for the next 5us
    } else if (timerState == 2) {
        // The 5us Step pulse is complete. Stop.
        STEP_PORT.OUTCLR = STEP_BM;
        timerState = 0;
        TCB0.CTRLA &= ~TCB_ENABLE_bm; // Disable timer
    }
}