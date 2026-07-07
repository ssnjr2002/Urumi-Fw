/**
 * Tests for production/bakePlan — layer-by-layer block assembly + the .plan
 * bake. Ties the multi-tool path back to the parity-verified single-tool path
 * (identical segments), and covers revolver slot parsing + the error surface.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { bakePlan, assembleBlocks } from "../bakePlan.js";
import { subpathsToPackets } from "./svgToPackets.js";
import { defaultConfig, KNIFE, ToolType } from "../../config/config.js";
import { loadSvgMmSubpaths, loadSvgMmLayers } from "../../svg/ingest.js";
import { savePlan, loadPlan } from "../../plan/src/planFile.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "..", "..", "..", "pipeline", "data");
const svg = (name: string) => readFileSync(join(DATA, name), "utf-8");

// inline layered SVGs (id doubles as the layer label — see ingest.layerLabel)
const tri = (x: number, y: number) =>
    `<path d="M${x},${y} L${x + 10},${y} L${x + 10},${y + 10} Z"/>`;
const wrap = (inner: string) =>
    `<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" ` +
    `width="100mm" height="100mm" viewBox="0 0 100 100">${inner}</svg>`;

describe("bakePlan: single-tool equivalence", () => {
    it("an unlayered SVG baked with defaultTool matches the parity path segments", () => {
        const config = defaultConfig();
        const text = svg("test_circle.svg");
        const { subpaths } = loadSvgMmSubpaths(text);
        const ref = subpathsToPackets(subpaths, config.machine, KNIFE, config.quality);

        const { plan } = bakePlan(config, text, { defaultTool: "knife" });
        expect(plan.blocks.length).toBe(1);
        expect(plan.blocks[0]!.profile.name).toBe("knife");
        expect(plan.blocks[0]!.segments).toEqual(ref);
    });

    it("the baked .plan bytes survive a load→save round-trip unchanged", () => {
        // byte-stability is the round-trip invariant: re-serialising a loaded
        // plan reproduces the file. (segment equality would trip over -0 vs 0,
        // which the int32 wire encoding collapses — same bytes either way.)
        const config = defaultConfig();
        const { bytes } = bakePlan(config, svg("test_circle.svg"), { defaultTool: "knife" });
        expect(savePlan(loadPlan(bytes))).toEqual(bytes);
    });
});

describe("bakePlan: multi-layer", () => {
    it("assembles one block per layer in document order", () => {
        const config = defaultConfig();
        const text = wrap(`<g id="knife">${tri(10, 10)}</g><g id="pen">${tri(30, 30)}</g>`);
        const { layers } = loadSvgMmLayers(text);
        const blocks = assembleBlocks(layers, config);
        expect(blocks.map((b) => b.profile.name)).toEqual(["knife", "pen"]);
        expect(blocks.every((b) => b.slot === undefined)).toBe(true);
    });
});

describe("bakePlan: revolver slots", () => {
    it("maps slotN sub-layers to 0-based slot indices (slot1→0, slot3→2)", () => {
        const config = defaultConfig();
        const text = wrap(
            `<g id="revolver_pen"><g id="slot1">${tri(10, 10)}</g><g id="slot3">${tri(30, 30)}</g></g>`,
        );
        const blocks = assembleBlocks(loadSvgMmLayers(text).layers, config);
        expect(blocks.length).toBe(2);
        expect(blocks.every((b) => b.profile.toolType === ToolType.REVOLVER_PEN)).toBe(true);
        expect(blocks.map((b) => b.slot)).toEqual([0, 2]);
    });

    it("round-trips slots through the .plan file", () => {
        const config = defaultConfig();
        const text = wrap(`<g id="revolver_pen"><g id="slot2">${tri(10, 10)}</g></g>`);
        const { bytes } = bakePlan(config, text);
        expect(loadPlan(bytes).blocks[0]!.slot).toBe(1);
    });
});

describe("bakePlan: error paths", () => {
    const config = defaultConfig();

    it("rejects an unlayered SVG with no defaultTool", () => {
        expect(() => bakePlan(config, svg("test_circle.svg"))).toThrow(/unlayered/);
    });

    it("rejects an unknown layer name", () => {
        const text = wrap(`<g id="sparkles">${tri(10, 10)}</g>`);
        expect(() => bakePlan(config, text)).toThrow(/no tool/);
    });

    it("rejects a revolver layer without slot sub-layers", () => {
        const text = wrap(`<g id="revolver_pen">${tri(10, 10)}</g>`);
        expect(() => bakePlan(config, text)).toThrow(/slot sub-layers/);
    });

    it("rejects an out-of-range slot", () => {
        const text = wrap(`<g id="revolver_pen"><g id="slot99">${tri(10, 10)}</g></g>`);
        expect(() => bakePlan(config, text)).toThrow(/out of range/);
    });
});
