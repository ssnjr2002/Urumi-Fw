#include <Arduino.h>
#include "RingBuf.hpp"

#define RS485_DIR_PIN   PIN3_bm // PIN_PA3
#define ENABLE_PIN      PIN4_bm // PIN_PA4
#define LED_PIN         PIN5_bm // PIN_PA5
#define STEP_PIN        PIN0_bm // PIN_PB0
#define DIR_PIN         PIN1_bm // PIN_PB1

#define CALC_BAUD_REG_VAL(BAUD_RATE) (((float)F_CPU * 64.0f / (16.0f * (float)BAUD_RATE)) + 0.5f)
#define RS485_BAUD      CALC_BAUD_REG_VAL(230400)

#define MAX_FRAME_SIZE 32
#define FRAME_BUFFER_SIZE 4
#define RS485_IDLE_TIMEOUT_US 500

struct Frame {
    uint8_t data[MAX_FRAME_SIZE];
    uint8_t len;
};

static RingBuffer<Frame, FRAME_BUFFER_SIZE> frameBuf;
volatile uint32_t rxLastMicros = 0; // Store when last rx
volatile uint8_t rxResetFlag = 0; // Triggers a reset, usually used for idle timeouts

// USART1 Receive complete interrupt
ISR(USART1_RXC_vect) {
    // State variables
    static uint8_t rxIndex = 0;
    static bool overflow = false;

    // Imp: read high value (RXDATAH) first, then low value (RXDATAL)
    uint8_t status = USART1.RXDATAH; // Read status register
    uint8_t b = USART1.RXDATAL; // Store received byte
    
    // Restart the Watchdog
    // Writing CNT = 0 and setting ENABLE restarts the single-shot countdown.
    TCB1.CNT = 0;
    TCB1.CTRLA = TCB_CLKSEL_DIV1_gc | TCB_ENABLE_bm;

    if (rxResetFlag) { // Idle timeout reset triggered by main loop
        rxIndex = 0;
        rxResetFlag = 0;
        overflow = false;
    }

    // Skip if status not ok
    if (status & (USART_FERR_bm | USART_PERR_bm | USART_BUFOVF_bm)) {
        rxIndex = 0;
        overflow = false;
        return;
    }

    // COBS Framing
    if (b == 0x00) { // End of frame
        Frame *f = frameBuf.getWritePtr(); // Get frame ptr
        if (f && rxIndex > 0) { // Store frame if we have a frame ptr and bytes
            f->len = rxIndex;
            frameBuf.advanceTail();
            // Notice we dont store the 0x00
        }
        // Reset for next frame
        rxIndex = 0;
        overflow = false;
    } else { // Collect bytes
        if (!overflow) { // If not overflowing
            Frame *f = frameBuf.getWritePtr(); // Get frame ptr
            
            if (f && rxIndex < MAX_FRAME_SIZE) { // If frame ptr and rxIndex not over max size
                f->data[rxIndex++] = b; // store
            } else {
                overflow = true; // else flag overflow
            }
        }
    }
}

ISR(TCB1_INT_vect) {
    TCB1.INTFLAGS = TCB_CAPT_bm;
    rxResetFlag = 1;
}

void setup() {
    // Setup pins to output
    PORTA.DIRSET = (RS485_DIR_PIN | ENABLE_PIN | LED_PIN);
    PORTB.DIRSET = (STEP_PIN | DIR_PIN);

    PORTA.OUTCLR = LED_PIN;

    // Setup Timer
    // TCB0.CTRLB      = TCB_CNTMODE_INT_gc;   // Periodic Interrupt
    // TCB0.INTCTRL    = TCB_CAPT_bm;          // Set Interrupt Control to Capture or Timeout 
    // TCB0.CTRLA      = TCB_CLKSEL_DIV1_gc;   // Set clock source as Peripheral Clock (CLK_PER)
    // CPUINT.LVL1VEC  = TCB0_INT_vect_num;    // Elevate timer interrupt to priority level 1 (higher priority)

    // Watch Dog Timer for Idle
    TCB1.CTRLB = TCB_CNTMODE_SINGLE_gc;  // Single Shot Mode
    TCB1.INTCTRL = TCB_CAPT_bm;          // Set Interrupt Control to Capture or Timeout 
    TCB1.CCMP = (F_CPU / 1000000) * 500; // 500 us timeout

    // Setup USART
    USART1.BAUD  = (uint16_t)RS485_BAUD;            // Set baud rate
    USART1.CTRLA = USART_RXCIE_bm | USART_RS485_bm; // Enable Receive Complete Interrupt and RS485 auto direction
    USART1.CTRLB = USART_RXEN_bm  | USART_TXEN_bm;  // Enable rx and tx

    Serial.begin(115200);

    int count = 100;
    while (count > 0) {
        count--;
        Serial.print('.');
        delay(250);
    }

    sei(); // Enable interrupts
}

void loop() {
    // Process received frames
    // Use peek/advanceHead to avoid large struct copies on the stack
    Frame *f = frameBuf.peek();
    if (f) {
        // Toggle LED to show activity
        PORTA.OUTTGL = LED_PIN;

        // For testing: Relay raw COBS data to USB Serial
        Serial.print("Frame Recv (len ");
        Serial.print(f->len);
        Serial.print("): ");
        for(uint8_t i=0; i < f->len; i++) {
            if(f->data[i] < 0x10) Serial.print('0');
            Serial.print(f->data[i], HEX);
            Serial.print(' ');
        }
        Serial.println();
        
        frameBuf.advanceHead();
    }
}