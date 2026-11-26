/**
 * Tests for packets/nack — the NACK and JOG_NACK records.
 */

import { describe, it, expect } from "vitest";
import { Nack, NACK_TEXT_MAX, Refusal, packJogNack, packNack, parseNack } from "../../../src/packets/nack.js";
import { MAGIC_JOG_NACK, MAGIC_NACK } from "../../../src/packets/constants.js";
import { crc8 } from "../../../src/packets/crc.js";

describe("packets/nack", () => {
    it("packs [magic, code, len, text, crc8] and parses it back", () => {
        const r = packNack(Nack.OUT_OF_RANGE, "y high, tool 1");
        expect([r[0], r[1], r[2]]).toEqual([MAGIC_NACK, Nack.OUT_OF_RANGE, 14]);
        expect(r[r.length - 1]).toBe(crc8(r, 0, r.length - 1));
        expect(parseNack(r)).toEqual(new Refusal(Nack.OUT_OF_RANGE, "y high, tool 1"));
    });

    it("a code with no text is five bytes, text empty", () => {
        const r = packNack(Nack.FULL);
        expect(r.length).toBe(4);
        expect(parseNack(r)).toEqual(new Refusal(Nack.FULL, ""));
    });

    it("a JOG_NACK is [magic, code] and parses with no text", () => {
        expect(Array.from(packJogNack(Nack.NOT_HOMED))).toEqual([MAGIC_JOG_NACK, Nack.NOT_HOMED]);
        expect(parseNack(packJogNack(Nack.NOT_HOMED))).toEqual(new Refusal(Nack.NOT_HOMED));
    });

    it("refuses a bad CRC8 or a length that doesn't match", () => {
        const r = packNack(Nack.ORDER, "no layer open");
        r[5] = r[5]! ^ 0xff;
        expect(() => parseNack(r)).toThrow(/CRC8/);
        expect(() => parseNack(packNack(Nack.ORDER, "abc").subarray(0, 6))).toThrow(/length/);
    });

    it("refuses text too long or not printable ASCII", () => {
        expect(() => packNack(Nack.BAD_FIELD, "x".repeat(NACK_TEXT_MAX + 1))).toThrow(RangeError);
        expect(() => packNack(Nack.BAD_FIELD, "é")).toThrow(RangeError);
        expect(packNack(Nack.BAD_FIELD, "x".repeat(NACK_TEXT_MAX)).length).toBe(NACK_TEXT_MAX + 4);
    });
});
