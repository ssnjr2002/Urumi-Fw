/**
 * Tests for wire/format/layer — the 22-byte LAYER_HDR.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";
import { MAGIC_JOB_HDR, MAGIC_LAYER_HDR, MAGIC_TOOL_HDR } from "../../../src/wire/format/constants.js";
import { packJobHdr, packLayerHdr, packToolHdr } from "../../../src/wire/format/layer.js";
import { stampSeq } from "../../../src/wire/format/packet.js";

describe("wire/format/layer: packLayerHdr", () => {
    it("lays out magic, chamber, seq, layer, record, feed, p, t and a trailing CRC", () => {
        const u8 = packLayerHdr({ chamber: 2, layer: 513, record: 70000, feed: 25.5, p: 0.5, thickness: 3.2 }, 9);
        const dv = new DataView(u8.buffer);
        expect(u8).toHaveLength(20);
        expect([u8[0], u8[1], u8[2]]).toEqual([MAGIC_LAYER_HDR, 2, 9]);
        expect(dv.getUint16(3, true)).toBe(513);
        expect(dv.getUint32(5, true)).toBe(70000);
        expect(dv.getFloat32(9, true)).toBe(25.5);
        expect(dv.getFloat32(13, true)).toBe(0.5);
        expect(dv.getUint16(17, true)).toBe(3200);
        expect(u8[19]).toBe(crc8(u8, 0, 19));
    });

    it("defaults: no chamber, layer and record 0, the tool's feed, a cut to the bed", () => {
        const dv = new DataView(packLayerHdr({}).buffer);
        expect(dv.getUint8(1)).toBe(0);
        expect(dv.getUint16(3, true)).toBe(0);
        expect(dv.getUint32(5, true)).toBe(0);
        expect(dv.getFloat32(9, true)).toBe(0);
        expect(dv.getFloat32(13, true)).toBe(0);
        expect(dv.getUint16(17, true)).toBe(0);
        const through = new DataView(packLayerHdr({ thickness: 2 }).buffer);
        expect(through.getFloat32(13, true)).toBe(2);
    });

    it("refuses a thickness that does not fit a u16 of µm", () => {
        expect(() => packLayerHdr({ thickness: 66 })).toThrow(RangeError);
        expect(() => packLayerHdr({ thickness: -1 })).toThrow(RangeError);
    });

    it("stampSeq restamps the seq and the CRC", () => {
        const u8 = stampSeq(packLayerHdr({}), 0x1ff);
        expect(u8[2]).toBe(0xff);
        expect(u8[19]).toBe(crc8(u8, 0, 19));
    });
});

describe("wire/format/layer: packJobHdr and packToolHdr", () => {
    it("lays out a job header", () => {
        const u8 = packJobHdr({ id: 7, recordCount: 70000, thickness: 1.5, toolCount: 2 }, 3);
        const dv = new DataView(u8.buffer);
        expect(u8).toHaveLength(17);
        expect([u8[0], u8[1], u8[2], u8[15]]).toEqual([MAGIC_JOB_HDR, 1, 3, 2]);
        expect(dv.getUint32(3, true)).toBe(7);
        expect(dv.getUint32(7, true)).toBe(70000);
        expect(dv.getFloat32(11, true)).toBe(1.5);
        expect(u8[16]).toBe(crc8(u8, 0, 16));
    });

    it("lays out a tool header, the bbox as xmin, xmax, ymin, ymax", () => {
        const u8 = packToolHdr({ tool: 4, head: 1, bbox: { minX: 1, minY: 2, maxX: 3, maxY: 4 } }, 5);
        const dv = new DataView(u8.buffer);
        expect(u8).toHaveLength(21);
        expect([u8[0], u8[1], u8[2], u8[3]]).toEqual([MAGIC_TOOL_HDR, 4, 5, 1]);
        expect([4, 8, 12, 16].map((o) => dv.getFloat32(o, true))).toEqual([1, 3, 2, 4]);
        expect(u8[20]).toBe(crc8(u8, 0, 20));
    });

    it("stampSeq takes both", () => {
        expect(stampSeq(packJobHdr({ recordCount: 0, toolCount: 1 }), 8)[2]).toBe(8);
        const box = { minX: 0, minY: 0, maxX: 0, maxY: 0 };
        expect(stampSeq(packToolHdr({ tool: 1, head: 0, bbox: box }), 9)[2]).toBe(9);
    });
});
