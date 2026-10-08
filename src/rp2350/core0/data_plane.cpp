// Core 0 data plane: binary ingest for the USB CDC stream.
//
// One byte-dispatcher (dataPlaneConsume) serves two receivers, selected by the
// leading magic byte and tracked in rxKind (docs/wire_protocol.md):
//   RX_PACKET  — MSEG (0xAB) / jog (0xAE), 26 bytes; BEZIER (0xAD), 56 bytes;
//                continuous jog (0xAF), 7 bytes
//   RX_CFG     — config write (0xB0), a variable-length transfer
//   (CFG_GET 0xB1 is answered inline — no receive state)
//
// Packet layouts: usb_protocol.h. Every packet is magic first, CRC8 last over
// the rest, with a rolling seq byte at a per-magic offset.
//
// CFG_SET transfer layout (after the 0xB0 magic):
//   [0..3]   length  uint32 LE  (1..CFG_MAX_BYTES)
//   [4..7]   crc32   uint32 LE  (host CRC32 of the payload)
//   [8..]    payload  <length> bytes  → staged, CRC folded incrementally

#include <Arduino.h>
#include <string.h>
#include "../ipc/shared_state.h"
#include "usb_protocol.h"
#include "hardware/sync.h"
#include "data_plane.h"
#include "config/config_store.h"
#include "config/machine_cfg.h"
#include "ops/position.h"          // axisNode
#include "ops/frames.h"
#include "planner/queue.h"
#include <planner/bezier.h>

// ─── Receiver dispatch ────────────────────────────────────────────────────────

enum RxKind : uint8_t { RX_NONE, RX_PACKET, RX_CFG };
static RxKind rxKind = RX_NONE;

// ─── Packet (MSEG / jog / BEZIER) state ───────────────────────────────────────

static uint8_t  pktBuf[BEZIER_PACKET_SIZE];   // the largest packet
static uint8_t  pktIdx      = 0;
static uint8_t  pktSize     = 0;   // set from the magic by the dispatcher
static uint32_t pktLastMs   = 0;   // millis() of last packet byte — inter-byte timeout
static uint8_t  expectedSeq = 0;   // next wire seq we will execute;
                                   // also the cumulative ACK value (see sendAck)
static uint8_t  pendingAcks = 0;   // accepted packets not yet confirmed on the wire

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

static void flushAck() {                        // cumulative stream ACK
    // Byte 1 is expectedSeq — the next wire seq we want, i.e. "I have accepted
    // every packet with a lower seq" (TCP-style cumulative ACK). Because it is
    // cumulative, ONE frame confirms every packet accepted since the last flush;
    // that is what makes coalescing free rather than a tradeoff. The host
    // advances its window to this point, so a lost ACK self-heals via the next
    // one. Byte 2 is reserved (0).
    //
    // One Serial.write of the whole frame, not three: USB CDC costs per
    // transaction, and a partial frame must never be able to interleave.
    pendingAcks = 0;
    const uint8_t frame[3] = { MSEG_ACK, expectedSeq, 0x00 };
    Serial.write(frame, sizeof(frame));
}

// Accept-path ACK. Deferred — flushed when the input drains (dataPlaneTick),
// when ACK_COALESCE_MAX pile up, or immediately by anything that must not be
// reordered behind them.
static inline void markAck() {
    if (++pendingAcks >= ACK_COALESCE_MAX) flushAck();
}

static void sendNack(uint8_t reason) {          // shared 3-byte NACK frame
    // Ordering matters more than latency here: a NACK rewinds the host's window,
    // so any ACK earned before it must land first. Otherwise the host applies a
    // rewind and then an advance past it, and re-skips packets it just resent.
    if (pendingAcks) flushAck();
    const uint8_t frame[3] = { MSEG_NACK, reason, 0x00 };
    Serial.write(frame, sizeof(frame));
}

static void sendCfgRdy()           { Serial.write(CFG_RDY); }   // header ok — send payload
static void sendCfgAck()           { Serial.write(CFG_ACK); }   // committed
static void sendCfgNack(uint8_t r) { Serial.write(CFG_NACK); Serial.write(r); }

static void acceptMseg();
static void acceptBezier();
static void acceptCjog();

// ─── Packet state machine ─────────────────────────────────────────────────────
// Receives bytes [1..pktSize-1]; byte [0] (magic) was stored by the dispatcher.

