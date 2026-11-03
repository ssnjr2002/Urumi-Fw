/**
 * layer.ts — a job's headers (src/rp2350/core0/usb_protocol.h). JOB_HDR and one
 * TOOL_HDR per tool open it; then each new layer is LAYER_HDR, TOOL_HDR, BEZIERs,
 * and a resume from JOB.PAUSED is LAYER_HDR alone. All little-endian, seq at
 * byte 2, stamped by the sender (stampSeq), CRC8 last.
 *
 * JOB_HDR (17 B):
 *   [0] magic 0xB8  [1] version  [2] seq  [3..6] job id u32
 *   [7..10] record count u32  [11..14] t f32 mm  [15] tool count 1..4  [16] CRC8
 *
 * TOOL_HDR (21 B):
 *   [0] magic 0xB9  [1] tool, the config tool's `id`  [2] seq  [3] head
 *   [4..19] xmin, xmax, ymin, ymax f32, work coordinates at the tip  [20] CRC8
 *
 * LAYER_HDR (20 B):
 *   [0]      magic 0xA1
 *   [1]      chamber  1..chamberCount; 0 for a tool without chambers
 *   [2]      seq
 *   [3..4]   layer    u16
 *   [5..8]   record   u32, the next BEZIER's number within the layer
 *   [9..12]  feed     f32 mm/s; 0 = the tool's cutFeed, then the machine's
 *   [13..16] p        f32 plunge depth below the material top, mm
 *   [17..18] t        u16 material thickness, µm
 *   [19]     CRC8 over bytes [0..18]
 *
 * Work Z 0 is the bed: the cut is at t − p, so a through cut is p = t plus
 * the overcut into the mat.
 */

import {
    JOB_HDR_SIZE, JOB_HDR_VERSION, LAYER_HDR_SEQ_OFFSET, LAYER_HDR_SIZE, MAGIC_JOB_HDR,
    MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, TOOL_HDR_SIZE,
} from "./constants.js";
import { crc8 } from "./crc.js";

export interface JobHdr {
    readonly id?: number;
    readonly recordCount: number;
    /** Material thickness, mm. */
    readonly thickness?: number;
    readonly toolCount: number;
}

/** Of the control points, work coordinates at the tip. */
export interface ToolBox {
    readonly minX: number;
    readonly minY: number;
    readonly maxX: number;
    readonly maxY: number;
}

export interface ToolHdr {
    readonly tool: number;
    readonly head: number;
    readonly bbox: ToolBox;
}

export interface LayerHdr {
    readonly chamber?: number;
    readonly layer?: number;
    readonly record?: number;
    readonly feed?: number;
    /** Plunge depth below the material top, mm; defaults to `thickness` (a through cut to the bed). */
    readonly p?: number;
    /** Material thickness, mm (to 65.535). */
    readonly thickness?: number;
}

export function packJobHdr(j: JobHdr, seq = 0): Uint8Array {
    const u8 = new Uint8Array(JOB_HDR_SIZE);
    const dv = new DataView(u8.buffer);
    dv.setUint8(0, MAGIC_JOB_HDR);
    dv.setUint8(1, JOB_HDR_VERSION);
    dv.setUint8(2, seq & 0xff);
    dv.setUint32(3, j.id ?? 0, true);
    dv.setUint32(7, j.recordCount, true);
    dv.setFloat32(11, j.thickness ?? 0, true);
    dv.setUint8(15, j.toolCount);
    u8[JOB_HDR_SIZE - 1] = crc8(u8, 0, JOB_HDR_SIZE - 1);
    return u8;
}

export function packToolHdr(t: ToolHdr, seq = 0): Uint8Array {
    const u8 = new Uint8Array(TOOL_HDR_SIZE);
    const dv = new DataView(u8.buffer);
    dv.setUint8(0, MAGIC_TOOL_HDR);
    dv.setUint8(1, t.tool & 0xff);
    dv.setUint8(2, seq & 0xff);
    dv.setUint8(3, t.head & 0xff);
    dv.setFloat32(4, t.bbox.minX, true);
    dv.setFloat32(8, t.bbox.maxX, true);
    dv.setFloat32(12, t.bbox.minY, true);
    dv.setFloat32(16, t.bbox.maxY, true);
    u8[TOOL_HDR_SIZE - 1] = crc8(u8, 0, TOOL_HDR_SIZE - 1);
    return u8;
}

export function packLayerHdr(l: LayerHdr, seq = 0): Uint8Array {
    const u8 = new Uint8Array(LAYER_HDR_SIZE);
    const dv = new DataView(u8.buffer);
    dv.setUint8(0, MAGIC_LAYER_HDR);
    dv.setUint8(1, (l.chamber ?? 0) & 0xff);
    dv.setUint8(LAYER_HDR_SEQ_OFFSET, seq & 0xff);
    dv.setUint16(3, l.layer ?? 0, true);
    dv.setUint32(5, l.record ?? 0, true);
    dv.setFloat32(9, l.feed ?? 0, true);
    dv.setFloat32(13, l.p ?? l.thickness ?? 0, true);
    const um = Math.round((l.thickness ?? 0) * 1000);
    if (!(um >= 0 && um <= 0xffff)) throw new RangeError(`LAYER_HDR: thickness ${l.thickness} mm out of range`);
    dv.setUint16(17, um, true);
    u8[LAYER_HDR_SIZE - 1] = crc8(u8, 0, LAYER_HDR_SIZE - 1);
    return u8;
}
