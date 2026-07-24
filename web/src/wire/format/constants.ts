/**
 * format/constants.ts — wire-magic bytes and fixed packet sizes.
 *
 * The single source of truth for the magic-byte dispatch table the firmware
 * uses (docs/wire_protocol.md). Each data-plane frame is keyed by its first
 * byte, so the magic constants here are imported by every per-format module
 * (packet, status, tool, spline, cfg) and by the link-layer demux.
 *
 * Constants for frames the host does not yet emit (TOOL, SPLINE, SEQRESET,
 * ABORT, CFG_*) land in the commits that add their packers; this file grows
 * as each per-format module is added.
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