static void feedPacket(uint8_t b) {
    pktBuf[pktIdx++] = b;
    pktLastMs = millis();

    if (pktIdx < pktSize) return;               // still accumulating

    rxKind = RX_NONE;                           // packet complete (any outcome)
    pktIdx = 0;

    uint8_t expected = crc8(pktBuf, pktSize - 1);
    if (pktBuf[pktSize - 1] != expected) {
        sendNack(MSEG_NACK_CRC);
        return;
    }

    // Before the abort barrier: a direction change brakes with it raised.
    if (pktBuf[0] == CJOG_MAGIC) { acceptCjog(); return; }

    // State gate (wire_protocol.md allowed-state matrix). The magic byte selects
    // the stream type; we record it as the streamIsJog intent so Core 1 sets
    // runningReason together with the RUNNING transition it owns.
    //   MSEG job stream — IDLE/RUNNING; NACK_PAUSED while paused, else bad_state.
    //   BEZIER records  — the same; the planner's admit narrows RUNNING to its own.
    //   JOG burst       — retired: jogs run on the planner (`jog`, CJOG).
    // Abort is a barrier — reject everything until the machine reaches IDLE,
    // with a reason distinct from BAD_STATE so the host waits and reopens rather
    // than surfacing an error. Checked before the per-magic gates because it
    // applies equally to jobs and jogs.
    if (abortRequested || runningReason == RUNNING_ABORT_DECEL) {
        sendNack(MSEG_NACK_ABORTING);
        return;
    }

    uint8_t st = machineState;
    if (pktBuf[0] == MSEG_MAGIC || pktBuf[0] == BEZIER_MAGIC) {
        if (st == STATE_PAUSED)                      { sendNack(MSEG_NACK_PAUSED);    return; }
        if (st != STATE_IDLE && st != STATE_RUNNING) { sendNack(MSEG_NACK_BAD_STATE); return; }
        if (pktBuf[0] == MSEG_MAGIC) streamIsJog = false;
    } else { // JOG_MAGIC
        sendNack(MSEG_NACK_BAD_STATE); return;
    }

    // Duplicate guard: the seq byte carries the host's rolling 8-bit seq. After a
    // NACK the host rewinds (Go-Back-N) and may resend packets we already
    // accepted; executing them again would duplicate motion — a permanent
    // position offset. A seq we are not expecting is a stale retransmit: ACK it
    // (so the host's window advances) but do not execute. The host resets this
    // counter with the "seqreset" text command before each stream.
    const uint8_t seqAt = pktBuf[0] == BEZIER_MAGIC ? BEZIER_SEQ_OFFSET : MSEG_SEQ_OFFSET;
    if (pktBuf[seqAt] != expectedSeq) {
        flushAck();     // immediate: this duplicate ACK is the host's resync signal
        return;
    }

    if (pktBuf[0] == BEZIER_MAGIC) acceptBezier();
    else acceptMseg();
}

static void acceptMseg() {
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

    // Steps for an unbound axis would reach whatever holds its slot (a probe's
    // vacuum reads them as poll requests), or no one.
    const int32_t d[MOTION_SLOTS] = { ms.dx, ms.dy, ms.dz, ms.da };
    for (uint8_t k = 0; k < MOTION_SLOTS; k++)
        if (d[k] != 0 && axisNode(k) == SLOT_NONE) { sendNack(MSEG_NACK_BAD_STATE); return; }

    masterBuf[mBufTail] = ms;
    // Queued-time accounting (§4.6) — must land BEFORE the tail publishes the
    // segment, or a status poll between the two reports a segment that is
    // visible in bufCount but contributes no time.
    queuedUsIn += microSegmentUs(ms.dx, ms.dy, ms.dz, ms.da, ms.interval);
    __dmb();
    mBufTail = next;

    expectedSeq++;
    markAck();
}

