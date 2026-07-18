// Core 0 data plane: binary ingest for the USB CDC stream.
//
// One byte-dispatcher (dataPlaneConsume) serves three receivers, selected by the
// leading magic byte and tracked in rxKind (docs/wire_protocol.md):
//   RX_FIXED26 — MSEG (0xAB) / jog (0xAE), a fixed 26-byte packet
//   RX_CFG     — config write (0xB0), a variable-length transfer
//   (CFG_GET 0xB1 is answered inline — no receive state)
//
// Fixed-26 packet layout (MSEG_PACKET_SIZE = 26 bytes):
//   [0]      magic  0xAB (MSEG) / 0xAE (JOG)
//   [1..24]  MicroSegment (24 bytes, little-endian; byte [22] = rolling seq)
//   [25]     CRC8 over bytes [0..24]
//
// CFG_SET transfer layout (after the 0xB0 magic):
//   [0..3]   length  uint32 LE  (1..CFG_MAX_BYTES)
//   [4..7]   crc32   uint32 LE  (host CRC32 of the payload)
//   [8..]    payload  <length> bytes  → staged, CRC folded incrementally

#include <Arduino.h>
#include <string.h>
#include "../shared.h"
#include "hardware/sync.h"
#include "data_plane.h"
#include "../config/config_store.h"

// ─── Receiver dispatch ────────────────────────────────────────────────────────

enum RxKind : uint8_t { RX_NONE, RX_FIXED26, RX_CFG };
static RxKind rxKind = RX_NONE;

// ─── Fixed-26 (MSEG / jog) state ──────────────────────────────────────────────

static uint8_t  pktBuf[MSEG_PACKET_SIZE];
static uint8_t  pktIdx      = 0;
static uint8_t  expectedSeq = 0;   // next wire seq (pktBuf[22]) we will execute;
                                   // also the cumulative ACK value (see sendAck)

// ─── CFG_SET receive state ────────────────────────────────────────────────────

static uint8_t  cfgHdr[8];         // length(4) + crc32(4)
static uint8_t  cfgHdrIdx  = 0;
static uint32_t cfgLen     = 0;    // expected payload length
static uint32_t cfgCrc     = 0;    // host-declared CRC32
static uint32_t cfgRxCnt   = 0;    // payload bytes received so far
static uint32_t cfgRunCrc  = 0;    // incremental CRC32 accumulator (pre-final-XOR)
static uint32_t cfgLastMs  = 0;    // millis() of last CFG byte — inter-byte timeout

static inline uint32_t crc32Byte(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (uint8_t k = 0; k < 8; k++)
        crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    return crc;
}

// ─── ACK / NACK ───────────────────────────────────────────────────────────────

static void sendAck() {                         // cumulative stream ACK
    // Byte 1 is expectedSeq — the next wire seq we want, i.e. "I have accepted
    // every packet with a lower seq" (TCP-style cumulative ACK). On an accepted
    // packet the caller bumps expectedSeq first, so this advances; on a stale or
    // gap seq (skipped, expectedSeq unchanged) this repeats the last value as a
    // duplicate ACK. The host advances its window to this point, so a lost ACK
    // self-heals via the next one. Byte 2 is reserved (0).
    Serial.write(MSEG_ACK);
    Serial.write(expectedSeq);
    Serial.write((uint8_t)0x00);
}

static void sendNack(uint8_t reason) {          // shared 3-byte NACK frame
    Serial.write(MSEG_NACK);
    Serial.write(reason);
    Serial.write((uint8_t)0x00);
}

static void sendCfgRdy()           { Serial.write(CFG_RDY); }   // header ok — send payload
static void sendCfgAck()           { Serial.write(CFG_ACK); }   // committed
static void sendCfgNack(uint8_t r) { Serial.write(CFG_NACK); Serial.write(r); }

// ─── Fixed-26 packet state machine ────────────────────────────────────────────
// Receives bytes [1..25]; byte [0] (magic) was stored by the dispatcher.

