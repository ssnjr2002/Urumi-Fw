// Core 0 data plane: binary MicroSegment / jog packet ingest.
//
// Packet layout (MSEG_PACKET_SIZE = 26 bytes):
//   [0]      magic  0xAB (MSEG) / 0xAE (JOG)
//   [1..24]  MicroSegment (24 bytes, little-endian; byte [22] = rolling seq
//            stamped by the host sender, used for the duplicate guard)
//   [25]     CRC8 over bytes [0..24]
//
// On success: push to ring buffer, send ACK (3 bytes).
// On failure: send NACK with reason byte, reset state machine.

#include <Arduino.h>
#include "../shared.h"
#include "hardware/sync.h"
#include "data_plane.h"

// ─── Ingest State ─────────────────────────────────────────────────────────────

static uint8_t  pktBuf[MSEG_PACKET_SIZE];
static uint8_t  pktIdx      = 0;
static bool     inPacket    = false;
static uint16_t pktSeq      = 0;   // rolling counter for ACK echo
static uint8_t  expectedSeq = 0;   // next wire seq (pktBuf[22]) we will execute

// ─── ACK / NACK ───────────────────────────────────────────────────────────────

static void sendAck() {
    Serial.write(MSEG_ACK);
    Serial.write((uint8_t)(pktSeq & 0xFF));
    Serial.write((uint8_t)(pktSeq >> 8));
    pktSeq++;
}

static void sendNack(uint8_t reason) {
    Serial.write(MSEG_NACK);
    Serial.write(reason);
    Serial.write((uint8_t)0x00);
}

// ─── Packet State Machine ─────────────────────────────────────────────────────

static void processBinaryByte(uint8_t b) {
    if (!inPacket) {
        if (b == MSEG_MAGIC || b == JOG_MAGIC) {
            pktBuf[0] = b;            // remember which stream type for the state gate
            pktIdx    = 1;
            inPacket  = true;
        }
        // Any non-magic byte while idle is ignored (text commands handled separately)
        return;
    }

    pktBuf[pktIdx++] = b;

    if (pktIdx < MSEG_PACKET_SIZE) return; // Still accumulating

    // Full packet received — validate CRC
    inPacket = false;
    pktIdx   = 0;

    uint8_t expected = crc8(pktBuf, MSEG_PACKET_SIZE - 1);
    if (pktBuf[MSEG_PACKET_SIZE - 1] != expected) {
        sendNack(MSEG_NACK_CRC);
        return;
    }

    // State gate (wire_protocol.md allowed-state matrix). The magic byte selects
    // the stream type; we record it as the streamIsJog intent so Core 1 sets
    // runningReason together with the RUNNING transition it owns.
    //   MSEG job stream — IDLE/RUNNING; NACK_PAUSED while paused, else bad_state.
    //   JOG burst       — IDLE/PAUSED, or RUNNING if the in-progress burst is
    //                     itself a jog (packet 2+ of the same multi-packet
    //                     burst arrives after Core 1 has already flipped the
    //                     state to RUNNING to execute packet 1 — rejecting
    //                     those left every jog after the first packet
    //                     NACK_BAD_STATE'd forever, scrambling the motion).
    uint8_t st = machineState;
    if (pktBuf[0] == MSEG_MAGIC) {
        if (st == STATE_PAUSED)                          { sendNack(MSEG_NACK_PAUSED);    return; }
        if (st != STATE_IDLE && st != STATE_RUNNING)     { sendNack(MSEG_NACK_BAD_STATE); return; }
        streamIsJog = false;
    } else { // JOG_MAGIC
        bool continuingJog = (st == STATE_RUNNING && runningReason == RUNNING_JOG);
        if (st != STATE_IDLE && st != STATE_PAUSED && !continuingJog) {
            sendNack(MSEG_NACK_BAD_STATE); return;
        }
        streamIsJog = true;
    }

    // Duplicate guard: byte [22] carries the host's rolling 8-bit seq. After a
    // NACK the host rewinds (Go-Back-N) and may resend packets we already
    // accepted; executing them again would duplicate motion — a permanent
    // position offset. A seq we are not expecting is a stale retransmit: ACK it
    // (so the host's window advances) but do not execute. The host resets this
    // counter with the "seqreset" text command before each stream.
    if (pktBuf[22] != expectedSeq) {
        sendAck();
        return;
    }

    // Check buffer space
    uint16_t next = (mBufTail + 1) % MASTER_BUF_SIZE;
    if (next == mBufHead) {
        sendNack(MSEG_NACK_FULL);     // backpressure — windowed sender retries
        return;
    }

    // Deserialise MicroSegment from bytes [1..24] (little-endian)
    MicroSegment ms;
    const uint8_t* p = &pktBuf[1];
    memcpy(&ms.dx,       p,      4); p += 4;
    memcpy(&ms.dy,       p,      4); p += 4;
    memcpy(&ms.dz,       p,      4); p += 4;
    memcpy(&ms.da,       p,      4); p += 4;
    memcpy(&ms.interval, p,      4); p += 4;
    ms.flags  = *p++;
    ms.pad[0] = ms.pad[1] = ms.pad[2] = 0;

    masterBuf[mBufTail] = ms;
    __dmb();
    mBufTail = next;

    expectedSeq++;
    sendAck();
}

// ─── Public Interface ─────────────────────────────────────────────────────────

bool dataPlaneConsume(uint8_t b) {
    if (inPacket) { processBinaryByte(b); return true; }
    if (b == MSEG_MAGIC || b == JOG_MAGIC) { processBinaryByte(b); return true; }
    return false;
}

void dataPlaneReset() {
    inPacket    = false;
    pktIdx      = 0;
    expectedSeq = 0;
    pktSeq      = 0;
}

void dataPlaneResetSeq() {
    expectedSeq = 0;
    pktSeq      = 0;
}