static void acceptBezier() {
    if (pauseRequested) { sendNack(MSEG_NACK_PAUSED); return; }

    planner::Bezier bz;
    const uint8_t* p = &pktBuf[3];
    for (int i = 0; i < 4; i++) {
        memcpy(&bz.p[i].x, p, 4); p += 4;
        memcpy(&bz.p[i].y, p, 4); p += 4;
    }
    memcpy(&bz.length,     p, 4); p += 4;
    memcpy(&bz.kappa_max,  p, 4); p += 4;
    memcpy(&bz.dkappa_max, p, 4); p += 4;
    memcpy(&bz.ts[1],      p, 4); p += 4;
    memcpy(&bz.ts[2],      p, 4);

    // Work → machine: a translation, so the analysed fields stand as sent.
    for (int i = 0; i < 4; i++) {
        planner::Vec2& q = bz.p[i];
        if (framesToMachine(q.x, q.y, &q.x, &q.y)) { sendNack(MSEG_NACK_BAD_STATE); return; }
        if (framesCheckXY(q.x, q.y)) { sendNack(MSEG_NACK_SOFT_LIMIT); return; }
    }

    const uint8_t flags = pktBuf[1];
    switch (plannerQueueRecord(bz, flags & BEZIER_FLAG_START, flags & BEZIER_FLAG_END)) {
        case PQ_OK:        break;
        case PQ_FULL:      sendNack(MSEG_NACK_FULL);      return;
        case PQ_BAD_CURVE: sendNack(MSEG_NACK_BAD_CURVE); return;
        case PQ_SOFT_LIMIT: sendNack(MSEG_NACK_SOFT_LIMIT); return;
        default:           sendNack(MSEG_NACK_BAD_STATE); return;   // state, config, limits, feed
    }
    expectedSeq++;
    markAck();
}

// ─── CFG_SET receiver (two-phase) ─────────────────────────────────────────────
// Phase 1: 8-byte header (length + crc32). Validate size and machine state, then
//          reply CFG_RDY (or CFG_NACK). A well-behaved host waits for CFG_RDY
//          before sending payload, so an early NACK cannot desync the stream.
// Phase 2: `length` payload bytes staged into configStageBuf(), CRC32 folded as
//          they land (one pass — the transfer-integrity gate), then decode and
//          validate (CFG_NACK_SCHEMA), commit, and re-commit the defaultHead
//          axis map from the new config before the ACK.
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

    // A blob that does not decode never becomes the active config.
    if (machineCfgStage(configStageBuf(), cfgLen) != CFG_DEC_OK) {
        sendCfgNack(CFG_NACK_SCHEMA);
        return;
    }
    uint8_t nack;
    if (!configStoreCommit(cfgLen, cfgCrc, &nack)) { sendCfgNack(nack); return; }
    machineCfgAdopt();
    sendCfgAck();
    // The boot sequence applies the new config: sweep, default map, `ready`.
    // Every push voids the datums, so no field has to be judged for whether it
    // invalidates one.
    soft_reset_requested = true;
}

// ─── CFG_GET responder ────────────────────────────────────────────────────────
// [CFG_DATA][length u32 LE][crc32 u32 LE][payload]. length 0 = no config stored.
// crc is the one verified at boot/commit; the payload streams from the file.
// A short read mid-stream cannot be signalled after the header, so it pads with
// zeros to keep the frame length and the host's CRC check rejects it.

static void handleCfgGet() {
    uint32_t len = g_cfg.valid ? g_cfg.length : 0u;
    uint32_t crc = g_cfg.valid ? g_cfg.crc32  : 0u;
    Serial.write(CFG_DATA);
    Serial.write((const uint8_t*)&len, 4);
    Serial.write((const uint8_t*)&crc, 4);

    uint8_t chunk[512];
    for (uint32_t off = 0; off < len; ) {
        uint32_t want = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        uint32_t got  = configStoreRead(off, chunk, want);
        if (got < want) memset(chunk + got, 0, want - got);
        Serial.write(chunk, want);
        off += want;
    }
}

// ─── Continuous jog ───────────────────────────────────────────────────────────
// A held jog is one line to the end of travel, stopped when the packets stop.
// It moves one axis set: XY, Z or A. cjogHeld: packets for cjogDir keep
// arriving (until the stop byte, the deadman or another direction). cjogOn:
// its line may still run. cjogWant: a direction to start once the machine is
// at rest, after a change stopped the last jog.

// How far a held A jog runs: A has no soft range, and a cabled head winds.
#define CJOG_A_RUN 360.0f

static int8_t   cjogDir[4]  = {0, 0, 0, 0};
static int8_t   cjogWant[4] = {0, 0, 0, 0};
static uint8_t  cjogSpeed   = 0;
static bool     cjogHeld    = false;
static bool     cjogOn      = false;
static uint32_t cjogLastMs  = 0;

static void sendAck() {
    const uint8_t f[3] = { MSEG_ACK, expectedSeq, 0 };
    Serial.write(f, 3);
}

