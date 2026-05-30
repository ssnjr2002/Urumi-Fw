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

#define MAX_COMMANDS 4
#define MAX_PACKET_LEN 32

typedef struct {
    uint8_t data[MAX_PACKET_LEN];
    uint8_t length;
} CommandPacket;

CommandPacket cmdQueue[MAX_COMMANDS];
volatile uint8_t cmdHead = 0; // Where ISR writes
volatile uint8_t cmdTail = 0; // Where loop reads

volatile bool inCommand = false;
volatile uint8_t rxIdx = 0;

void sendPong() {
    // VISUAL DEBUG: Toggle LED when we attempt to send a PONG
    PORTA.OUTTGL = PIN5_bm;

    digitalWrite(RS485_DE_PIN, HIGH);
    delayMicroseconds(1); // Give transceiver time to switch
    
    uint8_t pongPacket[4] = {NODE_ID, CMD_PONG, 0, 0};
    pongPacket[3] = crc8(pongPacket, 3);
    
    USART1.STATUS = USART_TXCIF_bm; // Clear any old TX complete flag!
    
    for (int i = 0; i < 4; i++) {
        while (!(USART1.STATUS & USART_DREIF_bm)); // Wait for Data Register Empty
        USART1.TXDATAH = 0x01; // 9th bit = 1
        USART1.TXDATAL = pongPacket[i];
    }
    
    while (!(USART1.STATUS & USART_TXCIF_bm)); // Wait for physical Shift Register Empty
    USART1.STATUS = USART_TXCIF_bm; // Clear flag
    
    delayMicroseconds(1); // Ensure stop bit fully propagates
    digitalWrite(RS485_DE_PIN, LOW); // Back to RX
}

void setup() {
    pinMode(RS485_DE_PIN, OUTPUT); digitalWrite(RS485_DE_PIN, LOW); // RX Mode
    pinMode(PIN_PA1,      OUTPUT); digitalWrite(PIN_PA1,      HIGH); // USART1 TX explicitly driven HIGH (idle)
    pinMode(LED_PIN,      OUTPUT); digitalWrite(LED_PIN,      LOW);
    pinMode(STEP_PIN,     OUTPUT); digitalWrite(STEP_PIN,     LOW);
    pinMode(DIR_PIN,      OUTPUT); digitalWrite(DIR_PIN,      LOW);
    pinMode(ENABLE_PIN,   OUTPUT); digitalWrite(ENABLE_PIN,   LOW); // Driver enabled

    // Node ID Bit Masks
    stepBitMask = 1 << ((NODE_ID - 1) * 2);
    dirBitMask  = 1 << (((NODE_ID - 1) * 2) + 1);

    // Hardware USART Initialization
    USART1.BAUD = (uint16_t)( (F_CPU * 64.0) / (16.0 * RS485_BAUD) + 0.5 );
    USART1.CTRLC = USART_CHSIZE_9BITH_gc; // Enable 9-bit mode!
    USART1.CTRLA = USART_RXCIE_bm; // Enable RX Complete Interrupt
    USART1.CTRLB = USART_RXEN_bm | USART_TXEN_bm;  // Enable BOTH Receiver and Transmitter
    
    // TCB0 Hardware Timer Initialization (Single Shot Mode)
    TCB0.CTRLB = TCB_CNTMODE_SINGLE_gc;
    TCB0.INTCTRL = TCB_CAPT_bm; // Enable TCB interrupt

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
    if (cmdHead != cmdTail) {
        // Offload processing from ISR
        CommandPacket* pkt = &cmdQueue[cmdTail];
        uint8_t len = pkt->length;
        
        bool validNode = (pkt->data[0] == NODE_ID || pkt->data[0] == 0xFF);
        bool validCrc = (pkt->data[len - 1] == crc8(pkt->data, len - 1));
        
        if (validNode && validCrc) {
            uint8_t cmdId = pkt->data[1];
            
            switch (cmdId) {
                case CMD_PING:
                    sendPong();
                    break;
                // Future commands will go here
            }
        }
        
        cmdTail = (cmdTail + 1) % MAX_COMMANDS; // Move to next command
    }
}

ISR(USART1_RXC_vect) {
    uint8_t status = USART1.RXDATAH; // MUST read high byte first to get 9th bit
    uint8_t b = USART1.RXDATAL;      // Reading low byte clears the interrupt flag
    
    bool isCommand = (status & 0x01); // 9th bit is bit 0 of RXDATAH
    
    if (isCommand) {
        uint8_t nextHead = (cmdHead + 1) % MAX_COMMANDS;
        if (nextHead == cmdTail) return; // Queue full, drop incoming command
        
        if (!inCommand) {
            inCommand = true;
            rxIdx = 0;
        }
        
        if (rxIdx < MAX_PACKET_LEN) {
            cmdQueue[cmdHead].data[rxIdx++] = b;
        }
        
        if (rxIdx >= 4) { // Node, Cmd, Len, CRC (min size)
            uint8_t expectedLen = cmdQueue[cmdHead].data[2];
            if (rxIdx == 3 + expectedLen + 1) { // Node + Cmd + Len + Payload + CRC
                cmdQueue[cmdHead].length = rxIdx;
                cmdHead = nextHead; // Commit command to queue
                inCommand = false;
            }
        }
        return; // Ignore command bytes for spatial processing
    }
    
    // 9th bit = 0. Instantly abort any active command parse to prevent desync!
    inCommand = false;
    
    bool stepReq = (b & stepBitMask) != 0;
    bool newDir = (b & dirBitMask) != 0;
    
    // If a pulse is actively executing, drop this incoming step command.
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