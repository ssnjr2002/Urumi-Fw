/**
 * packet.ts — 26-byte MicroSegment wire packet packer.
 * Ported from host/protocol/packets.py (pack_microsegment, _crc8,
 * serialise_microsegments, write_stream).
 *
 * The wire format the RP2350 firmware expects. One MicroSegment -> one
 * 26-byte packet; a .bin stream is length-prefixed framing:
 *   [u16 LE 26][26-byte packet] per packet, concatenated.
 *
 * Packet layout (all little-endian):
 *   [0]      magic    0xAB
 *   [1..4]   dx       int32 LE   X steps (signed)
 *   [5..8]   dy       int32 LE   Y steps (signed)
 *   [9..12]  dz       int32 LE   Z steps (signed, +lift / -lower)
 *   [13..16] da       int32 LE   A steps (signed, tangential rotation)
 *   [17..20] interval uint32 LE  step interval in RP2350 CPU cycles
 *   [21]     flags    uint8      MSEG_FLAG_* bitmask
 *   [22]     seq      uint8      rolling sequence number (stamped by the
 *                                sender; we emit 0 here — stamping is the
 *                                sender's job, not the baker's)
 *   [23..24] pad      2 bytes    zero (matches C struct alignment)
 *   [25]     CRC8     over bytes [0..24], polynomial 0x8C
 */

import type { MicroSegment } from "./microsegment.js";

// ── magic bytes ───────────────────────────────────────────────────────────────

export const MAGIC_MICROSEG = 0xab;
export const PACKET_SIZE = 26;
export const FRAME_PREFIX_SIZE = 2; // u16 LE length prefix
export const FRAMED_PACKET_SIZE = FRAME_PREFIX_SIZE + PACKET_SIZE; // 28

// ── CRC-8 (polynomial 0x8C, matches Pico firmware + Python _crc8) ─────────────

/**
 * CRC-8 over data[start..end] (exclusive end). Polynomial 0x8C, init 0x00,
 * reflected — the "Dallas/Maxim" 1-Wire variant the firmware uses.
 *
 * Default range is the whole buffer. The start/end form lets the caller
 * CRC a slice without allocating a subarray.
 */
export function crc8(data: ArrayLike<number>, start = 0, end = data.length): number {
    let crc = 0x00;
    for (let i = start; i < end; i++) {
        crc ^= data[i]!;
        for (let _ = 0; _ < 8; _++) {
            if (crc & 0x01) {
                crc = (crc >> 1) ^ 0x8c;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc & 0xff;
}

// ── 26-byte MicroSegment packer ───────────────────────────────────────────────

/**
 * Pack a MicroSegment into a 26-byte Uint8Array (the wire packet).
 * seq is stamped 0 — the sender (job_runner) stamps a rolling sequence
 * number into byte [22] before transmission; the baker emits 0.
 */
export function packMicrosegment(ms: MicroSegment, seq = 0): Uint8Array {
    const buf = new ArrayBuffer(PACKET_SIZE);
    const dv = new DataView(buf);
    const u8 = new Uint8Array(buf);

    dv.setUint8(0, MAGIC_MICROSEG);
    dv.setInt32(1, ms.dx, true);       // little-endian
    dv.setInt32(5, ms.dy, true);
    dv.setInt32(9, ms.dz, true);
    dv.setInt32(13, ms.da, true);
    dv.setUint32(17, ms.interval, true);
    dv.setUint8(21, ms.flags & 0xff);
    dv.setUint8(22, seq & 0xff);
    // bytes [23..24] pad — zeroed by ArrayBuffer init
    dv.setUint8(25, crc8(u8, 0, PACKET_SIZE - 1));

    return u8;
}

// ── stream serialisers ────────────────────────────────────────────────────────

/**
 * Yield one 26-byte Uint8Array per MicroSegment. Mirrors Python
 * serialise_microsegments.
 */
export function* serialiseMicrosegments(
    segments: Iterable<MicroSegment>,
): Generator<Uint8Array> {
    for (const ms of segments) {
        yield packMicrosegment(ms);
    }
}

/**
 * Length-prefixed framing: [u16 LE 26][26-byte packet] per packet,
 * concatenated into one Uint8Array. Mirrors Python write_stream.
 *
 * This is the .bin file format — what host.production.svg_to_packets
 * writes with --out, and what the TS bake produces for parity comparison.
 */
export function writeStream(packets: Iterable<Uint8Array>): Uint8Array {
    const parts = [...packets];
    const out = new Uint8Array(parts.length * FRAMED_PACKET_SIZE);
    const dv = new DataView(out.buffer);
    let offset = 0;
    for (const pkt of parts) {
        if (pkt.length !== PACKET_SIZE) {
            throw new Error(
                `writeStream: expected ${PACKET_SIZE}-byte packet, got ${pkt.length}`,
            );
        }
        dv.setUint16(offset, PACKET_SIZE, true); // u16 LE length prefix
        out.set(pkt, offset + FRAME_PREFIX_SIZE);
        offset += FRAMED_PACKET_SIZE;
    }
    return out;
}

// ── decoder (for parity diagnostics) ──────────────────────────────────────────

export interface DecodedPacket {
    readonly magic: number;
    readonly dx: number;
    readonly dy: number;
    readonly dz: number;
    readonly da: number;
    readonly interval: number;
    readonly flags: number;
    readonly seq: number;
    readonly crc: number;
    readonly crcOk: boolean;
}

/**
 * Decode a 26-byte packet at `offset` in `data` for parity diagnostics.
 * Verifies the CRC and exposes every field so a divergent packet can be
 * diffed field-by-field. Throws if the slice is too short.
 */
export function decodePacket(data: Uint8Array, offset = 0): DecodedPacket {
    if (offset + PACKET_SIZE > data.length) {
        throw new Error(
            `decodePacket: need ${PACKET_SIZE} bytes at offset ${offset}, have ${data.length - offset}`,
        );
    }
    const dv = new DataView(data.buffer, data.byteOffset + offset, PACKET_SIZE);
    const magic = dv.getUint8(0);
    const dx = dv.getInt32(1, true);
    const dy = dv.getInt32(5, true);
    const dz = dv.getInt32(9, true);
    const da = dv.getInt32(13, true);
    const interval = dv.getUint32(17, true);
    const flags = dv.getUint8(21);
    const seq = dv.getUint8(22);
    const crc = dv.getUint8(25);
    const expected = crc8(data, offset, offset + PACKET_SIZE - 1);
    return {
        magic, dx, dy, dz, da, interval, flags, seq, crc,
        crcOk: crc === expected,
    };
}
