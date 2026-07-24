/**
 * format/constants.ts — wire-magic bytes and fixed packet sizes.
 *
 * The single source of truth for the magic-byte dispatch table the firmware
 * uses (docs/wire_protocol.md). Each data-plane frame is keyed by its first
 * byte, so the magic constants here are imported by every per-format module
 * (packet, status, tool, spline, cfg) and by the link-layer demux.
 *
 * Constants for frames the host does not yet emit land in the commits that
 * add their packers; the TOOL (0xAC) and SPLINE (0xAD) packers are
 * deliberately NOT ported — they are declared-but-unimplemented wire surface
 * (docs/comms_architecture.md §4.7) with no consumer on this branch, and a
 * speculative packer that nothing exercises could drift from whatever the
 * spline-streaming branch eventually settles on. Add them when there is a
 * caller.
 */

// ── MicroSegment / Jog — 26-byte packets, shared layout ───────────────────────
export const MAGIC_MICROSEG = 0xab; // host → Pico: pre-computed step event
export const MAGIC_JOG = 0xae; // host → Pico: operator jog packet (same 26B layout)
export const PACKET_SIZE = 26;

// ── Status plane — binary mirror of `getstate` (+ getpos, queued time) ─────────
// docs/wire_protocol.md "STATUS_RSP". The magic bumped from 0xA6 to 0xA7 when
// the frame grew 9 → 30 bytes (pos[4], expectedSeq, queuedUs). The retired
// magic is reserved, never emitted, and parsed as unknown rather than
// mis-parsed as 9 bytes — see format/status.ts parseStatusRsp for the message.
export const MAGIC_STATUS_REQ = 0xa5; // host → Pico: one-byte request, no payload/CRC
export const MAGIC_STATUS_RSP = 0xa7; // Pico → host: 30-byte status snapshot
export const MAGIC_STATUS_RSP_V1 = 0xa6; // retired 9-byte frame; reserved, never emitted
export const STATUS_RSP_SIZE = 30;

// ── Stream control replies (Pico → host) — fixed 3-byte frames ────────────────
// docs/wire_protocol.md "ACK" / "NACK". Cumulative ACK: byte[1] = the next wire
// seq the Pico wants (i.e. every packet below was accepted), so a lost or stale
// ACK self-heals on the next one and a duplicate is idempotent. The sender
// decodes the advance in 8-bit rolling space, clamped to the in-flight window.
export const MAGIC_ACK = 0xaa;
export const MAGIC_NACK = 0xbb;

// ── Binary stream control (host → Pico) — single-byte, no payload/CRC ──────────
// SEQRESET zeroes expectedSeq and replies ACK(0) — keeps stream start on the
// data plane instead of dragging it through the one-outstanding text plane
// (§4.3). ABORT is a soft stop: ramps to rest, flushes the ring, lands IDLE
// with position intact (§4.5). Fire-and-forget; confirmation arrives on the
// status sink as the state settles.
export const MAGIC_SEQRESET = 0xa8;
export const MAGIC_ABORT = 0xa9;

// ── MSEG stream NACK reasons (byte[1] of a NACK frame) ─────────────────────────
// Two namespaces: these are the stream/MSEG/jog reasons. CFG command NACK
// reasons live in cfg.ts (a different namespace, same byte values).
export const NACK_CRC = 0x01; // CRC8 mismatch — packet corrupt
export const NACK_FULL = 0x02; // ring buffer full — backpressure, sender retries
export const NACK_BAD_MAGIC = 0x03; // unrecognised magic byte — fatal
export const NACK_PAUSED = 0x04; // stream rejected — machine is PAUSED
export const NACK_BAD_STATE = 0x06; // command rejected — wrong machine state
// Barrier, not an error: stream rejected because the machine is mid-abort-ramp.
// The host waits for IDLE and reopens rather than surfacing a failure (§4.5).
export const NACK_ABORTING = 0x07;

// ── Config plane magics (Phase-2) — CFG blob push/pull over the data plane ──────
// docs/wire_protocol.md "CRC Algorithms" + packets.py. CFG_SET/GET are host→Pico
// requests; the Pico replies with CFG_RDY (header accepted, send payload),
// CFG_ACK (committed), CFG_NACK (rejected — next byte is the reason), or
// CFG_DATA (the GET response: 9-byte header + opaque payload). The header
// pack/unpack helpers + the CFG NACK reasons live in cfg.ts.
export const MAGIC_CFG_SET = 0xb0; // host → Pico: header, then (on RDY) payload
export const MAGIC_CFG_GET = 0xb1; // host → Pico: request the active blob
export const MAGIC_CFG_RDY = 0xb2; // Pico → host: header accepted — send payload
export const MAGIC_CFG_ACK = 0xb3; // Pico → host: blob committed
export const MAGIC_CFG_NACK = 0xb4; // Pico → host: rejected — next byte is the reason
export const MAGIC_CFG_DATA = 0xb5; // Pico → host: CFG_GET response (9 + length)