// ISR bodies shared across node MCUs.
// config.h (resolved per MCU via -I build flag) provides:
//   NODE_USART, USART_RXC_vect_, STEP_PORT/BM, DIR_PORT/BM, STEP_PULSE_CCMP
#include <Arduino.h>
#include "config.h"
#include "../include/common.h"
#include "protocol.h"

extern volatile bool        streamEnabled;
extern volatile int32_t     absolutePosition;
extern volatile bool        inCommand;
extern volatile uint8_t     rxIdx;
extern volatile uint8_t     cmdHead;
extern volatile uint8_t     cmdTail;
extern CommandPacket        cmdQueue[];

static bool    currentDir  = false;
static uint8_t stepBitMask = 0;
static uint8_t dirBitMask  = 0;
static bool    discardCmd  = false;  // dropping a packet not addressed to us

void isr_init() {
    stepBitMask = 1 << ((NODE_ID - 1) * 2);
    dirBitMask  = 1 << (((NODE_ID - 1) * 2) + 1);
}

ISR(USART_RXC_vect_) {
    uint8_t status = NODE_USART.RXDATAH;
    uint8_t b      = NODE_USART.RXDATAL;

    bool isCommand = (status & 0x01);

    if (isCommand) {
        if (!inCommand) {
            inCommand   = true;
            rxIdx       = 0;
            discardCmd  = false;
        }

        // Foreign-traffic filter: the first byte is the destination node ID.
        // Every node on the bus hears every packet (including other nodes'
        // PONG replies). Without this, those packets pile into cmdQueue and
        // evict our own commands once the ring fills. Drop anything not
        // addressed to us (or broadcast) before it ever touches the queue.
        if (discardCmd) return;

        uint8_t nextHead = (cmdHead + 1) % MAX_COMMANDS;
        if (nextHead == cmdTail) return;

        if (rxIdx < MAX_PACKET_LEN) {
            cmdQueue[cmdHead].data[rxIdx++] = b;
        }

        if (rxIdx == 1) {
            uint8_t dest = cmdQueue[cmdHead].data[0];
            if (dest != NODE_ID && dest != 0xFF) {
                discardCmd = true;
                return;
            }
        }

        if (rxIdx >= 4) {
            uint8_t expectedLen = cmdQueue[cmdHead].data[2];
            if (rxIdx == 3 + expectedLen + 1) {
                cmdQueue[cmdHead].length = rxIdx;
                cmdHead = nextHead;
                inCommand = false;
            }
        }
        return;
    }

    // Stream byte (9th bit = 0) — reset command parser
    inCommand  = false;
    discardCmd = false;
    if (!streamEnabled) return;

    bool stepReq = (b & stepBitMask) != 0;
    bool newDir  = (b & dirBitMask)  != 0;

    if (newDir != currentDir) {
        if (newDir) DIR_PORT.OUTSET = DIR_BM;
        else        DIR_PORT.OUTCLR = DIR_BM;
        currentDir = newDir;
        delayMicroseconds(5);
    }

    if (stepReq) {
        STEP_PORT.OUTSET = STEP_BM;
        absolutePosition += (currentDir ? 1 : -1);
        TCB0.CCMP  = STEP_PULSE_CCMP;
        TCB0.CNT   = 0;
        TCB0.CTRLA = TCB_CLKSEL_CLKDIV1_gc | TCB_ENABLE_bm;
    }
}

ISR(TCB0_INT_vect) {
    TCB0.INTFLAGS = TCB_CAPT_bm;
    STEP_PORT.OUTCLR = STEP_BM;
    TCB0.CTRLA &= ~TCB_ENABLE_bm;
}
