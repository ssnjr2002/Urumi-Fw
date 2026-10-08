/**
 * mesh.ts — a bed mesh file → the Pico's /mesh.bin, for flashing with
 * `pio run -e pico -t uploadfs` (scripts/pio/config_image.py).
 *
 * Source (JSON or JSONC), tip machine coordinates, mm:
 *   { "x0": 0, "y0": 0, "dx": 600, "dy": 400,
 *     "z": [[0, -1, -2], [-0.5, -1.5, -2.5]] }   // rows by y, columns by x
 *
 * Layout as src/rp2350/core0/ops/mesh.h: header, int16 µm heights row-major
 * (x fastest), CRC32 of everything before it.
 */

import { crc32 } from "../../wire/format/crc.js";
import { stripJsonComments } from "./image.js";

export const MESH_MAGIC = 0x4853454d;   // "MESH"
export const MESH_VERSION = 1;
export const MESH_HEADER_BYTES = 28;
export const MESH_MAX_POINTS = 16384;

export interface MeshSource {
    x0: number;
    y0: number;
    dx: number;
    dy: number;
    z: number[][];   // mm, + up
}

/** The file for `m`; throws naming the first thing wrong with it. */
export function encodeMesh(m: MeshSource): Uint8Array {
    for (const k of ["x0", "y0", "dx", "dy"] as const) {
        if (typeof m[k] !== "number" || !Number.isFinite(m[k])) throw new Error(`${k}: not a number`);
    }
    if (!(m.dx > 0) || !(m.dy > 0)) throw new Error("dx, dy: must be positive");
    if (!Array.isArray(m.z) || m.z.length < 2) throw new Error("z: needs at least 2 rows");
    const nx = Array.isArray(m.z[0]) ? m.z[0].length : 0;
    if (nx < 2) throw new Error("z: needs at least 2 columns");
    const ny = m.z.length;
    if (nx * ny > MESH_MAX_POINTS) throw new Error(`z: ${nx}x${ny} is over ${MESH_MAX_POINTS} points`);

    const bytes = new Uint8Array(MESH_HEADER_BYTES + 2 * nx * ny + 4);
    const v = new DataView(bytes.buffer);
    v.setUint32(0, MESH_MAGIC, true);
    v.setUint16(4, MESH_VERSION, true);
    v.setUint16(6, nx, true);
    v.setUint16(8, ny, true);
    v.setUint16(10, 0, true);
    v.setFloat32(12, m.x0, true);
    v.setFloat32(16, m.y0, true);
    v.setFloat32(20, m.dx, true);
    v.setFloat32(24, m.dy, true);
    m.z.forEach((row, j) => {
        if (!Array.isArray(row) || row.length !== nx) throw new Error(`z[${j}]: needs ${nx} values`);
        row.forEach((h, i) => {
            const um = Math.round(h * 1000);
            if (!Number.isFinite(h) || um < -32768 || um > 32767) {
                throw new Error(`z[${j}][${i}]: ${h} is outside ±32.767 mm`);
            }
            v.setInt16(MESH_HEADER_BYTES + 2 * (j * nx + i), um, true);
        });
    });
    v.setUint32(bytes.length - 4, crc32(bytes.subarray(0, bytes.length - 4)), true);
    return bytes;
}

/** JSON or JSONC text → the file. */
export function meshImageFromText(text: string): Uint8Array {
    return encodeMesh(JSON.parse(stripJsonComments(text)) as MeshSource);
}
