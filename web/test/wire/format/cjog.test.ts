/**
 * Tests for wire/format/cjog — the 7-byte continuous-jog packet.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";
import { CJOG_SIZE, MAGIC_CJOG } from "../../../src/wire/format/constants.js";
import { packCjog } from "../../../src/wire/format/cjog.js";

describe("wire/format/cjog", () => {
    it("lays out magic, directions, speed and CRC", () => {
        const p = packCjog(-1, 1, 0.25);
        expect(p.length).toBe(CJOG_SIZE);
        expect(Array.from(p.subarray(0, 6))).toEqual([MAGIC_CJOG, 0xff, 0x01, 0, 0, 16]);
        expect(p[6]).toBe(crc8(p, 0, 6));
    });

    it("1× speed is 64; the default is 1×", () => {
        expect(packCjog(1, 0)[5]).toBe(64);
        expect(packCjog(0, -1, 255 / 64)[5]).toBe(255);
    });

    it("refuses a speed the byte cannot carry", () => {
        expect(() => packCjog(1, 0, 0)).toThrow(RangeError);
        expect(() => packCjog(1, 0, 4)).toThrow(RangeError);
    });
});
