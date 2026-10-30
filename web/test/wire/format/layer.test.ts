/**
 * Tests for wire/format/layer — the 22-byte LAYER_HDR.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";
import { MAGIC_LAYER_HDR } from "../../../src/wire/format/constants.js";
import { packLayerHdr } from "../../../src/wire/format/layer.js";
import { stampSeq } from "../../../src/wire/format/packet.js";

describe("wire/format/layer: packLayerHdr", () => {
    it("lays out magic, chamber, seq, tool, head, layer, record, feed, p, t and a trailing CRC", () => {
        const u8 = packLayerHdr({ tool: 4, head: 1, chamber: 2, layer: 513, record: 70000, feed: 25.5, p: 0.5, thickness: 3.2 }, 9);
        const dv = new DataView(u8.buffer);
        expect(u8).toHaveLength(22);
        expect([u8[0], u8[1], u8[2], u8[3], u8[4]]).toEqual([MAGIC_LAYER_HDR, 2, 9, 4, 1]);
        expect(dv.getUint16(5, true)).toBe(513);
        expect(dv.getUint32(7, true)).toBe(70000);
        expect(dv.getFloat32(11, true)).toBe(25.5);
        expect(dv.getFloat32(15, true)).toBe(0.5);
        expect(dv.getUint16(19, true)).toBe(3200);
        expect(u8[21]).toBe(crc8(u8, 0, 21));
    });

    it("defaults: no chamber, layer and record 0, the tool's feed, a cut to the bed", () => {
        const dv = new DataView(packLayerHdr({ tool: 1, head: 0 }).buffer);
        expect(dv.getUint8(1)).toBe(0);
        expect(dv.getUint16(5, true)).toBe(0);
        expect(dv.getUint32(7, true)).toBe(0);
        expect(dv.getFloat32(11, true)).toBe(0);
        expect(dv.getFloat32(15, true)).toBe(0);
        expect(dv.getUint16(19, true)).toBe(0);
        const through = new DataView(packLayerHdr({ tool: 1, head: 0, thickness: 2 }).buffer);
        expect(through.getFloat32(15, true)).toBe(2);
    });

    it("refuses a thickness that does not fit a u16 of µm", () => {
        expect(() => packLayerHdr({ tool: 1, head: 0, thickness: 66 })).toThrow(RangeError);
        expect(() => packLayerHdr({ tool: 1, head: 0, thickness: -1 })).toThrow(RangeError);
    });

    it("stampSeq restamps the seq and the CRC", () => {
        const u8 = stampSeq(packLayerHdr({ tool: 1, head: 0 }), 0x1ff);
        expect(u8[2]).toBe(0xff);
        expect(u8[21]).toBe(crc8(u8, 0, 21));
    });
});
