/**
 * Tests for production/svgToPackets — the stage 3-8 chain glue.
 * Smoke test: verifies the chain runs end-to-end on a real SVG fixture
 * and produces well-formed packets (magic byte, CRC, framing).
 * Byte-for-byte parity with Python is in parity.test.ts.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { subpathsToPackets, bakeBin } from "../svgToPackets.js";
import { defaultConfig, KNIFE, qualityConfig } from "../../config/config.js";
import { loadSvgMmSubpaths } from "../../svg/ingest.js";
import {
    decodePacket,
    MAGIC_MICROSEG,
    FRAMED_PACKET_SIZE,
    FRAME_PREFIX_SIZE,
} from "../../wire/src/packet.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "..", "..", "..", "pipeline", "data");

function svg(name: string): string {
    return readFileSync(join(DATA, name), "utf-8");
}

describe("production: subpathsToPackets", () => {
    it("runs the full chain on test_circle.svg and emits MicroSegments", () => {
        const { subpaths } = loadSvgMmSubpaths(svg("test_circle.svg"));
        const { machine } = defaultConfig();
        const q = qualityConfig();
        const segs = subpathsToPackets(subpaths, machine, KNIFE, q);
        expect(segs.length).toBeGreaterThan(0);
        // every segment has integer step deltas
        for (const s of segs) {
            expect(Number.isInteger(s.dx)).toBe(true);
            expect(Number.isInteger(s.dy)).toBe(true);
            expect(Number.isInteger(s.dz)).toBe(true);
            expect(Number.isInteger(s.da)).toBe(true);
            expect(s.interval).toBeGreaterThan(0);
        }
    });

    it("emits at least one PATH_END flag (the path must terminate)", () => {
        const { subpaths } = loadSvgMmSubpaths(svg("test_circle.svg"));
        const { machine } = defaultConfig();
        const segs = subpathsToPackets(subpaths, machine, KNIFE, qualityConfig());
        const hasEnd = segs.some((s) => (s.flags & 0x01) !== 0);
        expect(hasEnd).toBe(true);
    });
});

describe("production: bakeBin", () => {
    it("produces a framed .bin: [u16 LE 26][26-byte packet] per packet", () => {
        const bin = bakeBin(svg("test_circle.svg"), defaultConfig().machine, KNIFE, qualityConfig());
        expect(bin.length).toBeGreaterThan(0);
        expect(bin.length % FRAMED_PACKET_SIZE).toBe(0);

        const nPackets = bin.length / FRAMED_PACKET_SIZE;
        expect(nPackets).toBeGreaterThan(0);

        // verify framing + first packet
        for (let i = 0; i < Math.min(3, nPackets); i++) {
            const off = i * FRAMED_PACKET_SIZE;
            // length prefix = 26 (LE)
            expect(bin[off]).toBe(0x1a);
            expect(bin[off + 1]).toBe(0x00);
            // magic byte
            expect(bin[off + 2]).toBe(MAGIC_MICROSEG);
        }
    });

    it("every packet has a valid CRC", () => {
        const bin = bakeBin(svg("test_circle.svg"), defaultConfig().machine, KNIFE, qualityConfig());
        const nPackets = bin.length / FRAMED_PACKET_SIZE;
        for (let i = 0; i < nPackets; i++) {
            const off = i * FRAMED_PACKET_SIZE + FRAME_PREFIX_SIZE;
            const d = decodePacket(bin, off);
            expect(d.crcOk).toBe(true);
            expect(d.magic).toBe(MAGIC_MICROSEG);
        }
    });

    it("packet count is non-zero and total size is a multiple of 28", () => {
        const bin = bakeBin(svg("test_circle.svg"), defaultConfig().machine, KNIFE, qualityConfig());
        const nPackets = bin.length / FRAMED_PACKET_SIZE;
        expect(nPackets).toBeGreaterThan(100);
        expect(bin.length).toBe(nPackets * FRAMED_PACKET_SIZE);
    });
});
