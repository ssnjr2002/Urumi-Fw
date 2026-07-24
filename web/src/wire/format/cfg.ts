/**
 * format/cfg.ts — the config-blob transfer frame (Phase-2 surface).
 * Ported from host/protocol/{packets,reader}.py: the CFG_* magics that the
 * demux routes on, plus the one length-prefixed frame helper the format layer
 * owns — the CFG_DATA 9-byte header.
 *
 * Config transfer (docs/wire_protocol.md "CRC Algorithms" + §config): the
 * host pushes a MachineConfigFlash blob with CFG_SET (header, then payload on
 * RDY), or pulls it with CFG_GET (replied with CFG_DATA). The magics live in
 * constants.ts (single source for the demux's magic table); this file owns the
 * CFG_DATA header layout, the CFG NACK reason namespace, and the payload-size
 * guard.
 *
 * Why a 9-byte header gets its own file: the demux consumes CFG_DATA
 * length-prefixed — it reads 9 header bytes, pulls a u32 LE `length`, then
 * consumes exactly that many opaque payload bytes without scanning them. That
 * is the one place a length-prefixed frame appears on this wire (everything
 * else is fixed-length or text-to-newline), and the helper that packs/unpacks
 * the header is the natural home for the size constants and the payload cap
 * that bounds it.
 *
 * The packer here has no Pico-side consumer yet — the host never sends CFG_GET
 * today (comms_architecture.md §5 "a missing sender, not dead surface"). It
 * exists so the demux test can build a hostile CFG_DATA frame (header + a
 * payload full of magic bytes) to pin the phantom-ACK hazard, and so the Sim
 * can emit CFG_DATA if/when the sender lands.
 */

import { MAGIC_CFG_DATA } from "./constants.js";

// ── CFG_DATA header layout ────────────────────────────────────────────────────
//
// [0]     magic   0xB5
// [1..4]  length  u32 LE — opaque payload byte count that follows the header
// [5..8]  crc32   u32 LE — CRC32 of the payload (the blob's own integrity check)
//
// No CRC8 on the header itself — the integrity check for the payload is the
// crc32 field it carries; the header's own correctness rides on the demux's
// fixed-length consumption (9 bytes blind, like every other fixed frame).
export const CFG_DATA_HDR_SIZE = 9;

// Cap on a single CFG_DATA payload. The firmware bounds this by CFG_MAX_BYTES;
// we bound it independently so a corrupted length field cannot make the demux
// sit in the payload state forever, swallowing every other plane's traffic.
// Mirrors host.protocol.reader.MAX_CFG_PAYLOAD.
export const MAX_CFG_PAYLOAD = 64 * 1024;

// ── CFG command NACK reasons (byte[1] of a CFG_NACK frame) ────────────────────
// A SEPARATE namespace from the MSEG stream NACK reasons in constants.ts — the
// same byte values, different command class, so they live here next to the
// frame they belong to.
export const CFG_NACK_CRC = 0x01; // CRC32 mismatch — struct corrupt in transit
export const CFG_NACK_TOO_BIG = 0x02; // payload exceeds CFG_MAX_BYTES
export const CFG_NACK_BAD_STATE = 0x03; // push rejected — machine not IDLE or ALARM
export const CFG_NACK_FLASH = 0x04; // flash write failed
export const CFG_NACK_TIMEOUT = 0x05; // payload did not arrive within the transfer window

export interface CfgDataHeader {
    readonly length: number;
    readonly crc32: number;
}

/**
 * Pack a CFG_DATA header — 9 bytes: [0xB5][u32 LE length][u32 LE crc32].
 *
 * `length` is the byte count of the payload that follows (the caller appends
 * it separately); `crc32` is the CRC32 of that payload. No CRC8 on the header.
 */
export function packCfgDataHeader(length: number, crc32: number): Uint8Array {
    const buf = new ArrayBuffer(CFG_DATA_HDR_SIZE);
    const dv = new DataView(buf);
    dv.setUint8(0, MAGIC_CFG_DATA);
    dv.setUint32(1, length >>> 0, true);
    dv.setUint32(5, crc32 >>> 0, true);
    return new Uint8Array(buf);
}

/**
 * Unpack a 9-byte CFG_DATA header. Throws on wrong size or bad magic. The
 * caller is responsible for checking `length` against MAX_CFG_PAYLOAD before
 * consuming that many payload bytes — the demux does this so a corrupted
 * length cannot blind every other plane.
 */
export function unpackCfgDataHeader(data: Uint8Array): CfgDataHeader {
    if (data.length !== CFG_DATA_HDR_SIZE) {
        throw new Error(
            `CFG_DATA header: expected ${CFG_DATA_HDR_SIZE} bytes, got ${data.length}`,
        );
    }
    if (data[0] !== MAGIC_CFG_DATA) {
        throw new Error(
            `CFG_DATA header: bad magic 0x${data[0]!.toString(16).padStart(2, "0")}`,
        );
    }
    const dv = new DataView(data.buffer, data.byteOffset, CFG_DATA_HDR_SIZE);
    return {
        length: dv.getUint32(1, true),
        crc32: dv.getUint32(5, true),
    };
}