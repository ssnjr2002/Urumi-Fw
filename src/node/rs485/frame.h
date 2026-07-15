// frame.h — RX command framing, written once, inlined into whichever RX ISR is
// compiled (the generic ISR for non-motion types, or the stepper's own ISR).
// Header-inline so there is no cross-TU call cost inside the ISR.
#pragma once
#include <stdint.h>
#include "rs485/rs485.h"         // cmdQueue + framing state + MAX_* (via protocol.h)
                                 // NODE_ID comes from the -DNODE_ID build flag.

// Accumulate one command byte (9th bit already known to be 1) into cmdQueue.
// Mirrors the original isr.cpp command path: foreign-id filter, length-driven
// packet completion, ring-full guard.
static inline void frame_command_byte(uint8_t b) {
    if (!inCommand) {
        inCommand  = true;
        rxIdx      = 0;
        discardCmd = false;
    }

    // Drop a packet already determined not to be addressed to us.
    if (discardCmd) return;

    uint8_t nextHead = (cmdHead + 1) % MAX_COMMANDS;
    if (nextHead == cmdTail) return;   // ring full — drop

    if (rxIdx < MAX_PACKET_LEN) {
        cmdQueue[cmdHead].data[rxIdx++] = b;
    }

    // First byte is the destination node id — drop foreign traffic early.
    if (rxIdx == 1) {
        uint8_t dest = cmdQueue[cmdHead].data[0];
        if (dest != NODE_ID && dest != 0xFF) {
            discardCmd = true;
            return;
        }
    }

    // [id][cmd][len][payload…][crc] — complete when len bytes + crc are in.
    if (rxIdx >= 4) {
        uint8_t expectedLen = cmdQueue[cmdHead].data[2];
        if (rxIdx == 3 + expectedLen + 1) {
            cmdQueue[cmdHead].length = rxIdx;
            cmdHead = nextHead;
            inCommand = false;
        }
    }
}

// A stream byte (9th bit = 0) resets the command parser: any partially received
// command frame is abandoned so the next command byte starts a fresh packet.
static inline void frame_stream_reset(void) {
    inCommand  = false;
    discardCmd = false;
}
