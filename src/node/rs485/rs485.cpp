// rs485.cpp — 9-bit UART transport (type-agnostic, always compiled).
// Owns the command receive ring / framing state and the packet TX path.
#include <Arduino.h>
#include "board.h"
#include "common.h"
#include "rs485/rs485.h"

// ─── Command receive ring + framing-parser state ────────────────────────────
CommandPacket    cmdQueue[MAX_COMMANDS];
volatile uint8_t cmdHead    = 0;
volatile uint8_t cmdTail    = 0;
volatile bool    inCommand  = false;
volatile uint8_t rxIdx      = 0;
volatile bool    discardCmd = false;

// ─── TX ─────────────────────────────────────────────────────────────────────
void sendCommandPacket(uint8_t* packet, uint8_t len) {
    packet[len - 1] = crc8(packet, len - 1);

    HAL_RS485_TX_BEGIN();

    HAL_USART_INST.STATUS = USART_TXCIF_bm;

    // Sync preamble — stream byte resets the parser on any listening node.
    while (!(HAL_USART_INST.STATUS & USART_DREIF_bm));
    HAL_USART_INST.TXDATAH = 0x00;
    HAL_USART_INST.TXDATAL = 0x00;

    for (int i = 0; i < len; i++) {
        while (!(HAL_USART_INST.STATUS & USART_DREIF_bm));
        HAL_USART_INST.TXDATAH = 0x01;      // 9th bit = 1 → command frame
        HAL_USART_INST.TXDATAL = packet[i];
    }

    while (!(HAL_USART_INST.STATUS & USART_TXCIF_bm));
    HAL_USART_INST.STATUS = USART_TXCIF_bm;

    HAL_RS485_TX_END();
}
