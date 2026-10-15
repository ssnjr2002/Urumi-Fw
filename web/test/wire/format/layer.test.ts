/**
 * Tests for wire/format/layer — the 19-byte LAYER_HDR.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";
import { MAGIC_LAYER_HDR } from "../../../src/wire/format/constants.js";
import { packLayerHdr } from "../../../src/wire/format/layer.js";
import { stampSeq } from "../../../src/wire/format/packet.js";

describe("wire/format/layer: packLayerHdr", () => {
    it("lays out magic, chamber, seq, tool, layer, record, feed, k and a trailing CRC", () => {
        const u8 = packLayerHdr({ tool: 4, chamber: 2, layer: 513, record: 70000, feed: 25.5, k: 0.5 }, 9);
        const dv = new DataView(u8.buffer);
        expect(u8).toHaveLength(19);
        expect([u8[0], u8[1], u8[2], u8[3]]).toEqual([MAGIC_LAYER_HDR, 2, 9, 4]);
        expect(dv.getUint16(4, true)).toBe(513);
        expect(dv.getUint32(6, true)).toBe(70000);
        expect(dv.getFloat32(10, true)).toBe(25.5);
        expect(dv.getFloat32(14, true)).toBe(0.5);
        expect(u8[18]).toBe(crc8(u8, 0, 18));
    });

    it("defaults: no chamber, layer and record 0, the tool's feed, a full cut", () => {
        const dv = new DataView(packLayerHdr({ tool: 1 }).buffer);
        expect(dv.getUint8(1)).toBe(0);
        expect(dv.getUint16(4, true)).toBe(0);
        expect(dv.getUint32(6, true)).toBe(0);
        expect(dv.getFloat32(10, true)).toBe(0);
        expect(dv.getFloat32(14, true)).toBe(1);
    });

    it("stampSeq restamps the seq and the CRC", () => {
        const u8 = stampSeq(packLayerHdr({ tool: 1 }), 0x1ff);
        expect(u8[2]).toBe(0xff);
        expect(u8[18]).toBe(crc8(u8, 0, 18));
    });
});
