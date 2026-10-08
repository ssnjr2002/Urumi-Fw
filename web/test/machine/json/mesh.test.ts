/**
 * Tests for the /mesh.bin encoder: its layout and CRC, and each refusal.
 */

import { describe, it, expect } from "vitest";
import {
    encodeMesh, meshImageFromText, MESH_HEADER_BYTES, MESH_MAGIC, type MeshSource,
} from "../../../src/machine/json/mesh.js";
import { crc32 } from "../../../src/wire/format/crc.js";

const good = (): MeshSource => ({ x0: -5, y0: 10, dx: 600, dy: 400, z: [[0, -1, 2.5], [0.0004, -0.0006, 32.767]] });

describe("mesh: encodeMesh", () => {
    it("lays out the header, heights in µm and a CRC", () => {
        const b = encodeMesh(good());
        const v = new DataView(b.buffer);
        expect(b.length).toBe(MESH_HEADER_BYTES + 2 * 6 + 4);
        expect(v.getUint32(0, true)).toBe(MESH_MAGIC);
        expect([v.getUint16(4, true), v.getUint16(6, true), v.getUint16(8, true)]).toEqual([1, 3, 2]);
        expect([12, 16, 20, 24].map((o) => v.getFloat32(o, true))).toEqual([-5, 10, 600, 400]);
        const z = Array.from({ length: 6 }, (_, i) => v.getInt16(MESH_HEADER_BYTES + 2 * i, true));
        expect(z).toEqual([0, -1000, 2500, 0, -1, 32767]);
        expect(v.getUint32(b.length - 4, true)).toBe(crc32(b.subarray(0, b.length - 4)));
    });

    it("reads JSONC", () => {
        const text = `// bench\n{ "x0": -5, "y0": 10, "dx": 600, "dy": 400, /* rows */ "z": [[0, -1, 2.5], [0.0004, -0.0006, 32.767]] }`;
        expect(meshImageFromText(text)).toEqual(encodeMesh(good()));
    });

    const refusals: Array<{ name: string; edit: (m: MeshSource) => void; error: RegExp }> = [
        { name: "a missing origin", edit: (m) => { delete (m as Partial<MeshSource>).x0; }, error: /x0/ },
        { name: "zero spacing", edit: (m) => { m.dx = 0; }, error: /positive/ },
        { name: "one row", edit: (m) => { m.z = [[0, 1]]; }, error: /2 rows/ },
        { name: "one column", edit: (m) => { m.z = [[0], [1]]; }, error: /2 columns/ },
        { name: "a ragged row", edit: (m) => { m.z[1] = [0, 1]; }, error: /z\[1\]/ },
        { name: "a height out of int16 µm", edit: (m) => { m.z[0]![0] = 32.768; }, error: /z\[0\]\[0\]/ },
        { name: "too many points", edit: (m) => { m.z = Array.from({ length: 129 }, () => new Array(128).fill(0)); }, error: /16384/ },
    ];
    for (const r of refusals) {
        it(`refuses ${r.name}`, () => {
            const m = good();
            r.edit(m);
            expect(() => encodeMesh(m)).toThrow(r.error);
        });
    }
});
