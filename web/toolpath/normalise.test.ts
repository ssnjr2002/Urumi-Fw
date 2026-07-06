/**
 * Tests for stage 2: SVG pixel coords -> mm + Y-axis flip.
 * Ported from host/production/test_normalise.py.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type { Pt } from "./bezier.js";
import { loadSvgMm, parseViewport } from "./normalise.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "..", "..", "pipeline", "data");

function svg(name: string): string {
    return readFileSync(join(DATA, name), "utf-8");
}

function approxPt(a: Pt, b: Pt, tol = 1e-3): boolean {
    return Math.abs(a.x - b.x) < tol && Math.abs(a.y - b.y) < tol;
}

describe("stage 2: normalise", () => {
    it("mm units — scale 2.0 (viewBox 100x100, 200mm canvas)", () => {
        const { viewport: vp } = loadSvgMm(svg("coord_mm_units.svg"));
        expect(vp.widthMm).toBe(200.0);
        expect(vp.heightMm).toBe(200.0);
        expect(vp.widthMm / vp.vbW).toBe(2.0);
    });

    it("mm units — SVG (10,10) -> (20, 180)mm after 2x scale + Y-flip", () => {
        const { curves } = loadSvgMm(svg("coord_mm_units.svg"));
        expect(approxPt(curves[0]!.p0, { x: 20, y: 180 })).toBe(true);
    });

    it("cm units — 10cm = 100mm, scale 1.0", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_cm_units.svg"));
        expect(Math.abs(vp.widthMm - 100.0)).toBeLessThan(0.01);
        expect(approxPt(curves[0]!.p0, { x: 10, y: 90 })).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 90, y: 10 })).toBe(true);
    });

    it("px units — 377px at 96dpi ≈ 99.748mm", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_px_units.svg"));
        expect(Math.abs(vp.widthMm - 99.748)).toBeLessThan(0.01);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 99.748 }, 0.01)).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 99.748, y: 0 }, 0.01)).toBe(true);
    });

    it("nonzero viewBox origin — (50,30) -> (0, 80)mm", () => {
        const { curves } = loadSvgMm(svg("coord_nonzero_origin.svg"));
        expect(approxPt(curves[0]!.p0, { x: 0, y: 80 })).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 100, y: 0 })).toBe(true);
    });

    it("nonsquare — viewBox 200x100 -> 100x50mm, scale 0.5", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_nonsquare.svg"));
        expect(Math.abs(vp.widthMm - 100.0)).toBeLessThan(0.01);
        expect(Math.abs(vp.heightMm - 50.0)).toBeLessThan(0.01);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 50 })).toBe(true);
        expect(approxPt(curves[2]!.p0, { x: 100, y: 0 })).toBe(true);
    });

    it("no size fallback — viewBox px == mm 1:1, canvas 100x60mm", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_no_size.svg"));
        expect(vp.widthMm).toBe(100.0);
        expect(vp.heightMm).toBe(60.0);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 60 })).toBe(true);
    });

    it("Y-flip top edge — SVG y=0 -> machine y = height_mm = 100", () => {
        const { curves } = loadSvgMm(svg("coord_yfliip_verify.svg"));
        const topEdge = curves[0]!;
        expect(approxPt(topEdge.p0, { x: 0, y: 100 })).toBe(true);
        expect(approxPt(topEdge.p3, { x: 100, y: 100 })).toBe(true);
    });

    it("Y-flip bottom edge — SVG y=100 -> machine y = 0", () => {
        const { curves } = loadSvgMm(svg("coord_yfliip_verify.svg"));
        const bottomEdge = curves[1]!;
        expect(approxPt(bottomEdge.p0, { x: 0, y: 0 })).toBe(true);
        expect(approxPt(bottomEdge.p3, { x: 100, y: 0 })).toBe(true);
    });

    it("Y-flip diagonal — (0,0) -> (0,100); (100,100) -> (100,0)", () => {
        const { curves } = loadSvgMm(svg("coord_yfliip_verify.svg"));
        const diag = curves[3]!;
        expect(approxPt(diag.p0, { x: 0, y: 100 })).toBe(true);
        expect(approxPt(diag.p3, { x: 100, y: 0 })).toBe(true);
    });

    it("parseViewport — returns named fields", () => {
        const vp = parseViewport(svg("coord_mm_units.svg"));
        expect(vp.vbMinX).toBe(0);
        expect(vp.vbMinY).toBe(0);
        expect(vp.vbW).toBe(100);
        expect(vp.vbH).toBe(100);
        expect(vp.widthMm).toBe(200);
        expect(vp.heightMm).toBe(200);
    });
});
