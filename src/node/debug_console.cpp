// debug_console.cpp — bench command console on the board's free USART
// (HAL_DEBUG_SERIAL), built only with -DNODE_DEBUG_CONSOLE (the *_dbg envs).
// A PC talks to the node over plain serial, with no controller and no bus.
//
// Line-based ASCII at 115200 8N1: a command frame without its CRC, as hex
// bytes separated by spaces or commas, [id] [cmd] [len] [payload…]. The console
// adds the CRC, runs it through routeCommand(), and prints "OK <hex…>" (the
// reply without its CRC), "NAK unhandled" or "ERR …". The PC side is
// node_console.py.
#ifdef NODE_DEBUG_CONSOLE
#include <Arduino.h>
#include "board.h"      // HAL_DEBUG_SERIAL
#include "common.h"
#include "protocol.h"

// Shared router defined in dispatch.cpp.
uint8_t routeCommand(const uint8_t* pkt, uint8_t len, uint8_t* reply);

// The serial port wired to the board's USB adapter, never the bus USART.
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

    // Add the CRC, so the handlers see a whole frame and their `len` checks pass.
    buf[n] = crc8(buf, n);
    uint8_t total = n + 1;

    uint8_t reply[MAX_PACKET_LEN];
    uint8_t replyLen = routeCommand(buf, total, reply);
    if (replyLen == 0) { DBG.println(F("NAK unhandled")); return; }

    // The CRC slot is filled only on the bus, so it is left out.
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
