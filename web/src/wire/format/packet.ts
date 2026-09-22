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
import { MAGIC_JOG, MAGIC_MICROSEG, PACKET_SIZE } from "./constants.js";
import { crc8 } from "./crc.js";

// ── .bin framing (length-prefixed) ──────────────────────────────────────────────
// The .bin file format packs each 26-byte packet with a u16 LE length prefix;
// these are file-format constants, separate from the wire PACKET_SIZE.
export const FRAME_PREFIX_SIZE = 2; // u16 LE length prefix
export const FRAMED_PACKET_SIZE = FRAME_PREFIX_SIZE + PACKET_SIZE; // 28

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

/** Pack a MicroSegment as a JOG packet (magic 0xAE, same 26-byte layout as MSEG). */
export function packJog(ms: MicroSegment, seq = 0): Uint8Array {
    const buf = new ArrayBuffer(PACKET_SIZE);
    const dv = new DataView(buf);
    const u8 = new Uint8Array(buf);

    dv.setUint8(0, MAGIC_JOG);
    dv.setInt32(1, ms.dx, true);
    dv.setInt32(5, ms.dy, true);
    dv.setInt32(9, ms.dz, true);
    dv.setInt32(13, ms.da, true);
    dv.setUint32(17, ms.interval, true);
    dv.setUint8(21, ms.flags & 0xff);
    dv.setUint8(22, seq & 0xff);
    dv.setUint8(25, crc8(u8, 0, PACKET_SIZE - 1));

    return u8;
}

/**
 * Stamp a rolling 8-bit sequence number into pad byte [22] of a MicroSegment /
 * Jog packet and recompute the CRC. The Pico only executes a packet whose seq
 * matches the one it expects next; a stale Go-Back-N retransmit (one it already
 * accepted) is ACKed but NOT executed. Without that, any go-back after the
 * Pico accepted in-flight packets would duplicate motion — a permanent position
 * offset. Ported from host/protocol/packets.py stamp_seq.
 */
export function stampSeq(packet: Uint8Array, seq: number): Uint8Array {
    const magic = packet[0];
    if (packet.length !== PACKET_SIZE || (magic !== MAGIC_MICROSEG && magic !== MAGIC_JOG)) {
        throw new Error("stampSeq: not a 26-byte MicroSegment or Jog packet");
    }
    const out = new Uint8Array(PACKET_SIZE);
    out.set(packet.subarray(0, PACKET_SIZE - 1));
    out[22] = seq & 0xff;
    out[PACKET_SIZE - 1] = crc8(out, 0, PACKET_SIZE - 1);
    return out;
}

/**
 * Unpack a 26-byte MSEG/JOG packet into a MicroSegment. Throws on bad magic or
 * CRC. The inverse of packMicrosegment; used by the in-process SimTransport to
 * extract the deltas it integrates into position. Ported from Python
 * unpack_microsegment.
 */
export function unpackMicrosegment(data: Uint8Array): MicroSegment {
    if (data.length !== PACKET_SIZE) {
        throw new Error(`Expected ${PACKET_SIZE} bytes, got ${data.length}`);
    }
    const magic = data[0];
    if (magic !== MAGIC_MICROSEG && magic !== MAGIC_JOG) {
        throw new Error(`Bad magic: 0x${magic!.toString(16).padStart(2, "0")} (expected MSEG or JOG)`);
    }
    if (crc8(data, 0, PACKET_SIZE - 1) !== data[PACKET_SIZE - 1]) {
        throw new Error("CRC mismatch");
    }
    const dv = new DataView(data.buffer, data.byteOffset, PACKET_SIZE);
    return {
        dx: dv.getInt32(1, true),
        dy: dv.getInt32(5, true),
        dz: dv.getInt32(9, true),
        da: dv.getInt32(13, true),
        interval: dv.getUint32(17, true),
        flags: dv.getUint8(21),
    };
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
