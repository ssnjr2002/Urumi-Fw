// frame.h — RX command framing, written once, inlined into whichever RX ISR is
// compiled (the generic ISR for non-motion types, or the stepper's own ISR).
// Header-inline so there is no cross-TU call cost inside the ISR.
#pragma once
#include <stdint.h>
#include "rs485/rs485.h"         // cmdQueue + framing state + MAX_* (via protocol.h)
                                 // NODE_ID comes from the -DNODE_ID build flag.

// Accumulate one command byte (9th bit already known to be 1) into cmdQueue.
// Every frame is counted to its end, `3 + len + 1` bytes, including one that is
// foreign, too long to store, or arrives with the ring full; those are dropped
// at the end, so the next frame still parses (docs/plans/bus-frame.md).
static inline void frame_command_byte(uint8_t b) {
    if (!inCommand) {
        inCommand  = true;
        rxIdx      = 0;
        discardCmd = false;
    }

    // The head slot is free even with the ring full; it is committed only at
    // the frame end.
    CommandPacket& p = cmdQueue[cmdHead];
    if (rxIdx < MAX_PACKET_LEN) p.data[rxIdx] = b;
    else                        discardCmd = true;
    rxIdx++;

    // First byte is the destination node id.
    if (rxIdx == 1 && b != NODE_ID && b != 0xFF) discardCmd = true;

    // [id][cmd][len][payload…][crc]
    if (rxIdx < 4 || rxIdx != 3u + p.data[2] + 1u) return;
    inCommand = false;
    if (discardCmd) return;

    const uint8_t nextHead = (cmdHead + 1) % MAX_COMMANDS;
    if (nextHead == cmdTail) return;   // ring full — drop
    p.length = (uint8_t)rxIdx;
    cmdHead  = nextHead;
}

// A stream byte (9th bit = 0) resets the command parser: any partially received
// command frame is abandoned so the next command byte starts a fresh packet.
static inline void frame_stream_reset(void) {
    inCommand  = false;
    discardCmd = false;
}

// Count receive errors from RXDATAH and say whether to drop the byte. Called
// first in every RX ISR, after RXDATAL has been read: on a framing error the
// 9th bit is as untrustworthy as the data, so the byte is neither a command
// nor a stream byte. Dropping resets framing; the frame it belonged to then
// fails its CRC.
static inline bool frame_rx_reject(uint8_t status) {
    if (status & USART_BUFOVF_bm) busOvfCount++;
    if (status & USART_FERR_bm) {
        busFerrCount++;
#ifndef NODE_IGNORE_FERR
        frame_stream_reset();
        return true;
#endif
    }
#ifdef NODE_HAS_SILENCE_TIMEOUT
    busHeard = true;
#endif
    return false;
}
