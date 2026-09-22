/**
 * Tests for wire/format/packet — CRC-8, 26-byte MicroSegment packer, framing.
 * Validated against Python host.protocol.packets (pack_microsegment,
 * _crc8, stamp_seq, write_stream) — the reference byte sequences below
 * are the actual Python output, so a passing test here means the TS
 * packer produces byte-identical packets.
 */

import { describe, it, expect } from "vitest";
import { crc8 } from "../../../src/wire/format/crc.js";
import { MAGIC_MICROSEG, PACKET_SIZE } from "../../../src/wire/format/constants.js";
import {
    packMicrosegment,
    serialiseMicrosegments,
    writeStream,
    decodePacket,
    FRAMED_PACKET_SIZE,
} from "../../../src/wire/format/packet.js";
import { microSegment, MICRO_JOG, MICRO_PATH_END } from "../../../src/wire/format/microsegment.js";

// ── reference vectors from Python host.protocol.packets ───────────────────────
// pack_microsegment(MS(dx=10, dy=-5, dz=1, da=-3, interval=1500, flags=0x11))
const REF_BYTES = [
    0xab, 0x0a, 0x00, 0x00, 0x00, 0xfb, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
    0xfd, 0xff, 0xff, 0xff, 0xdc, 0x05, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0xd3,
];
// stamp_seq(REF_BYTES, seq=42) — same packet with seq=42 (0x2A) and recomputed CRC
const REF_BYTES_SEQ42 = [
    0xab, 0x0a, 0x00, 0x00, 0x00, 0xfb, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
    0xfd, 0xff, 0xff, 0xff, 0xdc, 0x05, 0x00, 0x00, 0x11, 0x2a, 0x00, 0x00, 0x2d,
];

describe("wire/format/packet: crc8", () => {
    it("CRC-8 poly 0x8C over empty input is 0", () => {
        expect(crc8(new Uint8Array(0))).toBe(0);
    });

    it("CRC-8 over a single byte 0x00 is 0", () => {
        // 0x00 XOR 0x00 = 0x00; 8 shifts of 0 stay 0
        expect(crc8(new Uint8Array([0x00]))).toBe(0);
    });

    it("CRC-8 matches Python _crc8 on the REF_BYTES payload", () => {
        // Python: _crc8(REF_BYTES[:25]) == 0xD3
        const payload = new Uint8Array(REF_BYTES.slice(0, 25));
        expect(crc8(payload)).toBe(0xd3);
    });

    it("CRC-8 with start/end range matches full-buffer CRC", () => {
        const buf = new Uint8Array([0xff, 0xff, ...REF_BYTES.slice(0, 25), 0xff, 0xff]);
        const start = 2;
        const end = 2 + 25;
        expect(crc8(buf, start, end)).toBe(0xd3);
    });
});

describe("wire/format/packet: packMicrosegment", () => {
    it("produces a 26-byte packet", () => {
        const ms = microSegment(10, -5, 1, -3, 1500, 0x11);
        const pkt = packMicrosegment(ms);
        expect(pkt.length).toBe(PACKET_SIZE);
    });

    it("byte-identical to Python pack_microsegment output (seq=0)", () => {
        const ms = microSegment(10, -5, 1, -3, 1500, 0x11);
        const pkt = packMicrosegment(ms);
        expect([...pkt]).toEqual(REF_BYTES);
    });

    it("byte-identical to Python stamp_seq(pkt, 42) when seq=42", () => {
        const ms = microSegment(10, -5, 1, -3, 1500, 0x11);
        const pkt = packMicrosegment(ms, 42);
        expect([...pkt]).toEqual(REF_BYTES_SEQ42);
    });

    it("magic byte is 0xAB at offset 0", () => {
        const pkt = packMicrosegment(microSegment(0, 0, 0, 0, 100, 0));
        expect(pkt[0]).toBe(MAGIC_MICROSEG);
    });

    it("signed fields use two's-complement int32 LE", () => {
        // dx=-1 should be 0xFF FF FF FF
        const pkt = packMicrosegment(microSegment(-1, 0, 0, 0, 100, 0));
        expect(pkt[1]).toBe(0xff);
        expect(pkt[2]).toBe(0xff);
        expect(pkt[3]).toBe(0xff);
        expect(pkt[4]).toBe(0xff);
    });

    it("pad bytes [23..24] are zero", () => {
        const pkt = packMicrosegment(microSegment(10, -5, 1, -3, 1500, 0x11));
        expect(pkt[23]).toBe(0);
        expect(pkt[24]).toBe(0);
    });

    it("CRC at byte [25] validates the first 25 bytes", () => {
        const pkt = packMicrosegment(microSegment(10, -5, 1, -3, 1500, 0x11));
        expect(crc8(pkt, 0, 25)).toBe(pkt[25]);
    });

    it("flags are masked to uint8", () => {
        const pkt = packMicrosegment(microSegment(0, 0, 0, 0, 100, 0x10));
        expect(pkt[21]).toBe(0x10);
    });
});

