#pragma once
#include <stdint.h>

// cjog.h — the continuous jog (docs/wire_protocol.md, CJOG): a held direction
// is one line to the end of travel, stopped when the packets stop. The data
// plane receives the packets and writes the replies these return.

// A reply: none, an ACK, or else an MSEG_NACK_* reason.
#define CJOG_NO_REPLY 0x00
#define CJOG_ACK      0xFF

// A CRC-checked packet: a direction per slot X, Y, Z, A (−1, 0, +1) and the
// speed (1/64 of jogFeed). `now` is millis().
uint8_t cjogPacket(const int8_t d[4], uint8_t speed, uint32_t now);

// Every Core 0 loop pass: the deadman, the end of a jog, and a start held for
// rest. Its reply is to that held start.
uint8_t cjogTick(uint32_t now);

// The stop byte: brake the jog and forget a held start.
void cjogStop(void);
