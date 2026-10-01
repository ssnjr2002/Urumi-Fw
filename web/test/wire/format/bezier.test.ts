/**
 * Tests for wire/format/bezier — the 56-byte BEZIER record.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";
import { MAGIC_BEZIER } from "../../../src/wire/format/constants.js";
import { packBezier } from "../../../src/wire/format/bezier.js";
import { packMicrosegment, stampSeq } from "../../../src/wire/format/packet.js";
import { microSegment } from "../../../src/wire/format/microsegment.js";
import { BezierFlag, type AnnotatedBezier } from "../../../src/toolpath/annotate.js";
import { cubic } from "../../../src/toolpath/geometry.js";

const piece: AnnotatedBezier = {
    curve: cubic({ x: 1, y: 2 }, { x: 3, y: 4 }, { x: 5, y: 6 }, { x: 7, y: 8 }),
    flags: BezierFlag.START | BezierFlag.END,
    length: 9,
    kappaMax: 0.5,
    dkappaMax: 0.25,
    ts: [0.1, 0.125, -0.0625],
    kappaStart: 0.3,
    kappaEnd: -0.3,
    fitError: 0,
};

describe("wire/format/bezier: packBezier", () => {
    it("lays out magic, flags, seq, the floats and a trailing CRC", () => {
        const u8 = packBezier(piece, 7);
        const dv = new DataView(u8.buffer);
        expect(u8).toHaveLength(56);
        expect([u8[0], u8[1], u8[2]]).toEqual([MAGIC_BEZIER, 5, 7]);
        const floats = Array.from({ length: 13 }, (_, i) => dv.getFloat32(3 + 4 * i, true));
        expect(floats).toEqual([1, 2, 3, 4, 5, 6, 7, 8, 9, 0.5, 0.25, 0.125, -0.0625]);
        expect(u8[55]).toBe(crc8(u8, 0, 55));
    });
});

describe("wire/format/packet: stampSeq by magic", () => {
    it.each([
        { name: "BEZIER at byte 2", pkt: packBezier(piece), at: 2 },
        { name: "MSEG at byte 22", pkt: packMicrosegment(microSegment(1, 0, 0, 0, 1000)), at: 22 },
    ])("$name", ({ pkt, at }) => {
        const out = stampSeq(pkt, 0x42);
        expect(out[at]).toBe(0x42);
        expect(out[out.length - 1]).toBe(crc8(out, 0, out.length - 1));
        expect(Array.from(out).filter((b, i) => b !== pkt[i]).length).toBeLessThanOrEqual(2);
    });

    it("refuses a packet it cannot frame", () => {
        expect(() => stampSeq(new Uint8Array(26), 1)).toThrow();
        expect(() => stampSeq(packBezier(piece).subarray(0, 26), 1)).toThrow();
    });
});
