/**
 * Tests for wire/format/cfg — the CFG_DATA 9-byte header pack/unpack, the
 * CFG NACK reason namespace, and the payload-size guard.
 *
 * The reference header bytes are the output of Python `struct.pack('<BII',
 * 0xB5, 12345, 0xDEADBEEF)` — captured from the Python, so a passing test
 * means the TS packer produces a byte-identical header and the parser reads
 * it back exactly as the demux will.
 */

import { describe, it, expect } from "vitest";
import {
    CFG_DATA_HDR_SIZE,
    MAX_CFG_PAYLOAD,
    CFG_NACK_CRC,
    CFG_NACK_TOO_BIG,
    CFG_NACK_BAD_STATE,
    CFG_NACK_FLASH,
    CFG_NACK_TIMEOUT,
    packCfgDataHeader,
    unpackCfgDataHeader,
} from "../../../src/wire/format/cfg.js";
import { MAGIC_CFG_DATA } from "../../../src/wire/format/constants.js";

// ── reference vector from Python struct.pack("<BII", 0xB5, 12345, 0xDEADBEEF) ──
const REF_HDR = new Uint8Array([181, 57, 48, 0, 0, 239, 190, 173, 222]);

describe("wire/format/cfg: CFG NACK reasons", () => {
    it("mirrors the firmware values (separate namespace from MSEG NACKs)", () => {
        expect(CFG_NACK_CRC).toBe(0x01);
        expect(CFG_NACK_TOO_BIG).toBe(0x02);
        expect(CFG_NACK_BAD_STATE).toBe(0x03);
        expect(CFG_NACK_FLASH).toBe(0x04);
        expect(CFG_NACK_TIMEOUT).toBe(0x05);
    });
});

describe("wire/format/cfg: sizes", () => {
    it("CFG_DATA header is 9 bytes", () => {
        expect(CFG_DATA_HDR_SIZE).toBe(9);
        expect(packCfgDataHeader(0, 0).length).toBe(9);
    });

    it("MAX_CFG_PAYLOAD bounds a corrupted length field at 64 KiB", () => {
        expect(MAX_CFG_PAYLOAD).toBe(64 * 1024);
    });
});

describe("wire/format/cfg: packCfgDataHeader", () => {
    it("is byte-identical to the Python reference vector", () => {
        const hdr = packCfgDataHeader(12345, 0xdeadbeef);
        expect([...hdr]).toEqual([...REF_HDR]);
    });

    it("writes length as u32 LE at [1..4]", () => {
        const hdr = packCfgDataHeader(1, 0);
        expect(hdr[1]).toBe(1);
        expect(hdr[2]).toBe(0);
        expect(hdr[3]).toBe(0);
        expect(hdr[4]).toBe(0);
    });

    it("writes crc32 as u32 LE at [5..8]", () => {
        const hdr = packCfgDataHeader(0, 0xffffffff);
        expect(hdr[5]).toBe(0xff);
        expect(hdr[6]).toBe(0xff);
        expect(hdr[7]).toBe(0xff);
        expect(hdr[8]).toBe(0xff);
    });

    it("stamps the magic as byte[0]", () => {
        expect(packCfgDataHeader(0, 0)[0]).toBe(MAGIC_CFG_DATA);
    });
});

describe("wire/format/cfg: unpackCfgDataHeader", () => {
    it("round-trips the Python reference vector exactly", () => {
        const h = unpackCfgDataHeader(REF_HDR);
        expect(h.length).toBe(12345);
        expect(h.crc32).toBe(0xdeadbeef);
    });

    it("round-trips length and crc32", () => {
        const hdr = packCfgDataHeader(0x12345678, 0xabcdef01);
        const h = unpackCfgDataHeader(hdr);
        expect(h.length).toBe(0x12345678);
        expect(h.crc32).toBe(0xabcdef01);
    });

    it("rejects a wrong-size buffer", () => {
        const short = new Uint8Array(4);
        short[0] = MAGIC_CFG_DATA;
        expect(() => unpackCfgDataHeader(short)).toThrow(/expected 9 bytes, got 4/);
    });

    it("rejects a bad magic", () => {
        const bad = packCfgDataHeader(0, 0);
        bad[0] = 0xcf;
        expect(() => unpackCfgDataHeader(bad)).toThrow(/bad magic 0xcf/);
    });

    it("preserves a 0 length (no stored config)", () => {
        const hdr = packCfgDataHeader(0, 0xdeadbeef);
        const h = unpackCfgDataHeader(hdr);
        expect(h.length).toBe(0);
        expect(h.crc32).toBe(0xdeadbeef);
    });
});