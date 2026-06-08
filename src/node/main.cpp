// Shared node firmware — ATtiny3224 and AVR128DB32
// MCU-specific pin/peripheral definitions come from config.h via -I build flag.
#include <Arduino.h>
#include "config.h"
#include "../include/common.h"
#include "protocol.h"

CommandPacket        cmdQueue[MAX_COMMANDS];
volatile uint8_t     cmdHead     = 0;
volatile uint8_t     cmdTail     = 0;
volatile bool        inCommand   = false;
volatile uint8_t     rxIdx       = 0;
volatile bool        streamEnabled = false;
volatile int32_t     absolutePosition = 0;

// Defined in isr.cpp
extern void isr_init();

// Defined in avr128db32/drivers.cpp (DB32 builds only, pulled in via build_src_filter)
extern void drivers_init() __attribute__((weak));

// ─── RS485 TX ─────────────────────────────────────────────────────────────────

void sendCommandPacket(uint8_t* packet, uint8_t len) {
    packet[len - 1] = crc8(packet, len - 1);

    digitalWrite(RS485_DE_PIN, HIGH);
    delayMicroseconds(10);

    NODE_USART.STATUS = USART_TXCIF_bm;

    // Sync preamble — stream byte resets parser on any listening node
    while (!(NODE_USART.STATUS & USART_DREIF_bm));
    NODE_USART.TXDATAH = 0x00;
    NODE_USART.TXDATAL = 0x00;

    for (int i = 0; i < len; i++) {
        while (!(NODE_USART.STATUS & USART_DREIF_bm));
        NODE_USART.TXDATAH = 0x01;
        NODE_USART.TXDATAL = packet[i];
    }

    while (!(NODE_USART.STATUS & USART_TXCIF_bm));
    NODE_USART.STATUS = USART_TXCIF_bm;

    delayMicroseconds(10);
    digitalWrite(RS485_DE_PIN, LOW);
}

// ─── Position ─────────────────────────────────────────────────────────────────

int32_t readPositionAtomic() {
    cli();
    int32_t pos = absolutePosition;
    sei();
    return pos;
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {
    pinMode(RS485_DE_PIN, OUTPUT); digitalWrite(RS485_DE_PIN, LOW);
    pinMode(STEP_PIN,     OUTPUT); digitalWrite(STEP_PIN,     LOW);
    pinMode(DIR_PIN,      OUTPUT); digitalWrite(DIR_PIN,      LOW);
    pinMode(EN_PIN,       OUTPUT); MOTOR_DISABLE();

    if (drivers_init) drivers_init();
    USART_TX_IDLE_INIT();
    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);

    USART_INIT();

    TCB0.CTRLB  = TCB_CNTMODE_SINGLE_gc;
    TCB0.INTCTRL = TCB_CAPT_bm;

    isr_init();
    sei();

    // Blink NODE_ID times — visual address confirmation
    uint8_t blinks = (NODE_ID < 6) ? (uint8_t)NODE_ID : 5u;
    for (uint8_t i = 0; i < blinks; i++) {
        digitalWrite(LED_PIN, HIGH); delay(150);
        digitalWrite(LED_PIN, LOW);  delay(150);
    }
}

// ─── Loop ─────────────────────────────────────────────────────────────────────

void loop() {
    if (cmdHead == cmdTail) return;

    CommandPacket* pkt = &cmdQueue[cmdTail];
    uint8_t len = pkt->length;

    bool validNode = (pkt->data[0] == NODE_ID || pkt->data[0] == 0xFF);
    bool validCrc  = (pkt->data[len - 1] == crc8(pkt->data, len - 1));

    if (validNode && validCrc) {
        uint8_t cmdId = pkt->data[1];

        switch (cmdId) {
            case CMD_PING: {
                uint8_t reply[4] = {NODE_ID, CMD_PONG, 0, 0};
                sendCommandPacket(reply, 4);
                break;
            }
            case CMD_GET_POS: {
                int32_t pos = readPositionAtomic();
                uint8_t reply[8] = {NODE_ID, CMD_GET_POS, 4, 0, 0, 0, 0, 0};
                reply[3] = (pos >> 24) & 0xFF;
                reply[4] = (pos >> 16) & 0xFF;
                reply[5] = (pos >> 8)  & 0xFF;
                reply[6] =  pos        & 0xFF;
                sendCommandPacket(reply, 8);
                break;
            }
            case CMD_ENABLE: {
                streamEnabled = true;
                MOTOR_ENABLE();
                uint8_t reply[4] = {NODE_ID, CMD_ENABLE, 0, 0};
                sendCommandPacket(reply, 4);
                break;
            }
            case CMD_DISABLE: {
                streamEnabled = false;
                MOTOR_DISABLE();
                uint8_t reply[4] = {NODE_ID, CMD_DISABLE, 0, 0};
                sendCommandPacket(reply, 4);
                break;
            }
        }
    }

    cmdTail = (cmdTail + 1) % MAX_COMMANDS;
}
