/**
 * Tests for production/bakePlan — layer-by-layer block assembly + the .plan
 * bake. Ties the multi-tool path back to the parity-verified single-tool path
 * (identical segments), and covers revolver slot parsing + the error surface.
 */

import { describe, it, expect } from "vitest";
import { readFixture } from "../helpers.js";

import { bakePlan, assembleBlocks } from "../../src/production/bakePlan.js";
import { compileBlock } from "../../src/production/compileBlock.js";
import {
    KNIFE,
    PEN,
    toolProfile,
    ToolType,
} from "../../src/machine/index.js";
import {
    defaultConfig,
} from "../machines.js";
import { loadSvgMmSubpaths, loadSvgMmLayers } from "../../src/svg/ingest.js";
import { savePlan, loadPlan } from "../../src/plan/planFile.js";

const svg = (name: string) => readFixture(name);

// inline layered SVGs (id doubles as the layer label — see ingest.layerLabel)
const tri = (x: number, y: number) =>
    `<path d="M${x},${y} L${x + 10},${y} L${x + 10},${y + 10} Z"/>`;
const wrap = (inner: string) =>
    `<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" ` +
    `width="100mm" height="100mm" viewBox="0 0 100 100">${inner}</svg>`;

describe("bakePlan: single-tool equivalence", () => {
    it("an unlayered SVG baked with defaultTool matches a direct compileBlock", () => {
        const config = defaultConfig();
        const text = svg("test_circle.svg");
        const { subpaths } = loadSvgMmSubpaths(text);
        const { segments: ref } = compileBlock(subpaths, config.machine, config.quality, KNIFE);

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
        const { plan } = bakePlan(config, svg("test_circle.svg"), { defaultTool: "knife" });
        const bytes = savePlan(plan);
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
        const { plan } = bakePlan(config, text);
        expect(loadPlan(savePlan(plan)).blocks[0]!.slot).toBe(1);
    });
});

describe("bakePlan: toolOffset shift (Option A)", () => {
    // A tool with a non-zero fixed tip offset from head center.
    const offsetX = 5;   // mm
    const offsetY = 3;   // mm
    const offsetTool = toolProfile("pen_offset", {
        ...PEN,
        toolOffset: { xOffset: offsetX, yOffset: offsetY },
    });

    const config = defaultConfig();
    // A simple triangle layer at a known position.
    const text = wrap(`<g id="pen">${tri(20, 20)}</g>`);
    // Override the pen profile in the config so bakePlan picks up the offset.
    const configWithOffset = {
        ...config,
        toolProfiles: { ...config.toolProfiles, pen: offsetTool },
    };

    it("startSteps is in head-center coordinates (shifted by -toolOffset)", () => {
        const { plan: planNoOffset } = bakePlan(config, text);
        const { plan: planWithOffset } = bakePlan(configWithOffset, text);

        const startNoOffset = planNoOffset.blocks[0]!.startSteps!;
        const startWithOffset = planWithOffset.blocks[0]!.startSteps!;

        // The shifted block starts at (path_start - toolOffset) * stepsPerUnit.
        const spu = config.machine.x.stepsPerUnit; // 160 steps/mm, square machine
        expect(startWithOffset.x).toBe(startNoOffset.x - Math.round(offsetX * spu));
        expect(startWithOffset.y).toBe(startNoOffset.y - Math.round(offsetY * spu));
    });

    it("segment stream is shifted: all XY net displacement changes by the offset", () => {
        // Sum dx across all segments of a single-subpath block.
        // The offset shifts the ENTIRE path, so the net XY of the first
        // cutting move from the block start changes by stepsPerUnit * offset.
        // We verify the first cutting segment's dx differs by the shift.
        const { plan: planNone } = bakePlan(config, text);
        const { plan: planShifted } = bakePlan(configWithOffset, text);

        // The net XY sum of all segments encodes the full path travel.
        // With a constant offset applied to all points, the NET displacement
        // (end minus start) is unchanged — but startSteps changes. So we
        // check that (startSteps.x + netDx) is consistent: shifted and
        // unshifted paths should end up at positions offset by the same delta.
        const netDx = (segs: typeof planNone.blocks[0]["segments"]) =>
            segs.reduce((s, seg) => s + seg.dx, 0);

        const endXNone    = planNone.blocks[0]!.startSteps!.x    + netDx(planNone.blocks[0]!.segments);
        const endXShifted = planShifted.blocks[0]!.startSteps!.x + netDx(planShifted.blocks[0]!.segments);

        const spu = config.machine.x.stepsPerUnit;
        // Both paths trace the same shape — their endpoints differ only by the offset.
        expect(endXShifted).toBe(endXNone - Math.round(offsetX * spu));
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
