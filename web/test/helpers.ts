/**
 * Shared test helpers. The vendored fixtures live in ./fixtures next to this
 * file, so the directory resolves with no upward path traversal — tests call
 * readFixture("foo.svg") instead of counting "../" segments to the repo root.
 */
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

/** Absolute path to the vendored fixtures directory (test/fixtures). */
export const FIXTURES = join(dirname(fileURLToPath(import.meta.url)), "fixtures");

/** Read a fixture as UTF-8 text (SVG, JSON, …). */
export function readFixture(name: string): string {
    return readFileSync(join(FIXTURES, name), "utf-8");
}

/** Read a fixture as raw bytes (.bin, …). */
export function readFixtureBytes(name: string): Uint8Array {
    return new Uint8Array(readFileSync(join(FIXTURES, name)));
}