describe("wire/format/packet: serialiseMicrosegments", () => {
    it("yields one 26-byte packet per MicroSegment", () => {
        const segs = [
            microSegment(1, 2, 0, 0, 100, 0),
            microSegment(-1, 0, 0, 1, 200, MICRO_PATH_END),
        ];
        const pkts = [...serialiseMicrosegments(segs)];
        expect(pkts.length).toBe(2);
        expect(pkts[0]!.length).toBe(PACKET_SIZE);
        expect(pkts[1]!.length).toBe(PACKET_SIZE);
        expect(pkts[1]![21]).toBe(MICRO_PATH_END);
    });

    it("works on an empty iterable", () => {
        expect([...serialiseMicrosegments([])]).toEqual([]);
    });
});

describe("wire/format/packet: writeStream (framing)", () => {
    it("frames a single packet: [u16 LE 26][26-byte packet]", () => {
        const pkt = packMicrosegment(microSegment(10, -5, 1, -3, 1500, 0x11));
        const framed = writeStream([pkt]);
        expect(framed.length).toBe(FRAMED_PACKET_SIZE);
        // length prefix u16 LE = 26 = 0x1A 0x00
        expect(framed[0]).toBe(0x1a);
        expect(framed[1]).toBe(0x00);
        // then the packet bytes verbatim
        expect([...framed.slice(2)]).toEqual([...pkt]);
    });

    it("frames N packets consecutively with no gaps", () => {
        const pkts = [
            packMicrosegment(microSegment(1, 0, 0, 0, 100, 0)),
            packMicrosegment(microSegment(0, 1, 0, 0, 100, 0)),
            packMicrosegment(microSegment(0, 0, 0, 0, 100, MICRO_PATH_END)),
        ];
        const framed = writeStream(pkts);
        expect(framed.length).toBe(3 * FRAMED_PACKET_SIZE);
        // each frame's length prefix should be 26
        for (let i = 0; i < 3; i++) {
            const off = i * FRAMED_PACKET_SIZE;
            expect(framed[off]).toBe(0x1a);
            expect(framed[off + 1]).toBe(0x00);
        }
    });

    it("empty packet list produces an empty Uint8Array", () => {
        expect(writeStream([]).length).toBe(0);
    });

    it("throws on a wrong-size packet", () => {
        const bad = new Uint8Array(10);
        expect(() => writeStream([bad])).toThrow(/expected 26-byte packet/);
    });
});

describe("wire/format/packet: decodePacket (parity diagnostics)", () => {
    it("roundtrips a packed packet field-by-field", () => {
        const ms = microSegment(10, -5, 1, -3, 1500, MICRO_JOG);
        const pkt = packMicrosegment(ms, 7);
        const d = decodePacket(pkt);
        expect(d.magic).toBe(MAGIC_MICROSEG);
        expect(d.dx).toBe(10);
        expect(d.dy).toBe(-5);
        expect(d.dz).toBe(1);
        expect(d.da).toBe(-3);
        expect(d.interval).toBe(1500);
        expect(d.flags).toBe(MICRO_JOG);
        expect(d.seq).toBe(7);
        expect(d.crcOk).toBe(true);
    });

    it("detects a corrupted byte via crcOk", () => {
        const pkt = packMicrosegment(microSegment(10, 0, 0, 0, 100, 0));
        pkt[5] = pkt[5]! ^ 0xff; // flip a bit in dy
        const d = decodePacket(pkt);
        expect(d.crcOk).toBe(false);
    });

    it("throws on a too-short slice", () => {
        const short = new Uint8Array(10);
        expect(() => decodePacket(short)).toThrow(/need 26 bytes/);
    });
});
