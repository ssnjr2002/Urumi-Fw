/**
 * Tests for wire/format/layer — the 17-byte LAYER_HDR.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";
import { MAGIC_LAYER_HDR } from "../../../src/wire/format/constants.js";
import { packLayerHdr } from "../../../src/wire/format/layer.js";
import { stampSeq } from "../../../src/wire/format/packet.js";

describe("wire/format/layer: packLayerHdr", () => {
    it("lays out magic, chamber, seq, tool, index, feed, k and a trailing CRC", () => {
        const u8 = packLayerHdr({ tool: 4, chamber: 2, index: 70000, feed: 25.5, k: 0.5 }, 9);
        const dv = new DataView(u8.buffer);
        expect(u8).toHaveLength(17);
        expect([u8[0], u8[1], u8[2], u8[3]]).toEqual([MAGIC_LAYER_HDR, 2, 9, 4]);
        expect(dv.getUint32(4, true)).toBe(70000);
        expect(dv.getFloat32(8, true)).toBe(25.5);
        expect(dv.getFloat32(12, true)).toBe(0.5);
        expect(u8[16]).toBe(crc8(u8, 0, 16));
    });

    it("defaults: no chamber, index 0, the tool's feed, a full cut", () => {
        const dv = new DataView(packLayerHdr({ tool: 1 }).buffer);
        expect(dv.getUint8(1)).toBe(0);
        expect(dv.getUint32(4, true)).toBe(0);
        expect(dv.getFloat32(8, true)).toBe(0);
        expect(dv.getFloat32(12, true)).toBe(1);
    });

    it("stampSeq restamps the seq and the CRC", () => {
        const u8 = stampSeq(packLayerHdr({ tool: 1 }), 0x1ff);
        expect(u8[2]).toBe(0xff);
        expect(u8[16]).toBe(crc8(u8, 0, 16));
    });
});
