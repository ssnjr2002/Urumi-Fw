/**
 * bezier.ts — the 62-byte BEZIER record: one annotated cubic for the Pico's
 * planner (src/rp2350/core0/usb_protocol.h).
 *
 * Layout (little-endian, float32, machine mm):
 *   [0]      magic 0xAD
 *   [1]      flags    BezierFlag bits (START 1, BREAK 2, END 4)
 *   [2]      seq      stamped by the sender (stampSeq); 0 here
 *   [3..34]  p0-p3    x, y each
 *   [35..46] length, kappaMax, dkappaMax
 *   [47..54] c2, c3   t(s) coefficients; the Pico derives c1
 *   [55..56] nextBreakDist u16 mm (BezierRecord.nextBreakDist)
 *   [57..58] sweepMin i16 deg (BezierRecord.sweepMin)
 *   [59..60] sweepMax i16 deg
 *   [61]     CRC8 over bytes [0..60]
 */

import { BEZIER_SEQ_OFFSET, BEZIER_SIZE, MAGIC_BEZIER } from "./constants.js";
import { crc8 } from "./crc.js";

/** A point, mm. */
export interface Point {
    readonly x: number;
    readonly y: number;
}

/** What a BEZIER record carries: one cubic and its annotations. The toolpath's AnnotatedBezier is one. */
export interface BezierRecord {
    readonly curve: { readonly p0: Point; readonly p1: Point; readonly p2: Point; readonly p3: Point };
    /** BezierFlag bits: START 1, BREAK 2, END 4. */
    readonly flags: number;
    /** Arc length, mm. */
    readonly length: number;
    /** Largest |curvature|, 1/mm. */
    readonly kappaMax: number;
    /** Largest |curvature change| per mm. */
    readonly dkappaMax: number;
    /** t(s) = s·(c1 + s·(c2 + s·c3)). */
    readonly ts: readonly [number, number, number];
    /**
     * On a START or BREAK piece: mm from p0 to the next BREAK or the end of
     * the contour, rounded up, at most 65535 (at least that far). 0 elsewhere.
     */
    readonly nextBreakDist: number;
    /**
     * The unwrapped heading range from p0 to the next BREAK or the end of the
     * contour, whole degrees from p0's heading, rounded outward, i16.
     */
    readonly sweepMin: number;
    readonly sweepMax: number;
}

export function packBezier(b: BezierRecord, seq = 0): Uint8Array {
    const u8 = new Uint8Array(BEZIER_SIZE);
    const dv = new DataView(u8.buffer);
    dv.setUint8(0, MAGIC_BEZIER);
    dv.setUint8(1, b.flags & 0xff);
    dv.setUint8(BEZIER_SEQ_OFFSET, seq & 0xff);
    const { p0, p1, p2, p3 } = b.curve;
    const fields = [
        p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y,
        b.length, b.kappaMax, b.dkappaMax, b.ts[1], b.ts[2],
    ];
    fields.forEach((v, i) => dv.setFloat32(3 + 4 * i, v, true));
    dv.setUint16(55, b.nextBreakDist, true);
    dv.setInt16(57, b.sweepMin, true);
    dv.setInt16(59, b.sweepMax, true);
    u8[BEZIER_SIZE - 1] = crc8(u8, 0, BEZIER_SIZE - 1);
    return u8;
}