static bool cjogWanted() {
    return cjogWant[0] != 0 || cjogWant[1] != 0 || cjogWant[2] != 0 || cjogWant[3] != 0;
}

static void cjogStop() {
    if (cjogOn) plannerStopJog();
    cjogOn = cjogHeld = false;
    for (uint8_t k = 0; k < 4; k++) cjogWant[k] = 0;
}

// Queue the line for direction `d` (one axis set). Returns a NACK reason, or 0
// once queued.
static uint8_t cjogStart(const int8_t d[4], uint8_t speed) {
    if (!machineCfgValid()) return MSEG_NACK_BAD_STATE;
    const MachineCfg& cfg = machineCfg();
    float at[4];
    if (!plannerJogFrom(at, JOGGING_CONT)) return MSEG_NACK_BAD_STATE;

    // How far each moving axis may go: to its soft end when homed with
    // softLimits, else maxTravel; A one CJOG_A_RUN. A diagonal stops at the
    // nearer end.
    float run = INFINITY, feed = INFINITY, step = 0;
    for (uint8_t k = 0; k < 4; k++) {
        if (d[k] == 0) continue;
        const CfgAxis* a = framesAxis(k);
        if (!a) return MSEG_NACK_BAD_STATE;
        const bool homed = axes_homed & (1u << k);
        if (!homed && !cfg.jogUnhomed) return MSEG_NACK_BAD_STATE;
        float lo, hi, room = k == SLOT_A ? CJOG_A_RUN : a->maxTravel;
        if (k != SLOT_A && homed && a->softLimits && configAxisRange(*a, &lo, &hi))
            room = d[k] > 0 ? hi - at[k] : at[k] - lo;
        run = fminf(run, room);
        float f = (homed ? a->jogFeed : a->jogFeedUnhomed) * speed / 64.0f;
        if (a->maxFeed > 0) f = fminf(f, a->maxFeed);
        feed = fminf(feed, f);
        step = fmaxf(step, 1 / a->stepsPerUnit);
    }
    if (!(run > step)) return MSEG_NACK_SOFT_LIMIT;
    if (!(feed > 0)) return MSEG_NACK_BAD_STATE;

    const PlannerQueueResult r =
        d[2] != 0 ? plannerQueueAxis(SLOT_Z, d[2] * run, feed, JOGGING_CONT)
      : d[3] != 0 ? plannerQueueAxis(SLOT_A, d[3] * run, feed, JOGGING_CONT)
      : plannerQueueLine(at[0] + d[0] * run, at[1] + d[1] * run, feed, JOGGING_CONT);
    if (r == PQ_SOFT_LIMIT) return MSEG_NACK_SOFT_LIMIT;   // the mesh leaves Z no room
    if (r != PQ_OK) return MSEG_NACK_BAD_STATE;
    for (uint8_t k = 0; k < 4; k++) cjogDir[k] = d[k];
    cjogSpeed = speed;
    cjogOn = cjogHeld = true;
    return 0;
}

static void acceptCjog() {
    const int8_t d[4] = { (int8_t)pktBuf[1], (int8_t)pktBuf[2], (int8_t)pktBuf[3], (int8_t)pktBuf[4] };
    const uint8_t speed = pktBuf[5];
    for (uint8_t k = 0; k < 4; k++)
        if (d[k] < -1 || d[k] > 1) { sendNack(MSEG_NACK_BAD_STATE); return; }
    if (speed == 0) { sendNack(MSEG_NACK_BAD_STATE); return; }
    const uint8_t sets = (d[0] != 0 || d[1] != 0) + (d[2] != 0) + (d[3] != 0);
    if (sets > 1) { sendNack(MSEG_NACK_MIXED_AXES); return; }

    cjogLastMs = millis();
    if (sets == 0) { cjogStop(); return; }
    if (cjogHeld && memcmp(d, cjogDir, 4) == 0) return;   // a repeat

    // Another direction, or step jogs running: stop them, start once at rest.
    const bool busy = cjogOn || plannerQueueDepth() != 0 || plannerActive || abortRequested;
    if (busy) {
        if (cjogOn || joggingReason == JOGGING_STEP) plannerStopJog();
        cjogOn = cjogHeld = false;
        for (uint8_t k = 0; k < 4; k++) cjogWant[k] = d[k];
        cjogSpeed = speed;
        return;
    }
    if (const uint8_t why = cjogStart(d, speed)) { sendNack(why); return; }
    sendAck();
}

