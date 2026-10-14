/**
 * layer.ts — the 17-byte LAYER_HDR: opens a job from IDLE and starts a layer
 * (src/rp2350/core0/usb_protocol.h). The BEZIERs after it run with its tool,
 * on the head that has it mounted.
 *
 * Layout (little-endian):
 *   [0]      magic 0xA2
 *   [1]      chamber  1..chamberCount; 0 for a tool without chambers
 *   [2]      seq      stamped by the sender (stampSeq); 0 here
 *   [3]      tool     the config tool's `id`
 *   [4..7]   index    u32, of the next BEZIER
 *   [8..11]  feed     f32 mm/s; 0 = the tool's path.feed, then the machine's
 *   [12..15] k        f32 plunge scale, 0..2
 *   [16]     CRC8 over bytes [0..15]
 */

import { LAYER_HDR_SEQ_OFFSET, LAYER_HDR_SIZE, MAGIC_LAYER_HDR } from "./constants.js";
import { crc8 } from "./crc.js";

export interface LayerHdr {
    readonly tool: number;
    readonly chamber?: number;
    readonly index?: number;
    readonly feed?: number;
    readonly k?: number;
}

export function packLayerHdr(l: LayerHdr, seq = 0): Uint8Array {
    const u8 = new Uint8Array(LAYER_HDR_SIZE);
    const dv = new DataView(u8.buffer);
    dv.setUint8(0, MAGIC_LAYER_HDR);
    dv.setUint8(1, (l.chamber ?? 0) & 0xff);
    dv.setUint8(LAYER_HDR_SEQ_OFFSET, seq & 0xff);
    dv.setUint8(3, l.tool & 0xff);
    dv.setUint32(4, l.index ?? 0, true);
    dv.setFloat32(8, l.feed ?? 0, true);
    dv.setFloat32(12, l.k ?? 1, true);
    u8[LAYER_HDR_SIZE - 1] = crc8(u8, 0, LAYER_HDR_SIZE - 1);
    return u8;
}
