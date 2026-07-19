/**
 * snapshot.test.ts — golden-snapshot regression for the wire stream bytes.
 *
 * Bakes each fixture SVG through the REAL production pipeline (compileBlock +
 * the wire packer) and asserts byte-for-byte equality against a committed
 * golden .bin. The golden is generated FROM this pipeline — it is
 * self-referential, not pinned to an external (Python) reference. Its job is
 * "the output does not change unless we intend it to," not "matches Python."
 *
 * When a change intentionally alters the bytes, regenerate and REVIEW:
 *   UPDATE_GOLDEN=1 npx vitest run test/production/snapshot
 * then eyeball `git diff` on the .bin (or the decoded field diff this test
 * prints on mismatch) before committing the new golden.
 */

import { describe, it, expect } from "vitest";
import { readFileSync, writeFileSync, existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { compileBlock } from "../../src/production/compileBlock.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";
import {
    serialiseMicrosegments,
    writeStream,
    decodePacket,
    FRAMED_PACKET_SIZE,
    FRAME_PREFIX_SIZE,
} from "../../src/wire/packet.js";
import {
    KNIFE,
    qualityConfig,
} from "../../src/config/config.js";
import {
    defaultConfig,
} from "../../src/config/fixtures.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "data");
const UPDATE = process.env.UPDATE_GOLDEN === "1";

/** Bake an SVG through the living production stage chain → framed stream .bin. */
function bakeStreamBin(svgText: string): Uint8Array {
    const { subpaths } = loadSvgMmSubpaths(svgText);
    const { segments } = compileBlock(subpaths, defaultConfig().machine, qualityConfig(), KNIFE);
    return writeStream([...serialiseMicrosegments(segments)]);
}

/** First byte offset where two buffers differ, or -1 if identical. */
function firstDiffOffset(a: Uint8Array, b: Uint8Array): number {
    const len = Math.min(a.length, b.length);
    for (let i = 0; i < len; i++) if (a[i] !== b[i]) return i;
    return a.length === b.length ? -1 : len;
}

/** Decode the packet containing byte `off` and print a field-level diff. */
function diffPacketAtByte(bin: Uint8Array, golden: Uint8Array, byteOff: number): string {
    const pktIdx = Math.floor(byteOff / FRAMED_PACKET_SIZE);
    const pktStart = pktIdx * FRAMED_PACKET_SIZE + FRAME_PREFIX_SIZE;
    const lines = [`  first diff at byte ${byteOff} (packet #${pktIdx})`];
    if (pktStart + 26 <= bin.length && pktStart + 26 <= golden.length) {
        const a = decodePacket(bin, pktStart);
        const b = decodePacket(golden, pktStart);
        const fields: [string, number, number][] = [
            ["magic", a.magic, b.magic], ["dx", a.dx, b.dx], ["dy", a.dy, b.dy],
            ["dz", a.dz, b.dz], ["da", a.da, b.da], ["interval", a.interval, b.interval],
            ["flags", a.flags, b.flags], ["seq", a.seq, b.seq], ["crc", a.crc, b.crc],
        ];
        for (const [name, av, bv] of fields) {
            lines.push(`    ${av === bv ? "  " : "!!"} ${name.padEnd(9)} now=${av}  golden=${bv}`);
        }
    }
    return lines.join("\n");
}

function snapshotCase(svgName: string, goldenName: string): void {
    const bin = bakeStreamBin(readFileSync(join(DATA, svgName), "utf-8"));
    const goldenPath = join(DATA, goldenName);

    if (UPDATE || !existsSync(goldenPath)) {
        writeFileSync(goldenPath, bin);
        return; // regenerated — nothing to assert this run
    }

    const golden = new Uint8Array(readFileSync(goldenPath));
    // Write the current output beside the golden (gitignored) for manual diff.
    writeFileSync(join(DATA, goldenName.replace("_golden", "_current")), bin);

    const off = firstDiffOffset(bin, golden);
    if (bin.length !== golden.length || off >= 0) {
        const diag = off >= 0 ? diffPacketAtByte(bin, golden, off) : "(length differs before any byte diff)";
        expect.fail(
            `snapshot drift at offset ${off} — if intentional, re-run with ` +
            `UPDATE_GOLDEN=1 and review the diff:\n${diag}`,
        );
    }
    expect([...bin]).toEqual([...golden]);
}

describe("golden snapshot: wire stream bytes (real pipeline)", () => {
    it("test_circle.svg + KNIFE", () => {
        snapshotCase("test_circle.svg", "test_circle_knife_golden.bin");
    });
    it("fish.svg + KNIFE", () => {
        snapshotCase("fish.svg", "fish_knife_golden.bin");
    });
});