// The deadman, the end of a jog, and a start held for rest.
static void cjogTick() {
    const bool silent = (millis() - cjogLastMs) > CJOG_DEADMAN_MS;
    const bool empty = !plannerActive && plannerQueueDepth() == 0;
    if (cjogOn && empty) cjogOn = false;              // ran to the end of travel
    if (cjogHeld && silent) cjogStop();
    if (!cjogWanted()) return;
    if (silent) { for (uint8_t k = 0; k < 4; k++) cjogWant[k] = 0; return; }
    if (!empty || abortRequested || machineState != STATE_IDLE) return;
    int8_t d[4];
    for (uint8_t k = 0; k < 4; k++) { d[k] = cjogWant[k]; cjogWant[k] = 0; }
    if (const uint8_t why = cjogStart(d, cjogSpeed)) sendNack(why);
    else sendAck();
}

// ─── Public Interface ─────────────────────────────────────────────────────────

bool dataPlaneConsume(uint8_t b) {
    switch (rxKind) {
        case RX_PACKET:  feedPacket(b);  return true;
        case RX_CFG:     feedCfg(b);     return true;
        case RX_NONE:    break;
    }

    // Idle — dispatch on the leading magic byte.
    if (b == MSEG_MAGIC || b == JOG_MAGIC || b == BEZIER_MAGIC || b == CJOG_MAGIC) {
        pktBuf[0] = b;                          // remember stream type for the state gate
        pktIdx    = 1;
        pktSize   = b == BEZIER_MAGIC ? BEZIER_PACKET_SIZE
                  : b == CJOG_MAGIC   ? CJOG_PACKET_SIZE : MSEG_PACKET_SIZE;
        pktLastMs = millis();
        rxKind    = RX_PACKET;
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
    if (b == ABORT_MAGIC) {                      // synchronous — no receive state
        // Record intent only; Core 1 owns the ramp and the transition (Layer 5).
        // No reply: like `stop` this correlates nothing, and confirmation
        // arrives on the status plane as the state settles to IDLE.
        abortRequested = true;
        __dmb();
        plannerEndContour();
        return true;
    }
    if (b == CJOG_STOP_MAGIC) {                  // synchronous — no reply
        cjogStop();
        return true;
    }
    if (b == SEQRESET_MAGIC) {                   // synchronous — no receive state
        dataPlaneResetSeq();                     // also drops any deferred ACK
        flushAck();                              // ACK(0) — immediate: it is the
        return true;                             // host's go-ahead, not a receipt
    }
    return false;                               // control-plane (text) byte
}

// Called every Core 0 loop pass, i.e. once the inbound stream has drained — which
// makes it both the flush point for deferred ACKs and the only place the
// byte-driven receivers can notice that nothing more is coming.
void dataPlaneTick() {
    // Never go idle dirty. The host may be waiting on exactly these ACKs to open
    // its window, so holding them while there is nothing left to read deadlocks
    // the stream. Deferral is only ever an optimisation over a busy wire.
    if (pendingAcks) flushAck();

    cjogTick();

    if (rxKind == RX_CFG && (millis() - cfgLastMs) > CFG_RX_TIMEOUT_MS) {
        rxKind = RX_NONE;
        sendCfgNack(CFG_NACK_TIMEOUT);
    }

    // A half-received packet is unrecoverable: its remaining bytes are gone, and
    // whatever arrives next would be consumed as packet body and then fail CRC.
    // Drop it silently — the host is not waiting on a reply for a frame it never
    // finished sending, and its own ACK timeout will retransmit.
    if (rxKind == RX_PACKET && (millis() - pktLastMs) > PACKET_RX_TIMEOUT_MS) {
        rxKind = RX_NONE;
        pktIdx = 0;
    }
}

void dataPlaneReset() {
    rxKind      = RX_NONE;
    pktIdx      = 0;
    expectedSeq = 0;
    pendingAcks = 0;
    cfgHdrIdx   = 0;
    cfgRxCnt    = 0;
    plannerEndContour();
}

void dataPlaneResetSeq() {
    expectedSeq = 0;
    pendingAcks = 0;    // any deferred ACK names the pre-reset numbering
    plannerEndContour();
}

uint8_t dataPlaneExpectedSeq() {
    return expectedSeq;
}
