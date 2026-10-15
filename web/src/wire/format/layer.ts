/**
 * layer.ts — the 19-byte LAYER_HDR: opens a job from IDLE and starts a layer,
 * or resumes a paused job (src/rp2350/core0/usb_protocol.h). The BEZIERs after
 * it run with its tool, on the head that has it mounted, numbered on from
 * `record`.
 *
 * Layout (little-endian):
 *   [0]      magic 0xA2
 *   [1]      chamber  1..chamberCount; 0 for a tool without chambers
 *   [2]      seq      stamped by the sender (stampSeq); 0 here
 *   [3]      tool     the config tool's `id`
 *   [4..5]   layer    u16
 *   [6..9]   record   u32, the next BEZIER's number within the layer
 *   [10..13] feed     f32 mm/s; 0 = the tool's path.feed, then the machine's
 *   [14..17] k        f32 plunge scale, 0..2
 *   [18]     CRC8 over bytes [0..17]
 */

import { LAYER_HDR_SEQ_OFFSET, LAYER_HDR_SIZE, MAGIC_LAYER_HDR } from "./constants.js";
import { crc8 } from "./crc.js";

export interface LayerHdr {
    readonly tool: number;
    readonly chamber?: number;
    readonly layer?: number;
    readonly record?: number;
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
    dv.setUint16(4, l.layer ?? 0, true);
    dv.setUint32(6, l.record ?? 0, true);
    dv.setFloat32(10, l.feed ?? 0, true);
    dv.setFloat32(14, l.k ?? 1, true);
    u8[LAYER_HDR_SIZE - 1] = crc8(u8, 0, LAYER_HDR_SIZE - 1);
    return u8;
}
