// Core 0 data plane: binary ingest for the USB CDC stream.
//
// One byte-dispatcher (dataPlaneConsume) serves two receivers, selected by the
// leading magic byte and tracked in rxKind (docs/wire_protocol.md):
//   RX_PACKET  — BEZIER (0xAD), 56 bytes; LAYER_HDR (0xA1), 22 bytes;
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
#include "ops/axes_map.h"
#include "ops/frames.h"
#include "ops/layer.h"
#include "ops/tools.h"
#include "ops/cjog.h"
#include "ops/machine.h"           // jobAbort
#include "controller/seq/toolchange.h"
#include "planner/queue.h"
#include <planner/bezier.h>

// ─── Receiver dispatch ────────────────────────────────────────────────────────

enum RxKind : uint8_t { RX_NONE, RX_PACKET, RX_CFG };
static RxKind rxKind = RX_NONE;

// ─── Packet (BEZIER / jog) state ───────────────────────────────────────────────

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

static void cjogReply(uint8_t r);               // Continuous jog, below

static void sendCfgRdy()         { Serial.write(CFG_RDY); }   // header ok — send payload
static void sendCfgAck()           { Serial.write(CFG_ACK); }   // committed
static void sendCfgNack(uint8_t r) { Serial.write(CFG_NACK); Serial.write(r); }

static void acceptBezier();
static void acceptLayer();
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
        if (pktBuf[0] == CJOG_MAGIC) cjogReply(MSEG_NACK_CRC);
        else                         sendNack(MSEG_NACK_CRC);
        return;
    }

    // Before the abort barrier: a direction change brakes with it raised.
    if (pktBuf[0] == CJOG_MAGIC) { acceptCjog(); return; }

    // Abort is a barrier — reject everything until the machine reaches IDLE,
    // with a reason distinct from BAD_STATE so the host waits and reopens rather
    // than surfacing an error.
    if (abortRequested || (machineContext == CTX_JOB && jobReason == JOB_ABORTING)) {
        sendNack(MSEG_NACK_ABORTING);
        return;
    }

    // A job holding for a tool change takes nothing until it is back, except a
    // header in TOOL_WAIT, which may bring it back.
    const bool layer = pktBuf[0] == LAYER_HDR_MAGIC;
    if (toolChangeHolding() && !(layer && toolChangeWaiting())) {
        sendNack(MSEG_NACK_TOOL_CHANGE);
        return;
    }

    // A layer header opens a job from IDLE or resumes a paused one, and records
    // join it; the planner's admit narrows JOB to the reasons that take them.
    const uint8_t st = machineView();
    if (st == STATE_JOB && jobReason == JOB_PAUSED && !layer) { sendNack(MSEG_NACK_PAUSED); return; }
    if (st != STATE_IDLE && st != STATE_JOB)        { sendNack(MSEG_NACK_BAD_STATE); return; }

    // Duplicate guard: the seq byte carries the host's rolling 8-bit seq. After a
    // NACK the host rewinds (Go-Back-N) and may resend packets we already
    // accepted; executing them again would duplicate motion — a permanent
    // position offset. A seq we are not expecting is a stale retransmit: ACK it
    // (so the host's window advances) but do not execute. The host resets this
    // counter with the "seqreset" text command before each stream.
    if (pktBuf[BEZIER_SEQ_OFFSET] != expectedSeq) {
        flushAck();     // immediate: this duplicate ACK is the host's resync signal
        return;
    }

    if (layer) acceptLayer();
    else                              acceptBezier();
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
    memcpy(&bz.ts[2],      p, 4); p += 4;
    uint16_t breakDist;
    int16_t sweepMin, sweepMax;
    memcpy(&breakDist,     p, 2); p += 2;
    memcpy(&sweepMin,      p, 2); p += 2;
    memcpy(&sweepMax,      p, 2);

    // Work → machine: a translation, so the analysed fields stand as sent.
    for (int i = 0; i < 4; i++) {
        planner::Vec2& q = bz.p[i];
        if (framesToMachine(q.x, q.y, &q.x, &q.y)) { sendNack(MSEG_NACK_BAD_STATE); return; }
        if (framesCheckXY(q.x, q.y)) { sendNack(MSEG_NACK_SOFT_LIMIT); return; }
    }

    const uint8_t flags = pktBuf[1];
    switch (plannerQueueRecord(bz, flags & BEZIER_FLAG_START, flags & BEZIER_FLAG_BREAK,
                               flags & BEZIER_FLAG_END,
                               breakDist, sweepMin, sweepMax)) {
        case PQ_OK:        break;
        case PQ_FULL:      sendNack(MSEG_NACK_FULL);      return;
        case PQ_BAD_CURVE: sendNack(MSEG_NACK_BAD_CURVE); return;
        case PQ_SOFT_LIMIT: sendNack(MSEG_NACK_SOFT_LIMIT); return;
        case PQ_NO_JOB:    sendNack(MSEG_NACK_JOB);       return;
        default:           sendNack(MSEG_NACK_BAD_STATE); return;   // state, config, limits, feed
    }
    expectedSeq++;
    markAck();
}

