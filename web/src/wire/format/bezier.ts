/**
 * bezier.ts — the 56-byte BEZIER record: one annotated cubic for the Pico's
 * planner (src/rp2350/core0/usb_protocol.h).
 *
 * Layout (little-endian, float32, machine mm):
 *   [0]      magic 0xAD
 *   [1]      flags    BezierFlag bits (START 1, BREAK 2, END 4)
 *   [2]      seq      stamped by the sender (stampSeq); 0 here
 *   [3..34]  p0-p3    x, y each
 *   [35..46] length, kappaMax, dkappaMax
 *   [47..54] c2, c3   t(s) coefficients; the Pico derives c1
 *   [55]     CRC8 over bytes [0..54]
 */

import type { AnnotatedBezier } from "../../toolpath/annotate.js";
import { BEZIER_SEQ_OFFSET, BEZIER_SIZE, MAGIC_BEZIER } from "./constants.js";
import { crc8 } from "./crc.js";

export function packBezier(b: AnnotatedBezier, seq = 0): Uint8Array {
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
    u8[BEZIER_SIZE - 1] = crc8(u8, 0, BEZIER_SIZE - 1);
    return u8;
}