static void feedFixed26(uint8_t b) {
    pktBuf[pktIdx++] = b;

    if (pktIdx < MSEG_PACKET_SIZE) return;      // still accumulating

    rxKind = RX_NONE;                           // packet complete (any outcome)
    pktIdx = 0;

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
    //                     itself a jog (packet 2+ of the same multi-packet burst
    //                     arrives after Core 1 has already flipped the state to
    //                     RUNNING to execute packet 1 — rejecting those left every
    //                     jog after the first NACK_BAD_STATE'd forever).
    uint8_t st = machineState;
    if (pktBuf[0] == MSEG_MAGIC) {
        if (st == STATE_PAUSED)                      { sendNack(MSEG_NACK_PAUSED);    return; }
        if (st != STATE_IDLE && st != STATE_RUNNING) { sendNack(MSEG_NACK_BAD_STATE); return; }
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

// ─── CFG_SET receiver (two-phase) ─────────────────────────────────────────────
// Phase 1: 8-byte header (length + crc32). Validate size and machine state, then
//          reply CFG_RDY (or CFG_NACK). A well-behaved host waits for CFG_RDY
//          before sending payload, so an early NACK cannot desync the stream.
// Phase 2: `length` payload bytes staged into configStageBuf(), CRC32 folded as
//          they land (one pass — the transfer-integrity gate), then commit.
// A stalled transfer is aborted by dataPlaneTick() via the inter-byte timeout.

static void feedCfg(uint8_t b) {
    cfgLastMs = millis();

    if (cfgHdrIdx < sizeof(cfgHdr)) {           // ── phase 1: header ──
        cfgHdr[cfgHdrIdx++] = b;
        if (cfgHdrIdx < sizeof(cfgHdr)) return;
        memcpy(&cfgLen, &cfgHdr[0], 4);
        memcpy(&cfgCrc, &cfgHdr[4], 4);
        if (cfgLen == 0 || cfgLen > CFG_MAX_BYTES) {
            sendCfgNack(CFG_NACK_TOO_BIG); rxKind = RX_NONE; return;
        }
        if (machineState != STATE_IDLE && machineState != STATE_ALARM) {
            sendCfgNack(CFG_NACK_BAD_STATE); rxKind = RX_NONE; return;
        }
        cfgRxCnt  = 0;
        cfgRunCrc = 0xFFFFFFFFu;
        sendCfgRdy();                           // go — host may now stream payload
        return;
    }

    configStageBuf()[cfgRxCnt] = b;             // ── phase 2: payload ──
    cfgRunCrc = crc32Byte(cfgRunCrc, b);
    if (++cfgRxCnt < cfgLen) return;

    rxKind = RX_NONE;                           // transfer complete
    if ((~cfgRunCrc) != cfgCrc) {               // final XOR, compare to host CRC
        sendCfgNack(CFG_NACK_CRC);
        return;
    }

    uint8_t nack;
    if (configStoreCommit(cfgLen, cfgCrc, &nack)) sendCfgAck();
    else                                          sendCfgNack(nack);
}

// ─── CFG_GET responder ────────────────────────────────────────────────────────
// [CFG_DATA][length u32 LE][crc32 u32 LE][payload]. length 0 = no config stored.
// Payload streams straight from XIP; crc is recomputed (GET is a one-shot fetch).

static void handleCfgGet() {
    uint32_t len = g_cfg.length;
    uint32_t crc = (len && g_cfg.addr) ? crc32(g_cfg.addr, len) : 0u;
    Serial.write(CFG_DATA);
    Serial.write((const uint8_t*)&len, 4);
    Serial.write((const uint8_t*)&crc, 4);
    if (len && g_cfg.addr) Serial.write(g_cfg.addr, len);
}

// ─── Public Interface ─────────────────────────────────────────────────────────

bool dataPlaneConsume(uint8_t b) {
    switch (rxKind) {
        case RX_FIXED26: feedFixed26(b); return true;
        case RX_CFG:     feedCfg(b);     return true;
        case RX_NONE:    break;
    }

    // Idle — dispatch on the leading magic byte.
    if (b == MSEG_MAGIC || b == JOG_MAGIC) {
        pktBuf[0] = b;                          // remember stream type for the state gate
        pktIdx    = 1;
        rxKind    = RX_FIXED26;
        return true;
    }
    if (b == CFG_SET_MAGIC) {
        cfgHdrIdx = 0;                          // payload counters init after header (phase 1)
        cfgLastMs = millis();
        rxKind    = RX_CFG;
        return true;
    }
    if (b == CFG_GET_MAGIC) {                    // synchronous — no receive state
        handleCfgGet();
        return true;
    }
    return false;                               // control-plane (text) byte
}

// Abort a stalled CFG_SET transfer. Called every Core 0 loop pass so the timeout
// fires even when no bytes arrive (feedCfg is byte-driven and would otherwise
// wait forever). A stall would else wedge the whole data plane in RX_CFG.
void dataPlaneTick() {
    if (rxKind == RX_CFG && (millis() - cfgLastMs) > CFG_RX_TIMEOUT_MS) {
        rxKind = RX_NONE;
        sendCfgNack(CFG_NACK_TIMEOUT);
    }
}

void dataPlaneReset() {
    rxKind      = RX_NONE;
    pktIdx      = 0;
    expectedSeq = 0;
    cfgHdrIdx   = 0;
    cfgRxCnt    = 0;
}

void dataPlaneResetSeq() {
    expectedSeq = 0;
}
