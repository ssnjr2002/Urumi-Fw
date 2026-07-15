// rs485.h — 9-bit UART transport, type-agnostic.
//
// Owns the command receive queue and its framing-parser state (filled by the
// inline framer in frame.h, drained by loop() in main.cpp) plus the packet TX.
// Definitions live in rs485.cpp; the RX ISR itself is per-type (see frame.h and
// types/stepper/, rs485/isr_generic.cpp).
#pragma once
#include <stdint.h>
#include "protocol.h"

// Command receive ring. Head advanced by the framer (ISR context), tail by loop.
extern CommandPacket    cmdQueue[MAX_COMMANDS];
extern volatile uint8_t cmdHead;
extern volatile uint8_t cmdTail;

// Framing-parser state, shared between the ISR framer (frame.h) and resets.
extern volatile bool    inCommand;
extern volatile uint8_t rxIdx;
extern volatile bool    discardCmd;

// Transmit a command packet: appends CRC8 into packet[len-1], drives DE, and
// sends each byte with the 9th bit = 1 behind a stream-byte sync preamble.
void sendCommandPacket(uint8_t* packet, uint8_t len);
