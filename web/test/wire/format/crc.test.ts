/**
 * Tests for wire/format/crc — CRC-8 (poly 0x8C), against the Python reference.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";

// A 25-byte payload whose CRC-8 the Python _crc8 gave as 0xD3.
const PAYLOAD = [
    0xab, 0x0a, 0x00, 0x00, 0x00, 0xfb, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
    0xfd, 0xff, 0xff, 0xff, 0xdc, 0x05, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00,
];

describe("wire/format/crc: crc8", () => {
    it("is 0 over empty input", () => {
        expect(crc8(new Uint8Array(0))).toBe(0);
    });

    it("is 0 over a single 0x00", () => {
        expect(crc8(new Uint8Array([0x00]))).toBe(0);
    });

    it("matches Python _crc8", () => {
        expect(crc8(new Uint8Array(PAYLOAD))).toBe(0xd3);
    });

    it("over a start/end range matches the same bytes alone", () => {
        const buf = new Uint8Array([0xff, 0xff, ...PAYLOAD, 0xff, 0xff]);
        expect(crc8(buf, 2, 2 + PAYLOAD.length)).toBe(0xd3);
    });
});
