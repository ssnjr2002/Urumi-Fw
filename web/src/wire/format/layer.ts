/**
 * layer.ts — the 22-byte LAYER_HDR: opens a job from IDLE and starts a layer,
 * or resumes a paused job (src/rp2350/core0/usb_protocol.h). The BEZIERs after
 * it run with its tool on `head`, numbered on from `record`. Without the tool
 * mounted there and the head selected it is NACK_TOOL_CHANGE.
 *
 * Layout (little-endian):
 *   [0]      magic 0xA1
 *   [1]      chamber  1..chamberCount; 0 for a tool without chambers
 *   [2]      seq      stamped by the sender (stampSeq); 0 here
 *   [3]      tool     the config tool's `id`
 *   [4]      head     the head the tool goes on
 *   [5..6]   layer    u16
 *   [7..10]  record   u32, the next BEZIER's number within the layer
 *   [11..14] feed     f32 mm/s; 0 = the tool's cutFeed, then the machine's
 *   [15..18] p        f32 plunge depth below the material top, mm
 *   [19..20] t        u16 material thickness, µm
 *   [21]     CRC8 over bytes [0..20]
 *
 * Work Z 0 is the bed: the cut is at t − p, so a through cut is p = t plus
 * the overcut into the mat.
 */

import { LAYER_HDR_SEQ_OFFSET, LAYER_HDR_SIZE, MAGIC_LAYER_HDR } from "./constants.js";
import { crc8 } from "./crc.js";

export interface LayerHdr {
    readonly tool: number;
    readonly head: number;
    readonly chamber?: number;
    readonly layer?: number;
    readonly record?: number;
    readonly feed?: number;
    /** Plunge depth below the material top, mm; defaults to `thickness` (a through cut to the bed). */
    readonly p?: number;
    /** Material thickness, mm (to 65.535). */
    readonly thickness?: number;
}

export function packLayerHdr(l: LayerHdr, seq = 0): Uint8Array {
    const u8 = new Uint8Array(LAYER_HDR_SIZE);
    const dv = new DataView(u8.buffer);
    dv.setUint8(0, MAGIC_LAYER_HDR);
    dv.setUint8(1, (l.chamber ?? 0) & 0xff);
    dv.setUint8(LAYER_HDR_SEQ_OFFSET, seq & 0xff);
    dv.setUint8(3, l.tool & 0xff);
    dv.setUint8(4, l.head & 0xff);
    dv.setUint16(5, l.layer ?? 0, true);
    dv.setUint32(7, l.record ?? 0, true);
    dv.setFloat32(11, l.feed ?? 0, true);
    dv.setFloat32(15, l.p ?? l.thickness ?? 0, true);
    const um = Math.round((l.thickness ?? 0) * 1000);
    if (!(um >= 0 && um <= 0xffff)) throw new RangeError(`LAYER_HDR: thickness ${l.thickness} mm out of range`);
    dv.setUint16(19, um, true);
    u8[LAYER_HDR_SIZE - 1] = crc8(u8, 0, LAYER_HDR_SIZE - 1);
    return u8;
}
