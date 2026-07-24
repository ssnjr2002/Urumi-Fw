/**
 * format/constants.ts — wire-magic bytes and fixed packet sizes.
 *
 * The single source of truth for the magic-byte dispatch table the firmware
 * uses (docs/wire_protocol.md). Each data-plane frame is keyed by its first
 * byte, so the magic constants here are imported by every per-format module
 * (packet, status, tool, spline, cfg) and by the link-layer demux.
 *
 * Constants for frames the host does not yet emit (JOG, TOOL, SPLINE, STATUS_*,
 * SEQRESET, ABORT, CFG_*) land in the commits that add their packers; this
 * file starts with just the MicroSegment magic + size that the pre-split
 * wire/packet.ts carried.
 */

// ── MicroSegment / Jog — 26-byte packets, shared layout ───────────────────────
export const MAGIC_MICROSEG = 0xab; // host → Pico: pre-computed step event
export const MAGIC_JOG = 0xae; // host → Pico: operator jog packet (same 26B layout)
export const PACKET_SIZE = 26;