/**
 * Writes the Pico's /mesh.bin from a JSON or JSONC bed mesh.
 *
 *   pnpm --dir web exec vite-node scripts/mesh-image.ts -- <mesh> <out>
 *
 * Run by scripts/pio/config_image.py before `buildfs` / `uploadfs`.
 */

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { meshImageFromText } from "../src/machine/json/mesh.js";

const args = process.argv.slice(2).filter((a) => a !== "--");
if (args.length !== 2) {
    console.error("usage: mesh-image.ts <mesh.json|jsonc> <out.bin>");
    process.exit(2);
}
const [src, out] = args.map((a) => resolve(a)) as [string, string];

try {
    const image = meshImageFromText(readFileSync(src, "utf8"));
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, image);
    console.log(`${out}: ${image.length} bytes from ${src}`);
} catch (e) {
    console.error(`${src}: ${(e as Error).message}`);
    process.exit(1);
}
