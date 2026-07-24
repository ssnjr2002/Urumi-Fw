// debug_console.cpp — bench command console on USART0 (megaTinyCore `Serial`).
//
// OPT-IN, DEBUG ONLY: compiled + wired only when -DNODE_DEBUG_CONSOLE is set
// (see the vac_node5_dbg env in platformio.ini). It lets a PC talk to the node
// DIRECTLY over a plain 8-bit serial link — no Pico, no RS485 bus, no 9-bit
// framing — which is the fastest way to bench-test a node type's own logic.
//
// Wiring: RS485 lives on USART1 (board.h), so USART0 is free. On the 20-pin
// tinyAVR-2 megaTinyCore maps `Serial` to USART0 on PB2 (TX) / PB3 (RX) — both
// unused by the vacuum pinout. Connect a USB-serial adapter there (this is a
// SEPARATE connection from the UPDI programming link).
//
// Protocol (line-based ASCII, 115200 8N1): send the command frame WITHOUT its
// CRC as space/comma-separated hex bytes — [id] [cmd] [len] [payload…] — exactly
// the bytes the Pico's sendPacket() would frame. The console appends the CRC and
// pushes it through routeCommand() (the same handler the RS485 path uses), then
// prints the reply as "OK <hex…>" (payload bytes, CRC omitted) or "NAK" /
// "ERR …". The host side is host/node_console.py.
#ifdef NODE_DEBUG_CONSOLE
#include <Arduino.h>
#include "board.h"      // provides HAL_DEBUG_SERIAL — WITHOUT this the fallback
                        // below silently picks Serial (USART0), the wrong USART.
#include "common.h"
#include "protocol.h"

// Shared router defined in dispatch.cpp.
uint8_t routeCommand(const uint8_t* pkt, uint8_t len, uint8_t* reply);

// Which Arduino serial the board's USB-serial adapter is wired to. It MUST NOT
// be the USART carrying RS485. Each board.h picks the free one:
//   AVR128DB32: RS485 = USART2 → console on Serial1 (USART1, PC0/PC1)
//   ATtiny3226: RS485 = USART1 → console on Serial  (USART0, PB2/PB3)
#ifndef HAL_DEBUG_SERIAL
#error "board.h must define HAL_DEBUG_SERIAL (the free USART wired to the USB-serial adapter) to use the debug console"
#endif

#define CONSOLE_BAUD 115200
#define DBG HAL_DEBUG_SERIAL

static char    line[64];
static uint8_t lineLen = 0;

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;                                  // to lower
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void printHexByte(uint8_t b) {
    if (b < 0x10) DBG.print('0');
    DBG.print(b, HEX);
}

static void processLine() {
    uint8_t buf[MAX_PACKET_LEN];
    uint8_t n = 0;
    const char* p = line;

    // Parse space/comma-separated hex tokens (1- or 2-digit) into buf[].
    while (*p && n < MAX_PACKET_LEN - 1) {          // leave room for the CRC byte
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        int hi = hexNibble(*p++);
        if (hi < 0) { DBG.println(F("ERR hex")); return; }
        int lo = hexNibble(*p);
        uint8_t val;
        if (lo >= 0) { val = (uint8_t)((hi << 4) | lo); p++; }
        else         { val = (uint8_t)hi; }         // single-nibble token
        buf[n++] = val;
    }

    if (n < 2) { DBG.println(F("ERR need >= [id][cmd]")); return; }

    // Append CRC over the frame — identical to the Pico's sendPacket(), so the
    // handlers see a byte-for-byte real command frame (their `len` checks pass).
    buf[n] = crc8(buf, n);
    uint8_t total = n + 1;

    uint8_t reply[MAX_PACKET_LEN];
    uint8_t replyLen = routeCommand(buf, total, reply);
    if (replyLen == 0) { DBG.println(F("NAK unhandled")); return; }

    // reply[] = [id][cmd][payloadLen][payload…][crc-slot]; the CRC slot is only
    // filled by sendCommandPacket on the wire, so drop it here.
    DBG.print(F("OK"));
    for (uint8_t i = 0; i + 1 < replyLen; i++) { DBG.print(' '); printHexByte(reply[i]); }
    DBG.println();
}

void debugConsoleBegin() {
    DBG.begin(CONSOLE_BAUD);
    DBG.println(F("node console ready"));
}

void debugConsolePoll() {
    while (DBG.available()) {
        char c = (char)DBG.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[lineLen] = '\0';
            if (lineLen) processLine();
            lineLen = 0;
        } else if (lineLen < sizeof(line) - 1) {
            line[lineLen++] = c;
        }
        // Overlong lines silently truncate; the CRC/handler will reject garbage.
    }
}
#endif // NODE_DEBUG_CONSOLE
