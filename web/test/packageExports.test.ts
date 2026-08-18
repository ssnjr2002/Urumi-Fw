/**
 * packageExports.test.ts — the package boundary is part of the API.
 *
 * Everything else in this suite imports `../src/...` directly, so nothing
 * exercises what a real consumer sees: the `exports` map in package.json. That
 * map is a whitelist — a path missing from it is *unreachable* through the
 * package name, no matter that the file builds and ships. The two real-port
 * backends are documented in src/index.ts as "import them directly from
 * wire/link/backends/", and for a while that instruction was simply wrong.
 *
 * These checks run without a build (they map each export target back to its
 * source file); the dist half only asserts once `npm run build` has run, so a
 * clean checkout still passes `npm test`.
 */

import { describe, it, expect } from "vitest";
import { existsSync, readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");

type ExportEntry = string | { types?: string; import?: string };
const exportsMap = JSON.parse(readFileSync(resolve(root, "package.json"), "utf8"))
    .exports as Record<string, ExportEntry>;

/** Subpaths a consumer must be able to import. Add a case when you add one. */
const REQUIRED = [".", "./wire/link/backends/webserial", "./wire/link/backends/node"];

describe("package exports map", () => {
    it("exposes every subpath the docs tell consumers to import", () => {
        expect(Object.keys(exportsMap)).toEqual(expect.arrayContaining(REQUIRED));
    });

    for (const [subpath, entry] of Object.entries(exportsMap)) {
        if (typeof entry === "string") continue; // ./package.json passthrough

        it(`"${subpath}" points at a file that is actually built`, () => {
            const js = entry.import;
            const types = entry.types;
            expect(js, `${subpath} has no "import" condition`).toBeDefined();
            expect(types, `${subpath} has no "types" condition`).toBeDefined();

            // dist/foo/bar.js is emitted from src/foo/bar.ts. If that source is
            // gone, the map is stale and the build silently drops the subpath.
            const src = resolve(root, js!.replace(/^\.\/dist\//, "src/").replace(/\.js$/, ".ts"));
            expect(existsSync(src), `no source for ${subpath} (looked for ${src})`).toBe(true);

            // Only meaningful after `npm run build`; skipped on a clean tree.
            if (existsSync(resolve(root, "dist"))) {
                expect(existsSync(resolve(root, js!)), `${js} missing from dist`).toBe(true);
                expect(existsSync(resolve(root, types!)), `${types} missing from dist`).toBe(true);
            }
        });
    }

    it("does not re-export the real-port backends from the main barrel", () => {
        // The whole point of the subpaths: a Node consumer importing the barrel
        // must not pull in navigator.serial, and a browser one must not pull in
        // the optional `serialport` native module.
        const barrel = readFileSync(resolve(root, "src/index.ts"), "utf8");
        expect(barrel).not.toMatch(/backends\/(webserial|node)\.js/);
    });
});