// The layer names its tool and head. A head switch maps axes over the bus, so
// it happens here only while the bus is free (IDLE, or a paused job resuming);
// otherwise, or with the tool not mounted on the head, the job holds for a tool
// change and the header is NACKed, not consumed.
static void acceptLayer() {
    if (pauseRequested) { sendNack(MSEG_NACK_PAUSED); return; }
    if (!machineCfgValid()) { sendNack(MSEG_NACK_BAD_STATE); return; }

    LayerHdr l;
    l.chamber = pktBuf[1];
    l.tool    = pktBuf[3];
    l.head    = pktBuf[4];
    uint16_t layerNo;
    uint32_t record;
    memcpy(&layerNo, &pktBuf[5], 2);
    memcpy(&record,  &pktBuf[7], 4);
    memcpy(&l.feed,  &pktBuf[11], 4);
    memcpy(&l.p,     &pktBuf[15], 4);
    uint16_t um;
    memcpy(&um,      &pktBuf[19], 2);
    l.thickness = um * 0.001f;

    const MachineCfg& c = machineCfg();
    uint8_t mounted[CFG_MAX_HEADS];
    for (uint8_t h = 0; h < CFG_MAX_HEADS; h++) mounted[h] = toolsMounted(h);
    LayerMotion mo;
    const LayerResult res = layerResolve(c, mounted, l, plannerMachineFeed(), &mo);
    switch (res) {
        case LAYER_OK:           break;
        case LAYER_NOT_MOUNTED:  break;
        case LAYER_TOOL_UNKNOWN: sendNack(MSEG_NACK_TOOL_UNKNOWN); return;
        default:                 sendNack(MSEG_NACK_JOB);          return;
    }
    // In TOOL_WAIT only this header leaves: its tool mounted and zeroed, the
    // job goes back and the header is taken as any other.
    if (toolChangeWaiting() && !toolChangeLeave(l.head, l.tool)) {
        sendNack(MSEG_NACK_TOOL_CHANGE);
        return;
    }
    // Lifts and plunges are heights over the head's work Z: a head not zeroed
    // since its tool was mounted stops for it as for a mount.
    const bool ready = res == LAYER_OK && framesWorkZSet(l.head);

    // Checked before the select or the change, so a refused layer changes nothing.
    PlannerQueueResult r = plannerLayerCheck(mo.cutFeed, layerNo, record);
    if (r == PQ_OK && (!ready || l.head != framesSelected())) {
        if (!ready || !machineBusFree()) {
            sendNack(toolChangeBegin(l.head, l.tool) ? MSEG_NACK_TOOL_CHANGE : MSEG_NACK_BAD_STATE);
            return;
        }
        const char* why;
        axesSelectHead(l.head, &why);
        if (why || framesSelected() != l.head) { sendNack(MSEG_NACK_BAD_STATE); return; }
    }
    if (r == PQ_OK) r = plannerOpenLayer(mo, layerNo, record);
    switch (r) {
        case PQ_OK:        break;
        case PQ_BAD_CURVE: sendNack(MSEG_NACK_JOB);       return;   // inside a contour
        case PQ_NO_RESUME: sendNack(MSEG_NACK_JOB);       return;
        default:           sendNack(MSEG_NACK_BAD_STATE); return;   // state, config, feed
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
        if (!machineAtRest() || machineContext != CTX_IDLE) {
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
    toolsReset();                           // its ids may name other tools
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
// ops/cjog.cpp; here only the replies.

static void cjogReply(uint8_t r) {
    if (r == CJOG_NO_REPLY) return;
    if (r == CJOG_ACK) { Serial.write(JOG_ACK); return; }
    const uint8_t f[2] = { JOG_NACK, r };
    Serial.write(f, 2);
}

static void acceptCjog() {
    const int8_t d[4] = { (int8_t)pktBuf[1], (int8_t)pktBuf[2], (int8_t)pktBuf[3], (int8_t)pktBuf[4] };
    cjogReply(cjogPacket(d, pktBuf[5], millis()));
}

// ─── Public Interface ─────────────────────────────────────────────────────────

bool dataPlaneConsume(uint8_t b) {
    switch (rxKind) {
        case RX_PACKET:  feedPacket(b);  return true;
        case RX_CFG:     feedCfg(b);     return true;
        case RX_NONE:    break;
    }

    // Idle — dispatch on the leading magic byte.
    if (b == BEZIER_MAGIC || b == LAYER_HDR_MAGIC || b == CJOG_MAGIC) {
        pktBuf[0] = b;
        pktIdx    = 1;
        pktSize   = b == BEZIER_MAGIC    ? BEZIER_PACKET_SIZE
                  : b == LAYER_HDR_MAGIC ? LAYER_HDR_PACKET_SIZE
                  :                        CJOG_PACKET_SIZE;
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
        jobAbort();
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

    cjogReply(cjogTick(millis()));

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
